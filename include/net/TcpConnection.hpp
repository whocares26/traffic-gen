#pragma once

#include <memory>
#include <string>
#include <functional>
#include "net/TcpSocket.hpp"
#include "net/InetAddress.hpp"
#include "net/EventLoop.hpp"

namespace net {

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using ConnectionCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using MessageCallback    = std::function<void(const std::shared_ptr<TcpConnection>&, std::string&)>;
    using CloseCallback      = std::function<void(const std::shared_ptr<TcpConnection>&)>;
    using WriteReadyCallback = std::function<void(const std::shared_ptr<TcpConnection>&)>;

    enum State { Connecting, Connected, Disconnecting, Disconnected };

    TcpConnection(EventLoop* loop, int client_fd, const InetAddress& peerAddr);
    ~TcpConnection() = default;

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    void connection_established();
    void connection_destroyed();

    void send(const std::string& data);
    void shutdown();

    void set_connection_callback(ConnectionCallback cb);
    void set_message_callback(MessageCallback cb);
    void set_close_callback(CloseCallback cb);
    void set_write_ready_callback(WriteReadyCallback cb);

    int         fd();
    State       state()        const { return m_state; }
    std::string peer_address() const;
    uint16_t    peer_port()    const;

private:
    void handle_read();
    void handle_write();
    void handle_close();
    void handle_error();

    EventLoop*                      m_loop;
    std::unique_ptr<net::TcpSocket> m_client_socket;
    InetAddress                     m_peer_addr;
    State                           m_state;
    std::string                     m_input_buffer;
    std::string                     m_output_buffer;
    ConnectionCallback              m_connection_cb;
    MessageCallback                 m_message_cb;
    CloseCallback                   m_close_cb;
    WriteReadyCallback              m_write_ready_cb;
};

} // namespace net