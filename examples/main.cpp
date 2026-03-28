#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <random>

#include "net/EventLoop.hpp"
#include "net/TcpClient.hpp"
#include "net/UdpClient.hpp"
#include "net/InetAddress.hpp"

// ─── globals ────────────────────────────────────────────────────────────────

std::atomic<bool>     g_running{true};
std::atomic<uint64_t> g_bytesSent{0};
std::atomic<uint64_t> g_packetsSent{0};
std::atomic<uint64_t> g_errors{0};

void signalHandler(int) {
    g_running = false;
}

// ─── config ─────────────────────────────────────────────────────────────────

struct Config {
    std::string host    = "127.0.0.1";
    int         port    = 8080;
    bool        tcp     = true;
    int         clients = 4;
    int         size    = 4096;       // bytes per packet
    int         duration = 0;         // 0 = run until Ctrl+C
    bool        randomPayload = false;
};

static void printHelp(const char* prog) {
    std::cout <<
        "Usage: " << prog << " [options]\n"
        "\n"
        "  --host  <ip>     Target IP address  (default: 127.0.0.1)\n"
        "  --port  <num>    Target port        (default: 8080)\n"
        "  --tcp            Use TCP            (default)\n"
        "  --udp            Use UDP\n"
        "  --clients <num>  Parallel senders   (default: 4)\n"
        "  --size  <bytes>  Packet size        (default: 4096)\n"
        "  --duration <sec> Stop after N sec   (default: run forever)\n"
        "  --random         Use random payload instead of zeroes\n"
        "  --help           Show this message\n"
        "\n"
        "Examples:\n"
        "  " << prog << " --host 192.168.1.100 --port 9000 --tcp --clients 8\n"
        "  " << prog << " --host 192.168.1.100 --port 9000 --udp --size 1400 --duration 30\n";
}

static bool parseArgs(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if      (a == "--help")     { printHelp(argv[0]); return false; }
        else if (a == "--tcp")      { cfg.tcp = true; }
        else if (a == "--udp")      { cfg.tcp = false; }
        else if (a == "--random")   { cfg.randomPayload = true; }
        else if (a == "--host"    && i+1 < argc) { cfg.host     = argv[++i]; }
        else if (a == "--port"    && i+1 < argc) { cfg.port     = std::stoi(argv[++i]); }
        else if (a == "--clients" && i+1 < argc) { cfg.clients  = std::stoi(argv[++i]); }
        else if (a == "--size"    && i+1 < argc) { cfg.size     = std::stoi(argv[++i]); }
        else if (a == "--duration"&& i+1 < argc) { cfg.duration = std::stoi(argv[++i]); }
        else {
            std::cerr << "Unknown argument: " << a << "\n";
            return false;
        }
    }
    return true;
}

// ─── payload builder ─────────────────────────────────────────────────────────

static std::string makePayload(int size, bool random) {
    std::string buf(size, '\0');
    if (random) {
        static thread_local std::mt19937 rng(std::random_device{}());
        static thread_local std::uniform_int_distribution<int> dist(0, 255);
        for (auto& c : buf) c = static_cast<char>(dist(rng));
    }
    return buf;
}

// ─── TCP sender ──────────────────────────────────────────────────────────────

static void tcpSendLoop(const std::shared_ptr<net::TcpConnection>& conn,
                        const Config& cfg)
{
    const std::string payload = makePayload(cfg.size, cfg.randomPayload);

    while (g_running && conn->state() == net::TcpConnection::Connected) {
        conn->send(payload);
        g_bytesSent   += payload.size();
        g_packetsSent += 1;
    }
}

static void runTcp(net::EventLoop* loop, const Config& cfg) {
    net::InetAddress serverAddr(cfg.port, cfg.host);

    std::vector<std::unique_ptr<net::TcpClient>> clients;
    clients.reserve(cfg.clients);

    for (int i = 0; i < cfg.clients; ++i) {
        auto client = std::make_unique<net::TcpClient>(loop);

        client->connect(
            serverAddr,
            // onConnect
            [i, &cfg](const std::shared_ptr<net::TcpConnection>& conn) {
                std::cout << "[TCP:" << i << "] connected\n";
                std::thread([conn, &cfg]() {
                    tcpSendLoop(conn, cfg);
                }).detach();
            },
            // onMessage  (echo / responses — we don't care, just discard)
            [](const std::shared_ptr<net::TcpConnection>&, std::string&) {},
            // onDisconnect
            [i](const std::shared_ptr<net::TcpConnection>&) {
                std::cout << "[TCP:" << i << "] disconnected\n";
            },
            // onError
            [i]() {
                std::cerr << "[TCP:" << i << "] connection error\n";
                g_errors++;
            }
        );

        clients.push_back(std::move(client));
    }

    // wait until done
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (auto& c : clients) c->cancel();
}

// ─── UDP sender ──────────────────────────────────────────────────────────────

static void runUdp(net::EventLoop* loop, const Config& cfg) {
    net::InetAddress serverAddr(cfg.port, cfg.host);
    const std::string payload = makePayload(cfg.size, cfg.randomPayload);

    std::vector<std::unique_ptr<net::UdpClient>> clients;
    clients.reserve(cfg.clients);

    for (int i = 0; i < cfg.clients; ++i) {
        auto client = std::make_unique<net::UdpClient>(loop);
        client->setServerAddr(serverAddr);
        clients.push_back(std::move(client));
    }

    // Each client gets its own send thread
    std::vector<std::thread> threads;
    threads.reserve(cfg.clients);

    for (int i = 0; i < cfg.clients; ++i) {
        threads.emplace_back([i, &clients, &payload]() {
            auto& client = clients[i];
            while (g_running) {
                ssize_t sent = client->send(payload);
                if (sent > 0) {
                    g_bytesSent   += static_cast<uint64_t>(sent);
                    g_packetsSent += 1;
                } else {
                    g_errors++;
                }
            }
        });
    }

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (auto& t : threads) t.join();
}

// ─── stats printer ───────────────────────────────────────────────────────────

static void statsPrinter() {
    uint64_t prevBytes = 0;
    uint64_t elapsed   = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        ++elapsed;

        uint64_t curBytes = g_bytesSent.load();
        uint64_t delta    = curBytes - prevBytes;
        prevBytes         = curBytes;

        double totalMB  = curBytes / 1048576.0;
        double rateMBps = delta    / 1048576.0;

        std::cout
            << "\r[" << std::setw(4) << elapsed << "s] "
            << "sent "  << std::fixed << std::setprecision(1) << totalMB  << " MB  |  "
            << "rate "  << std::fixed << std::setprecision(1) << rateMBps << " MB/s  |  "
            << "pkts "  << g_packetsSent.load() << "  |  "
            << "errs "  << g_errors.load()
            << "        "
            << std::flush;
    }
    std::cout << '\n';
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) return 1;

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout
        << "┌─────────────────────────────────────────┐\n"
        << "│          traffic-gen  stress test        │\n"
        << "└─────────────────────────────────────────┘\n"
        << "  target   : " << cfg.host << ":" << cfg.port << "\n"
        << "  protocol : " << (cfg.tcp ? "TCP" : "UDP") << "\n"
        << "  clients  : " << cfg.clients << "\n"
        << "  pkt size : " << cfg.size << " bytes\n"
        << "  duration : " << (cfg.duration ? std::to_string(cfg.duration) + "s" : "∞") << "\n"
        << "  Ctrl+C to stop\n"
        << "─────────────────────────────────────────────\n";

    // optional duration watchdog
    if (cfg.duration > 0) {
        std::thread([dur = cfg.duration]() {
            std::this_thread::sleep_for(std::chrono::seconds(dur));
            g_running = false;
        }).detach();
    }

    net::EventLoop loop;

    // stats in background
    std::thread statsThread(statsPrinter);

    // event loop in background
    std::thread loopThread([&loop]() { loop.run(); });

    // blocking send (returns when g_running == false)
    if (cfg.tcp)
        runTcp(&loop, cfg);
    else
        runUdp(&loop, cfg);

    g_running = false;
    loop.stop();

    statsThread.join();
    loopThread.join();

    // final summary
    double totalMB  = g_bytesSent.load() / 1048576.0;
    double avgMBps  = cfg.duration > 0
                        ? totalMB / cfg.duration
                        : 0.0;

    std::cout
        << "\n─────────────────────────────────────────────\n"
        << "  total sent  : " << std::fixed << std::setprecision(2) << totalMB << " MB\n"
        << "  total pkts  : " << g_packetsSent.load() << "\n"
        << "  errors      : " << g_errors.load() << "\n";

    if (cfg.duration > 0)
        std::cout << "  avg rate    : " << avgMBps << " MB/s\n";

    std::cout << "  done.\n";
    return 0;
}