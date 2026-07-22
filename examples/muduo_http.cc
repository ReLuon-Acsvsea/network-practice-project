// 编译: g++  muduo_http.cc -o muduo_http -std=c++17 -O2 -lmuduo_net -lmuduo_base -lpthread

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/Buffer.h>
#include <muduo/base/Logging.h>

#include <boost/bind.hpp>
#include <boost/any.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

struct HttpContext {
    enum ParseState { kExpectHeaders, kExpectBody, kGotAll };

    ParseState state = kExpectHeaders;
    std::string method;
    std::string path;
    std::string httpVersion; // e.g. HTTP/1.1
    bool keepAlive = true;   // default per HTTP version / Connection header
    std::unordered_map<std::string, std::string> headers; // lower-cased keys
    size_t contentLength = 0;
    std::string body;
};

inline std::string toLower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

inline void trim(std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    if (start > 0 || end < s.size()) s = s.substr(start, end - start);
}

const char CRLFCRLF[] = "\r\n\r\n";

bool parseHeadersFromString(const std::string& data, HttpContext& ctx) {
    // data includes request line + headers (without the trailing CRLFCRLF)
    size_t lineStart = 0;
    size_t lineEnd = data.find("\r\n", lineStart);
    if (lineEnd == std::string::npos) return false;

    // Request line: METHOD SP PATH SP HTTP/1.1
    const std::string requestLine = data.substr(lineStart, lineEnd - lineStart);
    {
        size_t p1 = requestLine.find(' ');
        if (p1 == std::string::npos) return false;
        size_t p2 = requestLine.find(' ', p1 + 1);
        if (p2 == std::string::npos) return false;
        ctx.method = requestLine.substr(0, p1);
        ctx.path = requestLine.substr(p1 + 1, p2 - (p1 + 1));
        ctx.httpVersion = requestLine.substr(p2 + 1);
    }

    // Headers
    lineStart = lineEnd + 2;
    while (lineStart < data.size()) {
        lineEnd = data.find("\r\n", lineStart);
        if (lineEnd == std::string::npos) break;
        if (lineEnd == lineStart) break; // empty line (should not happen here)
        std::string line = data.substr(lineStart, lineEnd - lineStart);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            trim(key);
            trim(val);
            ctx.headers[toLower(key)] = val;
        }
        lineStart = lineEnd + 2;
    }

    auto it = ctx.headers.find("content-length");
    if (it != ctx.headers.end()) {
        ctx.contentLength = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    } else {
        ctx.contentLength = 0;
    }

    // Determine keep-alive policy
    // Default: HTTP/1.1 keep-alive, HTTP/1.0 close unless explicitly keep-alive
    if (toLower(ctx.httpVersion) == "http/1.1") {
        ctx.keepAlive = true;
    } else {
        ctx.keepAlive = false;
    }
    auto itConn = ctx.headers.find("connection");
    if (itConn != ctx.headers.end()) {
        std::string v = toLower(itConn->second);
        trim(v);
        if (v == "close") ctx.keepAlive = false;
        else if (v == "keep-alive") ctx.keepAlive = true;
    }
    return true;
}

std::string makeHttpResponse(int code, const std::string& status, const std::string& body, const std::string& contentType = "text/plain", bool keepAlive = true) {
    std::string res;
    res.reserve(128 + body.size());
    res.append("HTTP/1.1 ").append(std::to_string(code)).append(" ").append(status).append("\r\n");
    res.append("Content-Type: ").append(contentType).append("\r\n");
    res.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
    if (keepAlive) {
        res.append("Connection: keep-alive\r\n");
        // Optionally advertise keep-alive params; harmless if omitted
        // res.append("Keep-Alive: timeout=5, max=1000\r\n");
    } else {
        res.append("Connection: close\r\n");
    }
    res.append("\r\n");
    res.append(body);
    return res;
}

} // namespace

class SimpleHttpServer {
public:
    SimpleHttpServer(muduo::net::EventLoop* loop, const muduo::net::InetAddress& listenAddr)
        : server_(loop, listenAddr, "MuduoHttpServer") {
        server_.setConnectionCallback(boost::bind(&SimpleHttpServer::onConnection, this, _1));
        server_.setMessageCallback(boost::bind(&SimpleHttpServer::onMessage, this, _1, _2, _3));
    }

    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
    void start() { server_.start(); }

private:
    void onConnection(const muduo::net::TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->setContext(HttpContext{});
            conn->setTcpNoDelay(true);
            LOG_INFO << "Connection UP from " << conn->peerAddress().toIpPort();
        } else {
            LOG_INFO << "Connection DOWN";
        }
    }

    void onMessage(const muduo::net::TcpConnectionPtr& conn, muduo::net::Buffer* buf, muduo::Timestamp) {
        HttpContext* ctx = boost::any_cast<HttpContext>(conn->getMutableContext());
        // Parse headers
        if (ctx->state == HttpContext::kExpectHeaders) {
            const char* begin = buf->peek();
            const char* end = buf->beginWrite();
            const char* headerEnd = std::search(begin, end, CRLFCRLF, CRLFCRLF + 4);
            if (headerEnd == end) {
                // wait for more data
                return;
            }
            std::string headerBlock(begin, headerEnd);
            if (!parseHeadersFromString(headerBlock, *ctx)) {
                const std::string bad = makeHttpResponse(400, "Bad Request", "Bad Request\n");
                conn->send(bad);
                conn->shutdown();
                return;
            }
            buf->retrieveUntil(headerEnd + 4);
            if (ctx->method == "POST" && ctx->contentLength > 0) {
                ctx->state = HttpContext::kExpectBody;
            } else {
                ctx->state = HttpContext::kGotAll;
            }
        }

        // Parse body if needed
        if (ctx->state == HttpContext::kExpectBody) {
            if (buf->readableBytes() < ctx->contentLength) {
                return; // wait for full body
            }
            ctx->body = buf->retrieveAsString(ctx->contentLength);
            ctx->state = HttpContext::kGotAll;
        }

        if (ctx->state == HttpContext::kGotAll) {
            std::string body;
            int code = 200;
            std::string status = "OK";
            std::string contentType = "text/plain";

            if (ctx->method == "GET" && ctx->path == "/") {
                body = "hello from muduo\n";
            } else if (ctx->method == "POST" && ctx->path == "/echo") {
                body = ctx->body;
            } else {
                code = 404;
                status = "Not Found";
                body = "404 Not Found\n";
            }

            const bool keepAlive = ctx->keepAlive;
            const std::string resp = makeHttpResponse(code, status, body, contentType, keepAlive);
            conn->send(resp);
            if (!keepAlive) {
            conn->shutdown();
            // reset context for next request (if client reconnects)
            conn->setContext(HttpContext{});
            } else {
                // keep connection open for next request on same connection
                conn->setContext(HttpContext{});
            }
        }
    }

    muduo::net::TcpServer server_;
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <thread_number>\n";
        std::cerr << "Example: " << argv[0] << " 4\n";
        return -1;
    }
    int thread_num = std::atoi(argv[1]);
    if (thread_num < 0) {
        std::cerr << "Error: Number of threads must be non-negative.\n";
        return -1;
    }

    muduo::Logger::setLogLevel(muduo::Logger::WARN);
    LOG_INFO << "Starting MuduoHttpServer with " << thread_num << " thread(s) on :8081";
    muduo::net::EventLoop loop;
    muduo::net::InetAddress listenAddr(8081);
    SimpleHttpServer server(&loop, listenAddr);
    server.setThreadNum(thread_num);
    server.start();
    loop.loop();
}


