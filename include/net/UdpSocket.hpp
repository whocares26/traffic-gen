#pragma once

#include "net/Socket.hpp"
#include "net/InetAddress.hpp"

namespace net {

class UdpSocket : public Socket {
public:
    UdpSocket();
    explicit UdpSocket(int fd);

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    ssize_t sendto(const void* data, size_t length, const InetAddress& dest_addr);
    ssize_t recvfrom(void* buffer, size_t length, InetAddress& src_addr);

    void set_broadcast(bool on);
    
    int native_handle() const { return fd(); }
};

} // namespace net