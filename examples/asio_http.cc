// 编译：
// g++  asio_http.cc -o asio_http -std=c++17 -lboost_system -lpthread
// 运行：
// ./asio_http <thread_number>
// 说明：thread_number 表示额外工作线程数，主线程也调用 io.run()（与 asio_echo 语义对齐）

#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <algorithm>

using boost::asio::ip::tcp;

namespace {

std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    explicit HttpSession(tcp::socket socket)
        : socket_(std::move(socket)) {}

    void start() {
        try {
            boost::asio::ip::tcp::no_delay option(true);
            socket_.set_option(option);
        } catch (...) {
        }
        readHeaders();
    }

private:
    void readHeaders() {
        auto self = shared_from_this();
        boost::asio::async_read_until(
            socket_, buffer_, "\r\n\r\n",
            [this, self](const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
                if (ec) {
                    return close();
                }

                // 解析请求行和头部
                std::istream is(&buffer_);
                std::string requestLine;
                if (!std::getline(is, requestLine)) {
                    return close();
                }
                if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();

                std::istringstream rl(requestLine);
                std::string method, target, version;
                rl >> method >> target >> version;
                if (method.empty() || target.empty()) {
                    return close();
                }

                std::unordered_map<std::string, std::string> headers;
                std::string line;
                while (std::getline(is, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) break; // 遇到空行，头结束
                    auto pos = line.find(':');
                    if (pos != std::string::npos) {
                        std::string name = line.substr(0, pos);
                        std::string value = line.substr(pos + 1);
                        // 去除前导空格
                        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
                        headers[toLower(name)] = value;
                    }
                }

                std::size_t contentLength = 0;
                auto it = headers.find("content-length");
                if (it != headers.end()) {
                    contentLength = static_cast<std::size_t>(std::strtoull(it->second.c_str(), nullptr, 10));
                }

                // 处理请求
                if (toLower(method) == "get") {
                    handleGet(target);
                } else if (toLower(method) == "post") {
                    readBodyAndHandlePost(target, contentLength);
                } else {
                    writeResponse(405, "Method Not Allowed", "text/plain", "method not allowed\n");
                }
            });
    }

    void handleGet(const std::string& target) {
        if (target == "/") {
            writeResponse(200, "OK", "text/plain", "hello from asio\n");
        } else {
            writeResponse(404, "Not Found", "text/plain", "not found\n");
        }
    }

    void readBodyAndHandlePost(const std::string& target, std::size_t contentLength) {
        if (target != "/echo") {
            // 未知路径也尝试读取并丢弃请求体以优雅关闭
            discardBodyThenRespond(contentLength, 404, "Not Found", "text/plain", "not found\n");
            return;
        }

        // buffer_ 里可能已经有部分或全部 body
        std::size_t alreadyBuffered = buffer_.size();
        if (alreadyBuffered >= contentLength) {
            std::string body(contentLength, '\0');
            std::istream is(&buffer_);
            is.read(&body[0], static_cast<std::streamsize>(contentLength));
            writeResponse(200, "OK", "text/plain", body);
            return;
        }

        auto self = shared_from_this();
        std::size_t need = contentLength - alreadyBuffered;
        boost::asio::async_read(
            socket_, buffer_, boost::asio::transfer_exactly(need),
            [this, self, contentLength](const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
                if (ec) {
                    return close();
                }
                std::string body(contentLength, '\0');
                std::istream is(&buffer_);
                is.read(&body[0], static_cast<std::streamsize>(contentLength));
                writeResponse(200, "OK", "text/plain", body);
            });
    }

    void discardBodyThenRespond(std::size_t contentLength, int status, const char* reason, const char* ct, const std::string& body) {
        std::size_t alreadyBuffered = buffer_.size();
        if (alreadyBuffered >= contentLength) {
            // 消耗掉 body
            std::istream is(&buffer_);
            if (contentLength > 0) {
                std::string tmp(contentLength, '\0');
                is.read(&tmp[0], static_cast<std::streamsize>(contentLength));
            }
            writeResponse(status, reason, ct, body);
            return;
        }
        auto self = shared_from_this();
        std::size_t need = contentLength - alreadyBuffered;
        boost::asio::async_read(
            socket_, buffer_, boost::asio::transfer_exactly(need),
            [this, self, status, reason, ct, body](const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
                if (ec) {
                    return close();
                }
                // 消耗掉 body
                std::istream is(&buffer_);
                // content-length 已知，但这里我们只需要把它从缓冲里读掉
                // 为简单起见，不再单独传长度，直接清空剩余缓冲（预期正好是 body）
                std::string tmp;
                tmp.assign(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
                writeResponse(status, reason, ct, body);
            });
    }

    void writeResponse(int status, const char* reason, const char* contentType, const std::string& body) {
        auto self = shared_from_this();
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << ' ' << reason << "\r\r\n"; // 故意写成 \r\r\n? 更正为标准 CRLF
        std::string startLine = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
        std::ostringstream hdr;
        hdr << "Content-Type: " << contentType << "\r\n";
        hdr << "Content-Length: " << body.size() << "\r\n";
        hdr << "Connection: close\r\n";

        responseBuffer_ = startLine + hdr.str() + "\r\n" + body;

        boost::asio::async_write(
            socket_, boost::asio::buffer(responseBuffer_),
            [this, self](const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
                (void)ec;
                close();
            });
    }

    void close() {
        boost::system::error_code ignored;
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    tcp::socket socket_;
    boost::asio::streambuf buffer_;
    std::string responseBuffer_;
};

class HttpServer {
public:
    HttpServer(boost::asio::io_context& io, unsigned short port)
        : io_(io), acceptor_(io, tcp::endpoint(tcp::v4(), port)) {}

    void start() { doAccept(); }

private:
    void doAccept() {
        acceptor_.async_accept([
            this
        ](const boost::system::error_code& ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<HttpSession>(std::move(socket))->start();
            }
            doAccept();
        });
    }

    boost::asio::io_context& io_;
    tcp::acceptor acceptor_;
};

} // namespace

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

    std::cout << "Starting Asio HttpServer with " << threadNum << " thread(s)..." << std::endl;

    try {
        boost::asio::io_context io;
        HttpServer server(io, 8080);
        server.start();

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


