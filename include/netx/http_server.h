#ifndef NETX_HTTP_SERVER_H_
#define NETX_HTTP_SERVER_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "netx/http_parser.h"
#include "netx/http_request.h"
#include "netx/http_response.h"
#include "netx/tcp_server.h"

namespace netx {

class EventLoop;
class TcpConnection;
// HttpServer 基于 TcpServer 提供简单的 HTTP 服务
class HttpServer{
public:
    using ConnectionPtr = std::shared_ptr<TcpConnection>;
    // Handler 类型：接收只读请求 + 可写响应，处理完后 HttpServer 自动序列化发送
    using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;
    
    HttpServer(EventLoop *loop, const InetAddress& addr, int io_threads, bool reuse_port);

    void Get(const std::string &path, Handler h)
    {
    
    }
private:
    void OnConnection(const ConnectionPtr& c);

    /*消息到达时调用
    循环 解析请求->路由分发->构造响应->发送*/
    void OnMessage(const ConnectionPtr&c, Buffer *buf);

    TcpServer server_;//内部持有TcpServer
    HttpParser parser_;//协议解析器

    std::unordered_map<std::string,Handler> get_handlers_;//get路由表
    std::unordered_map<std::string,Handler> post_handlers_;

    struct StaticResponse {
        std::string keep;
        std::string close;
    }



};

}
#endif