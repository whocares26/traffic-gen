#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <memory>
#include <cstdint>
#include <iomanip>
#include <random>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <cerrno>

#include "net/EventLoop.hpp"
#include "net/ThreadPool.hpp"
#include "net/TcpClient.hpp"
#include "net/UdpClient.hpp"
#include "net/InetAddress.hpp"

std::atomic<bool>     g_running{true};
std::atomic<uint64_t> g_bytesSent{0};
std::atomic<uint64_t> g_packetsSent{0};
std::atomic<uint64_t> g_errors{0};

void signalHandler(int) { g_running = false; }

struct Config {
    std::string host       = "127.0.0.1";
    int         port       = 8080;
    bool        tcp        = true;
    int         clients    = 4;
    int         size       = 4096;
    int         duration   = 0;
    bool        randomData = false;
};

static void printHelp(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "  --host  <ip>     Target IP          (default: 127.0.0.1)\n"
        << "  --port  <num>    Target port        (default: 8080)\n"
        << "  --tcp            Use TCP            (default)\n"
        << "  --udp            Use UDP\n"
        << "  --clients <num>  Parallel senders   (default: 4)\n"
        << "  --size  <bytes>  Packet size        (default: 4096)\n"
        << "  --duration <sec> Stop after N sec   (0 = run forever)\n"
        << "  --random         Random payload\n"
        << "  --help\n";
}

static bool parseArgs(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--help")                    { printHelp(argv[0]); return false; }
        else if (a == "--tcp")                     { cfg.tcp        = true;  }
        else if (a == "--udp")                     { cfg.tcp        = false; }
        else if (a == "--random")                  { cfg.randomData = true;  }
        else if (a == "--host"     && i+1 < argc)  { cfg.host       = argv[++i]; }
        else if (a == "--port"     && i+1 < argc)  { cfg.port       = std::stoi(argv[++i]); }
        else if (a == "--clients"  && i+1 < argc)  { cfg.clients    = std::stoi(argv[++i]); }
        else if (a == "--size"     && i+1 < argc)  { cfg.size       = std::stoi(argv[++i]); }
        else if (a == "--duration" && i+1 < argc)  { cfg.duration   = std::stoi(argv[++i]); }
        else { std::cerr << "Unknown arg: " << a << "\n"; return false; }
    }
    return true;
}

static std::string makePayload(int size, bool randomData) {
    std::string buf(size, '\xAB');
    if (randomData) {
        static thread_local std::mt19937 rng(std::random_device{}());
        static thread_local std::uniform_int_distribution<int> dist(0, 255);
        for (auto& c : buf) c = static_cast<char>(dist(rng));
    }
    return buf;
}

static void tcpSendLoop(net::EventLoop* loop, const Config& cfg, int id) {
    const std::string payload = makePayload(cfg.size, cfg.randomData);
    net::InetAddress addr(cfg.port, cfg.host);

    while (g_running) {
        std::shared_ptr<net::TcpConnection> sharedConn;
        int connFd = -1;
        std::mutex mtx;
        std::condition_variable cv;
        bool ready = false;

        net::TcpClient client(loop);
        client.connect(
            addr,
            [id, &sharedConn, &connFd, &mtx, &cv, &ready](const std::shared_ptr<net::TcpConnection>& conn) {
                std::cout << "[TCP:" << id << "] connected\n";
                std::lock_guard<std::mutex> lock(mtx);
                sharedConn = conn;
                connFd     = conn->fd();
                ready      = true;
                cv.notify_one();
            },
            [](const std::shared_ptr<net::TcpConnection>&, std::string&) {},
            [id](const std::shared_ptr<net::TcpConnection>&) {
                std::cout << "[TCP:" << id << "] disconnected\n";
            },
            [id, &mtx, &cv, &ready]() {
                std::cerr << "[TCP:" << id << "] error\n";
                g_errors++;
                std::lock_guard<std::mutex> lock(mtx);
                ready = true;
                cv.notify_one();
            }
        );

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&ready]{ return ready || !g_running; });
        }

        if (connFd < 0 || !g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        while (g_running && sharedConn->state() == net::TcpConnection::Connected) {
            ssize_t n = ::write(connFd, payload.data(), payload.size());
            if (n > 0) {
                g_bytesSent   += static_cast<uint64_t>(n);
                g_packetsSent += 1;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                g_errors++;
                break;
            }
        }

        client.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

static void udpSendLoop(net::EventLoop* loop, const Config& cfg, int id) {
    net::InetAddress addr(cfg.port, cfg.host);
    const std::string payload = makePayload(cfg.size, cfg.randomData);

    net::UdpClient client(loop);
    client.setServerAddr(addr);

    std::cout << "[UDP:" << id << "] sender started\n";

    while (g_running) {
        ssize_t sent = client.send(payload);
        if (sent > 0) {
            g_bytesSent   += static_cast<uint64_t>(sent);
            g_packetsSent += 1;
        } else {
            g_errors++;
        }
    }
}

static void statsPrinter() {
    uint64_t prevBytes = 0;
    uint64_t elapsed   = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++elapsed;

        uint64_t cur   = g_bytesSent.load();
        uint64_t delta = cur - prevBytes;
        prevBytes      = cur;

        std::cout
            << "\r[" << std::setw(4) << elapsed << "s] "
            << "sent "  << std::fixed << std::setprecision(1) << cur   / 1048576.0 << " MB  |  "
            << "rate "  << std::fixed << std::setprecision(1) << delta / 1048576.0 << " MB/s  |  "
            << "pkts "  << g_packetsSent.load() << "  |  "
            << "errs "  << g_errors.load()
            << "        " << std::flush;
    }
    std::cout << '\n';
}

int main(int argc, char* argv[]) {
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) return 1;

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    std::cout
        << "┌──────────────────────────────────────────┐\n"
        << "│          traffic-gen  stress test         │\n"
        << "└──────────────────────────────────────────┘\n"
        << "  target   : " << cfg.host << ":" << cfg.port << "\n"
        << "  protocol : " << (cfg.tcp ? "TCP" : "UDP") << "\n"
        << "  clients  : " << cfg.clients << "\n"
        << "  pkt size : " << cfg.size << " bytes\n"
        << "  duration : " << (cfg.duration ? std::to_string(cfg.duration) + "s" : "∞  (Ctrl+C to stop)") << "\n"
        << "──────────────────────────────────────────────\n";

    if (cfg.duration > 0) {
        std::thread([d = cfg.duration]() {
            std::this_thread::sleep_for(std::chrono::seconds(d));
            g_running = false;
        }).detach();
    }

    net::EventLoop loop;
    std::thread loopThread([&loop]{ loop.run(); });

    net::ThreadPool pool(cfg.clients);
    pool.start();

    for (int i = 0; i < cfg.clients; ++i) {
        if (cfg.tcp)
            pool.submit([&loop, &cfg, i]{ tcpSendLoop(&loop, cfg, i); });
        else
            pool.submit([&loop, &cfg, i]{ udpSendLoop(&loop, cfg, i); });
    }

    std::thread statsThread(statsPrinter);

    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    g_running = false;
    pool.stop();
    loop.stop();

    statsThread.join();
    loopThread.join();

    double totalMB = g_bytesSent.load() / 1048576.0;
    std::cout
        << "\n──────────────────────────────────────────────\n"
        << "  total sent : " << std::fixed << std::setprecision(2) << totalMB << " MB\n"
        << "  total pkts : " << g_packetsSent.load() << "\n"
        << "  errors     : " << g_errors.load() << "\n"
        << "  done.\n";

    return 0;
}