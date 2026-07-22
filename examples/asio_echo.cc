// 编译：g++ examples/asio_echo.cc -o asio_echo -std=c++17 -lboost_system -lpthread

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <cstdlib>

using boost::asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket socket)
        : socket_(std::move(socket)) {}

    void start() {
        try {
            boost::asio::ip::tcp::no_delay option(true);
            socket_.set_option(option);
        } catch (...) {
            // 忽略设置失败
        }
        // std::cout << "Connection UP" << std::endl;
        doRead();
    }

private:
    void doRead() {
        auto self = shared_from_this();
        socket_.async_read_some(
            boost::asio::buffer(readBuffer_),
            [this, self](const boost::system::error_code& ec, std::size_t length) {
                if (!ec) {
                    doWrite(length);
                } else {
                    handleClose(ec);
                }
            });
    }

    void doWrite(std::size_t length) {
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(readBuffer_.data(), length),
            [this, self](const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
                if (!ec) {
                    doRead();
                } else {
                    handleClose(ec);
                }
            });
    }

    void handleClose(const boost::system::error_code& /*ec*/) {
        boost::system::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
        // std::cout << "Connection DOWN" << std::endl;
    }

    tcp::socket socket_;
    std::array<char, 64 * 1024> readBuffer_{};
};

class EchoServer {
public:
    EchoServer(boost::asio::io_context& io, unsigned short port)
        : io_(io), acceptor_(io, tcp::endpoint(tcp::v4(), port)) {}

    void start() { doAccept(); }

private:
    void doAccept() {
        acceptor_.async_accept([
            this
        ](const boost::system::error_code& ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket))->start();
            }
            doAccept();
        });
    }

    boost::asio::io_context& io_;
    tcp::acceptor acceptor_;
};
// g++  asio_echo.cc -o asio_echo -std=c++17 -lboost_system -lpthread

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <thread_number>\n";
        std::cerr << "Example: " << argv[0] << " 4\n";
        return -1;
    }

    int threadNum = std::atoi(argv[1]);
    if (threadNum < 0) {
        std::cerr << "Error: Number of threads must be non-negative." << std::endl;
        return -1;
    }

    std::cout << "Starting Asio EchoServer with " << threadNum << " thread(s)..." << std::endl;

    try {
        boost::asio::io_context io;
        EchoServer server(io, 8083);
        server.start();

        // 与 muduo 语义对齐：参数表示额外工作线程数，主线程也参与 run
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(threadNum));
        for (int i = 0; i < threadNum; ++i) {
            workers.emplace_back([&io]() { io.run(); });
        }

        io.run();
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    } catch (const std::exception& ex) {
        std::cerr << "Exception: " << ex.what() << std::endl;
        return -1;
    }

    return 0;
}


