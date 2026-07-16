#include "net/EventLoop.hpp"
#include "net/InetAddress.hpp"
#include "net/TcpClient.hpp"
#include "net/TcpConnection.hpp"
#include "net/TcpServer.hpp"
#include "net/ThreadPool.hpp"
#include "net/UdpClient.hpp"
#include "net/UdpServer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Config {
    std::string mode;
    std::string host = "127.0.0.1";
    int  tcp_port     = 5000;
    int  udp_port     = 5001;
    int  tcp_clients  = 8;
    int  udp_clients  = 4;
    int  duration_sec = 10;
    int  msg_size     = 128;
    int  threads      = 0;
};

struct Stats {
    std::atomic<uint64_t> tcp_conn_ok    {0};
    std::atomic<uint64_t> tcp_conn_err   {0};
    std::atomic<uint64_t> tcp_msgs_sent  {0};
    std::atomic<uint64_t> tcp_msgs_recv  {0};
    std::atomic<uint64_t> tcp_bytes_sent {0};
    std::atomic<uint64_t> tcp_bytes_recv {0};
    std::atomic<uint64_t> udp_pkts_sent  {0};
    std::atomic<uint64_t> udp_pkts_recv  {0};
    std::atomic<uint64_t> udp_bytes_sent {0};
    std::atomic<uint64_t> udp_bytes_recv {0};
};

std::atomic<bool> g_interrupted{false};

void on_signal(int) { g_interrupted.store(true); }

void install_signals() {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
}

void print_usage(const char* prog) {
    std::cout <<
        "Usage:\n"
        "  " << prog << " server [--tcp-port N] [--udp-port N]\n"
        "  " << prog << " client [--host HOST] [--tcp-port N] [--udp-port N]\n"
        "                   [--tcp-clients N] [--udp-clients N]\n"
        "                   [--duration SEC] [--msg-size BYTES] [--threads N]\n"
        "\n"
        "Set --tcp-clients 0 or --udp-clients 0 to disable that protocol.\n";
}

bool parse_args(int argc, char** argv, Config& cfg) {
    if (argc < 2) return false;
    cfg.mode = argv[1];
    if (cfg.mode != "server" && cfg.mode != "client") return false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--host")        { auto v = need("--host");        if (!v) return false; cfg.host = v; }
        else if (a == "--tcp-port")    { auto v = need("--tcp-port");    if (!v) return false; cfg.tcp_port = std::atoi(v); }
        else if (a == "--udp-port")    { auto v = need("--udp-port");    if (!v) return false; cfg.udp_port = std::atoi(v); }
        else if (a == "--tcp-clients") { auto v = need("--tcp-clients"); if (!v) return false; cfg.tcp_clients = std::atoi(v); }
        else if (a == "--udp-clients") { auto v = need("--udp-clients"); if (!v) return false; cfg.udp_clients = std::atoi(v); }
        else if (a == "--duration")    { auto v = need("--duration");    if (!v) return false; cfg.duration_sec = std::atoi(v); }
        else if (a == "--msg-size")    { auto v = need("--msg-size");    if (!v) return false; cfg.msg_size = std::atoi(v); }
        else if (a == "--threads")     { auto v = need("--threads");     if (!v) return false; cfg.threads = std::atoi(v); }
        else if (a == "-h" || a == "--help") return false;
        else { std::cerr << "unknown arg: " << a << "\n"; return false; }
    }
    return true;
}

int run_server(const Config& cfg) {
    net::EventLoop loop;

    std::unique_ptr<net::TcpServer> tcp;
    std::unique_ptr<net::UdpServer> udp;

    if (cfg.tcp_port > 0) {
        net::InetAddress addr(static_cast<uint16_t>(cfg.tcp_port));
        tcp = std::make_unique<net::TcpServer>(&loop, addr);
        tcp->set_connection_callback(
            [](const std::shared_ptr<net::TcpConnection>& conn) {
                const char* sign =
                    (conn->state() == net::TcpConnection::Connected) ? "+" : "-";
                std::cout << "[tcp] " << sign << ' '
                          << conn->peer_address() << ':' << conn->peer_port() << '\n';
            });
        tcp->set_message_callback(
            [](const std::shared_ptr<net::TcpConnection>& conn, std::string& buf) {
                conn->send(buf);
                buf.clear();
            });
        tcp->start();
        std::cout << "[tcp] echo listening on :" << cfg.tcp_port << '\n';
    }

    if (cfg.udp_port > 0) {
        net::InetAddress addr(static_cast<uint16_t>(cfg.udp_port));
        udp = std::make_unique<net::UdpServer>(&loop, addr);
        net::UdpServer* raw = udp.get();
        udp->set_message_callback(
            [raw](const std::string& data, const net::InetAddress& from) {
                raw->sendTo(data, from);
            });
        udp->start();
        std::cout << "[udp] echo listening on :" << cfg.udp_port << '\n';
    }

    install_signals();

    std::thread watcher([&loop] {
        while (!g_interrupted.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        loop.stop();
    });

    std::cout << "server running (Ctrl-C to stop)\n";
    loop.run();
    watcher.join();
    std::cout << "\nserver stopped\n";
    return 0;
}

int run_client(const Config& cfg) {
    if (cfg.tcp_clients < 0 || cfg.udp_clients < 0) {
        std::cerr << "clients must be >= 0\n"; return 1;
    }
    if (cfg.tcp_clients == 0 && cfg.udp_clients == 0) {
        std::cerr << "need at least one of --tcp-clients / --udp-clients > 0\n"; return 1;
    }
    if (cfg.duration_sec <= 0) {
        std::cerr << "--duration must be > 0\n"; return 1;
    }
    if (cfg.msg_size <= 0 || cfg.msg_size > 60000) {
        std::cerr << "--msg-size must be in (0, 60000]\n"; return 1;
    }

    Stats stats;
    const std::string payload(static_cast<size_t>(cfg.msg_size), 'X');

    const int total_clients = cfg.tcp_clients + cfg.udp_clients;
    int threads = cfg.threads > 0
                    ? cfg.threads
                    : static_cast<int>(std::thread::hardware_concurrency());
    if (threads <= 0) threads = 1;
    threads = std::min(threads, total_clients);

    auto share = [](int total, int workers, int idx) {
        int base = total / workers;
        int rem  = total % workers;
        return base + (idx < rem ? 1 : 0);
    };

    net::InetAddress tcp_addr(static_cast<uint16_t>(cfg.tcp_port), cfg.host);
    net::InetAddress udp_addr(static_cast<uint16_t>(cfg.udp_port), cfg.host);

    std::mutex                       loops_mtx;
    std::vector<net::EventLoop*>     worker_loops(threads, nullptr);
    std::atomic<int>                 ready{0};

    net::ThreadPool pool(threads);
    pool.start();

    for (int w = 0; w < threads; ++w) {
        const int my_tcp = share(cfg.tcp_clients, threads, w);
        const int my_udp = share(cfg.udp_clients, threads, w);

        pool.submit([&, w, my_tcp, my_udp] {
            net::EventLoop loop;

            std::vector<std::unique_ptr<net::TcpClient>> tcps;
            tcps.reserve(my_tcp);

            for (int i = 0; i < my_tcp; ++i) {
                auto c = std::make_unique<net::TcpClient>(&loop);
                c->connect(
                    tcp_addr,
                    // onConnect — включаем «конвейер»: как только буфер опустеет,
                    // write_ready_callback положит следующий пакет.
                    [&](const std::shared_ptr<net::TcpConnection>& conn) {
                        stats.tcp_conn_ok.fetch_add(1, std::memory_order_relaxed);
                        conn->set_write_ready_callback(
                            [&](const std::shared_ptr<net::TcpConnection>& c) {
                                c->send(payload);
                                stats.tcp_msgs_sent.fetch_add(1, std::memory_order_relaxed);
                                stats.tcp_bytes_sent.fetch_add(payload.size(), std::memory_order_relaxed);
                            });
                        conn->send(payload);
                        stats.tcp_msgs_sent.fetch_add(1, std::memory_order_relaxed);
                        stats.tcp_bytes_sent.fetch_add(payload.size(), std::memory_order_relaxed);
                    },
                    [&](const std::shared_ptr<net::TcpConnection>&, std::string& buf) {
                        stats.tcp_msgs_recv.fetch_add(1, std::memory_order_relaxed);
                        stats.tcp_bytes_recv.fetch_add(buf.size(), std::memory_order_relaxed);
                        buf.clear();
                    },
                    [](const std::shared_ptr<net::TcpConnection>&) {},
                    [&] { stats.tcp_conn_err.fetch_add(1, std::memory_order_relaxed); });
                tcps.push_back(std::move(c));
            }

            std::vector<std::unique_ptr<net::UdpClient>> udps;
            udps.reserve(my_udp);

            for (int i = 0; i < my_udp; ++i) {
                auto c = std::make_unique<net::UdpClient>(&loop);
                c->setServerAddr(udp_addr);
                net::UdpClient* raw = c.get();
                // UDP ping-pong: получили ответ → шлём следующий пакет.
                c->setMessageCallback(
                    [&, raw](const std::string& data, const net::InetAddress&) {
                        stats.udp_pkts_recv.fetch_add(1, std::memory_order_relaxed);
                        stats.udp_bytes_recv.fetch_add(data.size(), std::memory_order_relaxed);
                        raw->send(payload);
                        stats.udp_pkts_sent.fetch_add(1, std::memory_order_relaxed);
                        stats.udp_bytes_sent.fetch_add(payload.size(), std::memory_order_relaxed);
                    });
                c->startReading();
                c->send(payload);
                stats.udp_pkts_sent.fetch_add(1, std::memory_order_relaxed);
                stats.udp_bytes_sent.fetch_add(payload.size(), std::memory_order_relaxed);
                udps.push_back(std::move(c));
            }

            {
                std::lock_guard<std::mutex> lk(loops_mtx);
                worker_loops[w] = &loop;
            }
            ready.fetch_add(1);

            loop.run();

            {
                std::lock_guard<std::mutex> lk(loops_mtx);
                worker_loops[w] = nullptr;
            }
        });
    }

    while (ready.load() < threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    install_signals();

    std::cout << "traffic-gen client -> " << cfg.host
              << "  tcp:" << cfg.tcp_port
              << "  udp:" << cfg.udp_port << '\n'
              << "  tcp_clients=" << cfg.tcp_clients
              << "  udp_clients=" << cfg.udp_clients
              << "  threads="     << threads
              << "  msg_size="    << cfg.msg_size
              << "  duration="    << cfg.duration_sec << "s\n\n";

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t prev_tx = 0, prev_rx = 0;
    for (int s = 0; s < cfg.duration_sec && !g_interrupted.load(); ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const uint64_t tx = stats.tcp_bytes_sent.load() + stats.udp_bytes_sent.load();
        const uint64_t rx = stats.tcp_bytes_recv.load() + stats.udp_bytes_recv.load();
        const double txr = (tx - prev_tx) / (1024.0 * 1024.0);
        const double rxr = (rx - prev_rx) / (1024.0 * 1024.0);
        std::cout << "  [" << std::setw(3) << (s + 1) << "s] "
                  << "tx " << std::fixed << std::setprecision(2) << std::setw(7) << txr
                  << " MB/s  rx " << std::setw(7) << rxr << " MB/s"
                  << "  tcp_conn=" << stats.tcp_conn_ok.load()
                  << "  tcp_err=" << stats.tcp_conn_err.load()
                  << '\n';
        prev_tx = tx;
        prev_rx = rx;
    }

    {
        std::lock_guard<std::mutex> lk(loops_mtx);
        for (auto* p : worker_loops) if (p) p->stop();
    }
    pool.stop();

    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();

    auto mb = [](uint64_t bytes) { return bytes / (1024.0 * 1024.0); };
    auto rate = [&](uint64_t bytes) { return sec > 0 ? mb(bytes) / sec : 0.0; };

    std::cout << "\n---- results ----\n"
              << std::fixed << std::setprecision(2)
              << "  duration       : " << sec << " s\n"
              << "  tcp connected  : " << stats.tcp_conn_ok.load() << " / "
                                       << cfg.tcp_clients
                                       << " (err " << stats.tcp_conn_err.load() << ")\n"
              << "  tcp msgs tx/rx : " << stats.tcp_msgs_sent.load()
                                       << " / " << stats.tcp_msgs_recv.load() << '\n'
              << "  tcp data tx/rx : " << mb(stats.tcp_bytes_sent.load()) << " MB"
                                       << " / " << mb(stats.tcp_bytes_recv.load()) << " MB\n"
              << "  tcp avg tx     : " << rate(stats.tcp_bytes_sent.load()) << " MB/s\n"
              << "  udp pkts tx/rx : " << stats.udp_pkts_sent.load()
                                       << " / " << stats.udp_pkts_recv.load() << '\n'
              << "  udp data tx/rx : " << mb(stats.udp_bytes_sent.load()) << " MB"
                                       << " / " << mb(stats.udp_bytes_recv.load()) << " MB\n"
              << "  udp avg tx     : " << rate(stats.udp_bytes_sent.load()) << " MB/s\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) {
        print_usage(argc > 0 ? argv[0] : "traffic-gen");
        return 1;
    }
    try {
        return cfg.mode == "server" ? run_server(cfg) : run_client(cfg);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 2;
    }
}
