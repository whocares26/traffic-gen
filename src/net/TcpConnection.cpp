#include "net/TcpConnection.hpp"
#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>

namespace net {

void TcpConnection::set_connection_callback(ConnectionCallback cb) { m_connection_cb  = cb; }
void TcpConnection::set_message_callback(MessageCallback cb)       { m_message_cb     = cb; }
void TcpConnection::set_close_callback(CloseCallback cb)           { m_close_cb       = cb; }
void TcpConnection::set_write_ready_callback(WriteReadyCallback cb){ m_write_ready_cb = cb; }

int         TcpConnection::fd()           { return m_client_socket->fd(); }
std::string TcpConnection::peer_address() const { return m_peer_addr.to_ip();  }
uint16_t    TcpConnection::peer_port()    const { return m_peer_addr.to_port(); }

TcpConnection::TcpConnection(EventLoop* loop, int client_fd, const InetAddress& peerAddr)
    : m_loop(loop)
    , m_peer_addr(peerAddr)
    , m_client_socket(std::make_unique<net::TcpSocket>(client_fd))
    , m_state(Connecting)
{
    m_client_socket->set_nonblocking();
}

void TcpConnection::connection_established() {
    m_state = Connected;

    // Стартуем только с EPOLLIN | EPOLLET.
    // EPOLLOUT будет добавлен в send() — только когда есть что писать.
    // Это предотвращает бесконечный firing level-triggered EPOLLOUT.
    m_loop->add_fd(m_client_socket->fd(),
                   EPOLLIN | EPOLLET,
                   [self = shared_from_this()](uint32_t events) {
                       if (events & (EPOLLERR | EPOLLHUP)) {
                           self->handle_error();
                           return;
                       }
                       if (events & EPOLLIN)  self->handle_read();
                       if (events & EPOLLOUT) self->handle_write();
                   });

    if (m_connection_cb)
        m_connection_cb(shared_from_this());
}

void TcpConnection::handle_read() {
    char buf[4096];
    while (true) {
        int n = ::read(m_client_socket->fd(), buf, sizeof(buf));
        if (n > 0) {
            m_input_buffer.append(buf, n);
        } else if (n == 0) {
            handle_close();
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR)                          continue;
            handle_error();
            return;
        }
    }
    if (!m_input_buffer.empty() && m_message_cb)
        m_message_cb(shared_from_this(), m_input_buffer);
}

void TcpConnection::send(const std::string& data) {
    if (m_state != Connected) return;
    m_output_buffer.append(data);
    // Включаем EPOLLOUT чтобы EventLoop вызвал handle_write()
    m_loop->update_fd(m_client_socket->fd(), EPOLLIN | EPOLLOUT | EPOLLET);
}

void TcpConnection::handle_write() {
    while (!m_output_buffer.empty()) {
        ssize_t n = ::write(m_client_socket->fd(),
                            m_output_buffer.data(),
                            m_output_buffer.size());
        if (n > 0) {
            m_output_buffer.erase(0, static_cast<size_t>(n));
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            handle_error();
            return;
        }
    }

    if (m_output_buffer.empty()) {
        // Буфер записан — убираем EPOLLOUT чтобы не жечь CPU впустую,
        // затем сразу просим прикладной код положить следующий пакет.
        m_loop->update_fd(m_client_socket->fd(), EPOLLIN | EPOLLET);

        if (m_state == Disconnecting) {
            handle_close();
            return;
        }

        // write_ready_callback вызывает conn->send() → буфер снова заполняется
        // → update_fd добавит EPOLLOUT → handle_write() вызовется снова.
        // Так EventLoop сам гонит трафик без внешних потоков.
        if (m_write_ready_cb)
            m_write_ready_cb(shared_from_this());
    }
}

void TcpConnection::handle_close() {
    // Удержание себя до конца метода: remove_fd уничтожает лямбду в EventLoop,
    // которая держала [self = shared_from_this()], а m_close_cb стирает нас
    // из TcpServer::m_connections. Без guard это может уронить refcount до 0
    // прямо посреди метода.
    auto guard = shared_from_this();
    m_state = Disconnected;
    m_loop->remove_fd(m_client_socket->fd());
    if (m_close_cb)      m_close_cb(guard);
    if (m_connection_cb) m_connection_cb(guard);
}

void TcpConnection::connection_destroyed() {
    auto guard = shared_from_this();
    if (m_state == Connected)
        m_state = Disconnected;
    m_loop->remove_fd(m_client_socket->fd());
    if (m_connection_cb) m_connection_cb(guard);
}

void TcpConnection::shutdown() {
    if (m_state == Connected)
        m_state = Disconnecting;
    if (m_output_buffer.empty())
        handle_close();
}

void TcpConnection::handle_error() {
    handle_close();
}

} // namespace net