# traffic-gen

An asynchronous network library for Linux built on `epoll`, plus a CLI
traffic generator that uses it. Written in C++17, single dependency: the
standard library and pthreads. Inspired by muduo.

The `traffic-gen` binary is a demo application on top of the library:
it can act as a TCP+UDP echo server, or spawn hundreds of concurrent
clients to load-test one. The library itself lives in `include/net/` and
`src/net/` and is the actually interesting part of the project.

## Architecture

Classic Reactor pattern.

- **`EventLoop`** — owns an `epoll` fd and an `eventfd` for cross-thread
  stop. Registers per-fd callbacks and dispatches epoll events to them.
  Edge-triggered (`EPOLLET`) throughout.
- **`Socket`** → **`TcpSocket`** / **`UdpSocket`** — thin RAII wrappers
  over kernel file descriptors. Non-copyable; the fd is closed in the
  destructor.
- **`Acceptor`** — sits on a listening `TcpSocket` and hands accepted
  fds to a user callback.
- **`TcpConnection`** — represents an established TCP connection.
  Owns its socket, its input/output buffers and four user callbacks:
  `connection`, `message`, `close`, `write_ready`. Manages its own
  lifetime via `std::enable_shared_from_this`: the reference held by
  the EventLoop callback keeps the connection alive as long as its fd
  is registered.
- **`TcpServer`** — glues an `Acceptor` with a map of live
  `TcpConnection`s.
- **`TcpClient`** — asynchronous client. An internal `Connector`
  performs the non-blocking `connect()` handshake against the event
  loop, then hands off the fd to a `TcpConnection` and calls
  `onConnect`.
- **`UdpServer`** / **`UdpClient`** — datagram counterparts, message
  callbacks only.
- **`ThreadPool`** — a simple task-queue thread pool. In `traffic-gen`
  the client mode uses it to run one `EventLoop` per worker thread.

## Requirements

- Linux (uses `epoll`, `eventfd`, `SOCK_NONBLOCK`).
- C++17 compiler.
- CMake >= 3.10.

## Build

```sh
cmake -S . -B build
cmake --build build
```

The binary is `build/traffic-gen`.

## Usage

### Server mode

Runs a TCP echo server and a UDP echo server on the same event loop.
Each incoming TCP message and each UDP datagram is echoed back
untouched.

```sh
build/traffic-gen server --tcp-port 5000 --udp-port 5001
```

Ctrl-C stops the loop cleanly.

### Client mode

Spawns N TCP clients and M UDP clients across a thread pool, each
worker running its own `EventLoop`. Every TCP client keeps its socket
saturated via `write_ready_callback`: as soon as the output buffer
drains, the next payload is queued. Every UDP client runs a ping-pong:
receive an echo, send the next packet.

```sh
build/traffic-gen client --host 127.0.0.1 \
                         --tcp-port 5000 --udp-port 5001 \
                         --tcp-clients 8 --udp-clients 4 \
                         --duration 10 --msg-size 1024
```

Options:

| flag              | default     | description                              |
|-------------------|-------------|------------------------------------------|
| `--host`          | `127.0.0.1` | target host                              |
| `--tcp-port`      | `5000`      | target TCP port                          |
| `--udp-port`      | `5001`      | target UDP port                          |
| `--tcp-clients`   | `8`         | concurrent TCP clients (0 disables TCP)  |
| `--udp-clients`   | `4`         | concurrent UDP clients (0 disables UDP)  |
| `--duration`      | `10`        | test duration, seconds                   |
| `--msg-size`      | `128`       | payload size, bytes                      |
| `--threads`       | auto        | worker threads (default: `hw_concurrency`)|

Sample output:

```
traffic-gen client -> 127.0.0.1  tcp:5000  udp:5001
  tcp_clients=8  udp_clients=4  threads=6  msg_size=1024  duration=3s

  [  1s] tx 1005.72 MB/s  rx 1005.51 MB/s  tcp_conn=8  tcp_err=0
  [  2s] tx  998.22 MB/s  rx  998.11 MB/s  tcp_conn=8  tcp_err=0
  [  3s] tx 1002.57 MB/s  rx 1002.66 MB/s  tcp_conn=8  tcp_err=0

---- results ----
  duration       : 3.01 s
  tcp connected  : 8 / 8 (err 0)
  tcp msgs tx/rx : 2968192 / 67980
  tcp data tx/rx : 2898.62 MB / 2898.38 MB
  tcp avg tx     : 964.36 MB/s
  udp pkts tx/rx : 110498 / 110496
  udp data tx/rx : 107.91 MB / 107.91 MB
  udp avg tx     : 35.90 MB/s
```

## Design notes

**Edge-triggered epoll.** Every fd is registered with `EPOLLET`, so
`handle_read` / `handle_write` loop until `EAGAIN` before returning
control to the event loop. Fewer wakeups, higher throughput.

**Write-ready callback for backpressure-free sending.** When a
`TcpConnection`'s output buffer drains, the loop stops watching for
`EPOLLOUT` on that fd and invokes `write_ready_callback` if set. The
callback is expected to enqueue the next payload via `conn->send()`,
which re-enables `EPOLLOUT`. This produces a self-clocking pipeline
that saturates the socket without any external timer or thread.

**Cross-thread stop, single-threaded I/O.** `EventLoop::stop()` may be
called from any thread — it writes to an internal `eventfd`, which
wakes `epoll_wait` and clears the running flag from within the loop
thread. Everything else on the loop (`add_fd`, `send`, callback
invocation) runs strictly on the loop thread; there is no cross-thread
task queue yet, so multi-threaded servers are built by sharding: one
`EventLoop` per worker thread, and clients partitioned across them.
That is what the `client` mode of `traffic-gen` does.

**Lifetime through shared_ptr.** `TcpConnection` is always held by a
`shared_ptr`. The lambda registered on `EventLoop` for its fd captures
`self = shared_from_this()`, so the connection stays alive as long as
its fd is watched. On close, `handle_close` takes its own guard before
touching anything, so the object is not destroyed mid-method by
callbacks that remove references.

## Roadmap

- `EventLoop::runInLoop` — real cross-thread task queue backed by
  `eventfd`, so `TcpConnection::send()` and friends become safe from
  any thread. The current single-thread-per-loop constraint would go
  away and multi-reactor server designs become possible.
- `EventLoopThreadPool` — canonical main-loop-plus-N-sub-loops
  configuration, so `TcpServer` can distribute new connections across
  sub-loops automatically.
- Unit and integration tests (GoogleTest).
- Timer support (`timerfd`) for scheduled callbacks.
