#include "net/UdpClient.hpp"
#include <cstring>

namespace net {

    UdpClient::UdpClient(EventLoop* loop) 
        : m_loop(loop), m_socket(std::make_unique<UdpSocket>()) { }

    void UdpClient::setServerAddr(const InetAddress& addr) {
        m_serverAddr = addr;
    }

    ssize_t UdpClient::send(const std::string& data) {
        return m_socket->sendto(data.c_str(), data.size(), m_serverAddr);
    }

    ssize_t UdpClient::sendTo(const std::string& data, const InetAddress& addr) {
        return m_socket->sendto(data.c_str(), data.size(), addr);
    }

    void UdpClient::setBroadcast(bool on) {
        m_socket->set_broadcast(on);
    }

    void UdpClient::setMessageCallback(MessageCallback callback) {
        m_msgCallback = callback;
    }

    void UdpClient::startReading() {
        if (m_reading) return;
        
        m_reading = true;
        m_loop->add_fd(m_socket->native_handle(), EPOLLIN | EPOLLET, 
            [this](uint32_t events) {
                if (events & (EPOLLERR | EPOLLHUP)) {
                    // Handle error or hangup
                    return;
                }
                handleRead();
            });
    }

    void UdpClient::handleRead() {
        char buffer[65536]; // Max UDP packet size
        InetAddress from_addr;
        
        while (true) {
            ssize_t bytes = m_socket->recvfrom(buffer, sizeof(buffer), from_addr);
            if (bytes == -1) {
                // EAGAIN or EWOULDBLOCK - no more data available
                break;
            }
            
            std::string data(buffer, bytes);
            if (m_msgCallback) {
                m_msgCallback(data, from_addr);
            }
        }
    }

} // namespace net
