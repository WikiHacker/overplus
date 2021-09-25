#ifndef SERVER_H_
#define SERVER_H_
#include "Shared/IoContextPool.h"
#include "Shared/Log.h"
#include "Session.h"
#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <memory>
using namespace boost::asio;

class Server : private boost::noncopyable {
public:
    Server(const std::string& address, const std::string& port);
    void run();

private:
    void add_signals();
    void do_accept();

private:
    IoContextPool context_pool;
    // ios_deque io_contexts_;
    boost::asio::io_context& io_context;

    boost::asio::signal_set signals;
    boost::asio::ip::tcp::acceptor acceptor_;
};
#endif