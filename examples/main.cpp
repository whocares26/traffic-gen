#include <iostream>
#include <string>
#include <cstdint>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <vector>
#include <memory>
#include <iomanip>

#include "net/EventLoop.hpp"
#include "net/ThreadPool.hpp"
#include "net/TcpClient.hpp"
#include "net/UdpClient.hpp"
#include "net/InetAddress.hpp"

std::atomic<uint64_t> totalBytes{0};
std::atomic<uint64_t> totalPackets{0};
std::atomic<uint64_t> activeClients{0};
std::atomic<uint64_t> errors{0};
std::atomic<bool> running{true};

void signalHandler(int) {
    running = false;
    std::cout << "\nStopping...\n";
}

struct Config {
    bool tcp = true;
    std::string host = "127.0.0.1";
    int port = 8080;
    int numClients = 1;
    int duration = 60;
    int packetSize = 1024;
    int rate = 1024 * 1024;
    bool broadcast = false;
    bool help = false;
};

void printHelp() {
    std::cout << "Usage: ./traffic-gen [options]\n"
              << "  --tcp                 Use TCP (default)\n"
              << "  --udp                 Use UDP\n"
              << "  --host <ip>           Target host (default: 127.0.0.1)\n"
              << "  --port <num>          Target port (default: 8080)\n"
              << "  --clients <num>       Number of clients (default: 1)\n"
              << "  --duration <sec>      Test duration (default: 60)\n"
              << "  --size <bytes>        Packet size (default: 1024)\n"
              << "  --rate <bytes/sec>    Send rate (default: 1MB/s)\n"
              << "  --broadcast           Enable broadcast (UDP only)\n"
              << "  --help                Show this help\n";
}

bool parseArgs(int argc, char* argv[], Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            cfg.help = true;
            return true;
        }
        
        if (arg == "--tcp") {
            cfg.tcp = true;
        }
        else if (arg == "--udp") {
            cfg.tcp = false;
        }
        else if (arg == "--host") {
            if (++i >= argc) return false;
            cfg.host = argv[i];
        }
        else if (arg == "--port") {
            if (++i >= argc) return false;
            cfg.port = std::stoi(argv[i]);
        }
        else if (arg == "--clients") {
            if (++i >= argc) return false;
            cfg.numClients = std::stoi(argv[i]);
        }
        else if (arg == "--duration") {
            if (++i >= argc) return false;
            cfg.duration = std::stoi(argv[i]);
        }
        else if (arg == "--size") {
            if (++i >= argc) return false;
            cfg.packetSize = std::stoi(argv[i]);
        }
        else if (arg == "--rate") {
            if (++i >= argc) return false;
            cfg.rate = std::stoi(argv[i]);
        }
        else if (arg == "--broadcast") {
            cfg.broadcast = true;
        }
        else {
            return false;
        }
    }
    return true;
}

class RateLimiter {
public:
    RateLimiter(int bytesPerSec) 
        : m_bytesPerSec(bytesPerSec)
        , m_bytesThisSec(0)
        , m_lastReset(std::chrono::steady_clock::now()) {}
    
    bool canSend(int bytes) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastReset).count();
        
        if (elapsed >= 1) {
            m_bytesThisSec = 0;
            m_lastReset = now;
        }
        
        if (m_bytesThisSec + bytes <= m_bytesPerSec) {
            m_bytesThisSec += bytes;
            return true;
        }
        return false;
    }
    
    void waitIfNeeded(int bytes) {
        while (running && !canSend(bytes)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
private:
    int m_bytesPerSec;
    int m_bytesThisSec;
    std::chrono::steady_clock::time_point m_lastReset;
};

class TcpTrafficGenerator {
public:
    TcpTrafficGenerator(net::EventLoop* loop, const Config& cfg, RateLimiter* limiter)
        : m_loop(loop), m_cfg(cfg), m_limiter(limiter) {}
    
    void start() {
        m_serverAddr = net::InetAddress(m_cfg.port, m_cfg.host);
        
        for (int i = 0; i < m_cfg.numClients; ++i) {
            auto client = std::make_unique<net::TcpClient>(m_loop);
            
            client->connect(m_serverAddr,
                [this, i](const std::shared_ptr<net::TcpConnection>& conn) {
                    std::cout << "[Client " << i << "] Connected\n";
                    activeClients++;
                    std::thread([this, conn]() {
                        sendLoop(conn);
                    }).detach();
                },
                [](const std::shared_ptr<net::TcpConnection>&, std::string& data) {
                    totalBytes += data.size();
                    totalPackets++;
                },
                [this, i](const std::shared_ptr<net::TcpConnection>&) {
                    std::cout << "[Client " << i << "] Disconnected\n";
                    activeClients--;
                },
                [this, i]() {
                    std::cout << "[Client " << i << "] Connection error\n";
                    errors++;
                }
            );
            
            m_clients.push_back(std::move(client));
        }
    }
    
    void stop() {
        for (auto& client : m_clients) {
            client->cancel();
        }
        m_clients.clear();
    }
    
private:
    void sendLoop(const std::shared_ptr<net::TcpConnection>& conn) {
        while (running && conn->state() == net::TcpConnection::Connected) {
            std::string data(m_cfg.packetSize, 'X');
            
            m_limiter->waitIfNeeded(data.size());
            
            conn->send(data);
            totalBytes += data.size();
            totalPackets++;
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    net::EventLoop* m_loop;
    Config m_cfg;
    RateLimiter* m_limiter;
    net::InetAddress m_serverAddr;
    std::vector<std::unique_ptr<net::TcpClient>> m_clients;
};

class UdpTrafficGenerator {
public:
    UdpTrafficGenerator(net::EventLoop* loop, const Config& cfg, RateLimiter* limiter)
        : m_loop(loop), m_cfg(cfg), m_limiter(limiter) {}
    
    void start() {
        net::InetAddress serverAddr(m_cfg.port, m_cfg.host);
        
        for (int i = 0; i < m_cfg.numClients; ++i) {
            auto client = std::make_unique<net::UdpClient>(m_loop);
            client->setServerAddr(serverAddr);
            
            if (m_cfg.broadcast) {
                client->setBroadcast(true);
            }
            
            m_clients.push_back(std::move(client));
        }
        
        // Запускаем отправку в отдельном потоке
        std::thread([this]() {
            sendLoop();
        }).detach();
    }
    
    void stop() {
        m_clients.clear();
    }
    
private:
    void sendLoop() {
        while (running && !m_clients.empty()) {
            std::string data(m_cfg.packetSize, 'U');
            
            // Ждём разрешения rate limiter (учитываем, что отправляем всем клиентам)
            m_limiter->waitIfNeeded(data.size() * m_clients.size());
            
            for (auto& client : m_clients) {
                ssize_t sent = client->send(data);
                if (sent > 0) {
                    totalBytes += sent;
                    totalPackets++;
                } else {
                    errors++;
                }
            }
                    
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    net::EventLoop* m_loop;
    Config m_cfg;
    RateLimiter* m_limiter;
    std::vector<std::unique_ptr<net::UdpClient>> m_clients;
};

void statsPrinter() {
    auto startTime = std::chrono::steady_clock::now();
    uint64_t lastBytes = 0;
    
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
        
        uint64_t currentBytes = totalBytes.load();
        uint64_t bytesPerSec = currentBytes - lastBytes;
        lastBytes = currentBytes;
        
        double mbSent = currentBytes / 1024.0 / 1024.0;
        double kbPerSec = bytesPerSec / 1024.0;
        
        std::cout << "\r[" << elapsed << "s] "
                  << "Sent: " << std::fixed << std::setprecision(2) << mbSent << " MB "
                  << "(" << kbPerSec << " KB/s) "
                  << "Packets: " << totalPackets.load() << " "
                  << "Active: " << activeClients.load() << " "
                  << "Errors: " << errors.load() << "   "
                  << std::flush;
    }
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    Config cfg;
    if (!parseArgs(argc, argv, cfg) || cfg.help) {
        printHelp();
        return cfg.help ? 0 : 1;
    }
    
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    double rateMB = cfg.rate / 1024.0 / 1024.0;
    
    std::cout << "Traffic Generator starting...\n"
              << "  Protocol: " << (cfg.tcp ? "TCP" : "UDP") << "\n"
              << "  Target: " << cfg.host << ":" << cfg.port << "\n"
              << "  Clients: " << cfg.numClients << "\n"
              << "  Duration: " << cfg.duration << "s\n"
              << "  Packet size: " << cfg.packetSize << " bytes\n"
              << "  Target rate: " << std::fixed << std::setprecision(2) << rateMB << " MB/s\n"
              << "────────────────────────────────────────────\n";
    
    try {
        net::EventLoop mainLoop;
        
        RateLimiter limiter(cfg.rate);
        
        std::unique_ptr<TcpTrafficGenerator> tcpGen;
        std::unique_ptr<UdpTrafficGenerator> udpGen;
        
        if (cfg.tcp) {
            tcpGen = std::make_unique<TcpTrafficGenerator>(
                &mainLoop, cfg, &limiter);
            tcpGen->start();
        } else {
            udpGen = std::make_unique<UdpTrafficGenerator>(
                &mainLoop, cfg, &limiter);
            udpGen->start();
        }
        
        std::thread statsThread(statsPrinter);
        
        std::thread loopThread([&mainLoop]() {
            mainLoop.run();
        });
        
        auto endTime = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.duration);
        while (running && std::chrono::steady_clock::now() < endTime) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        running = false;
        mainLoop.stop();
        
        if (cfg.tcp && tcpGen) {
            tcpGen->stop();
        } else if (!cfg.tcp && udpGen) {
            udpGen->stop();
        }
        
        statsThread.join();
        loopThread.join();
        double totalMB = totalBytes.load() / 1024.0 / 1024.0;
        double avgRate = totalBytes.load() / cfg.duration / 1024.0;
        
        std::cout << "\nTest completed.\n"
                  << "  Total bytes: " << std::fixed << std::setprecision(2) << totalMB << " MB\n"
                  << "  Average rate: " << avgRate << " KB/s\n"
                  << "  Total packets: " << totalPackets.load() << "\n"
                  << "  Errors: " << errors.load() << "\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}