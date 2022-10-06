
// The following is a flow and structural diagram depicting the
// various elements  (proxy, server  and client)  and how  they
// connect and interact with each other.

//
//                                    ---> upstream --->           +---------------+
//                                                     +---->------>               |
//                               +-----------+         |           | Remote Server |
//                     +--------->          [x]--->----+  +---<---[x]              |
//                     |         | TCP Proxy |            |        +---------------+
// +-----------+       |  +--<--[x] Server   <-----<------+
// |          [x]--->--+  |      +-----------+
// |  Client   |          |
// |           <-----<----+
// +-----------+
//                <--- downstream <---
#include "ServerSession.h"
#include "Shared/ConfigManage.h"
#include "Shared/Log.h"
#include <chrono>
#include <cstring>
#include <string>

constexpr static int SSL_SHUTDOWN_TIMEOUT = 30;
std::atomic<uint32_t> ServerSession::connection_num(0);
ServerSession::ServerSession(boost::asio::io_context& ioctx, boost::asio::ssl::context& sslctx)

    : io_context_(ioctx)
    , upstream_ssl_socket(ioctx, sslctx)
    , downstream_socket(ioctx)
    , resolver_(ioctx)
    , in_buf(MAX_BUFF_SIZE)
    , out_buf(MAX_BUFF_SIZE)
// , ssl_shutdown_timer(ioctx)

{
    connection_num++;
}
ServerSession::~ServerSession()
{
    connection_num--;
    DEBUG_LOG << "Session dectructed, current alive session:" << connection_num.load();
}
void ServerSession::start()
{
    auto self = shared_from_this();
    upstream_ssl_socket.async_handshake(boost::asio::ssl::stream_base::server, [this, self](const boost::system::error_code& error) {
        if (error) {
            ERROR_LOG << "Current alive sessions:" << connection_num.load() << "SSL handshake failed: " << error.message();
            destroy();
            return;
        }
        handle_trojan_handshake();
    });
}
void ServerSession::handle_trojan_handshake()
{
    // trojan handshak
    auto self = shared_from_this();
    // upstream_ssl_socket.next_layer().read_some(boost::asio::buffer)read_some()
    //  char temp[4096];
    upstream_ssl_socket.async_read_some(boost::asio::buffer(in_buf),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (ec) {
                NOTICE_LOG << "Current alive sessions:" << connection_num.load() << "Read trojan message failed:" << ec.message();
                // Log::log_with_endpoint(in_endpoint, "SSL handshake failed: " + error.message(), Log::ERROR);
                destroy();
                return;
            }
            bool valid = req.parse(std::string(in_buf.data(), length)) != -1;
            if (valid) {
                //
                if (!ConfigManage::instance().server_cfg.allowed_passwords.count(req.password)) {
                    ERROR_LOG << "Current alive sessions:" << connection_num.load() << "unspoorted password from client....,end session";
                    destroy();
                    return;
                }

            } else {
                ERROR_LOG << "parse trojan request fail";
                destroy();
                return;
            }
            do_resolve();
        });
}
void ServerSession::do_resolve()
{
    auto self(shared_from_this());

    resolver_.async_resolve(tcp::resolver::query(req.address.address, std::to_string(req.address.port)),
        [this, self](const boost::system::error_code& ec, tcp::resolver::iterator it) {
            if (!ec) {
                do_connect(it);
            } else {
                ERROR_LOG << "Current alive sessions:" << connection_num.load() << "failed to resolve " << remote_host << ":" << remote_port << " " << ec.message();
                destroy();
            }
        });
}

void ServerSession::do_connect(tcp::resolver::iterator& it)
{
    auto self(shared_from_this());
    state_ = FORWARD;
    downstream_socket.async_connect(*it,
        [this, self, it](const boost::system::error_code& ec) {
            if (!ec) {
                boost::asio::socket_base::keep_alive option(true);
                downstream_socket.set_option(option);
                DEBUG_LOG << "connected to " << req.address.address << ":" << req.address.port;
                // write_socks5_response();
                // TODO
                if (req.payload.empty() == false) {
                    DEBUG_LOG << "payload not empty";
                    // std::memcpy(out_buf.data(), req.payload.data(), req.payload.length());
                    //  async_bidirectional_write(1, req.payload.length());

                    boost::asio::async_write(downstream_socket, boost::asio::buffer(req.payload),
                        [this, self](boost::system::error_code ec, std::size_t length) {
                            if (!ec)
                                async_bidirectional_read(3);
                            else {
                                ERROR_LOG << "Current alive sessions:" << connection_num.load() << "closing session. Client socket write error" << ec.message();
                                // Most probably client closed socket. Let's close both sockets and exit session.
                                destroy();
                                return;
                            }
                        });
                }
                // read packet from both dirction
                else
                    async_bidirectional_read(3);

            } else {
                ERROR_LOG << "Current alive sessions:" << connection_num.load() << "failed to connect " << remote_host << ":" << remote_port << " " << ec.message();
                destroy();
            }
        });
}
void ServerSession::async_bidirectional_read(int direction)
{
    auto self = shared_from_this();
    // We must divide reads by direction to not permit second read call on the same socket.
    if (direction & 0x01)
        upstream_ssl_socket.async_read_some(boost::asio::buffer(in_buf),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    DEBUG_LOG << "--> " << std::to_string(length) << " bytes";

                    async_bidirectional_write(1, length);
                } else // if (ec != boost::asio::error::eof)
                {
                    if (ec != boost::asio::error::eof && ec != boost::asio::error::operation_aborted) {
                        ERROR_LOG << "Current alive sessions:" << connection_num.load() << "closing session. Client socket read error: " << ec.message();
                    }

                    // Most probably client closed socket. Let's close both sockets and exit session.
                    destroy();
                    return;
                }
            });

    if (direction & 0x2)
        downstream_socket.async_read_some(boost::asio::buffer(out_buf),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {

                    DEBUG_LOG << "<-- " << std::to_string(length) << " bytes";

                    async_bidirectional_write(2, length);
                } else // if (ec != boost::asio::error::eof)
                {
                    if (ec != boost::asio::error::eof && ec != boost::asio::error::operation_aborted) {
                        ERROR_LOG << "Current alive sessions:" << connection_num.load() << "closing session. Remote socket read error: " << ec.message();
                    }

                    // Most probably remote server closed socket. Let's close both sockets and exit session.
                    destroy();
                    return;
                }
            });
}
void ServerSession::async_bidirectional_write(int direction, size_t len)
{
    auto self(shared_from_this());

    switch (direction) {
    case 1:
        boost::asio::async_write(downstream_socket, boost::asio::buffer(in_buf, len),
            [this, self, direction](boost::system::error_code ec, std::size_t length) {
                if (!ec)
                    async_bidirectional_read(direction);
                else {
                    if (ec != boost::asio::error::operation_aborted) {
                        ERROR_LOG << "Current alive sessions:" << connection_num.load() << "closing session. Client socket write error" << ec.message();
                    }
                    // Most probably client closed socket. Let's close both sockets and exit session.
                    destroy();
                    return;
                }
            });
        break;
    case 2:
        boost::asio::async_write(upstream_ssl_socket, boost::asio::buffer(out_buf, len),
            [this, self, direction](boost::system::error_code ec, std::size_t length) {
                if (!ec)
                    async_bidirectional_read(direction);
                else {
                    if (ec != boost::asio::error::operation_aborted) {
                        ERROR_LOG << "Current alive sessions:" << connection_num.load() << "closing session. Remote socket write error", ec.message();
                    }
                    // Most probably remote server closed socket. Let's close both sockets and exit session.
                    destroy();
                    return;
                }
            });
        break;
    }
}
boost::asio::ip::tcp::socket& ServerSession::socket()
{
    return upstream_ssl_socket.next_layer();
}
void ServerSession::destroy()
{
    if (state_ == DESTROY) {
        return;
    }
    state_ = DESTROY;

    boost::system::error_code ec;
    resolver_.cancel();
    if (downstream_socket.is_open()) {
        downstream_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        downstream_socket.cancel();
        downstream_socket.close();
    }
    if (upstream_ssl_socket.lowest_layer().is_open()) {
        upstream_ssl_socket.lowest_layer().cancel(ec);
        upstream_ssl_socket.lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
        upstream_ssl_socket.lowest_layer().close(ec);
    }
}