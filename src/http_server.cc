#include "netx/http_server.h"

#include "netx/tcp_connection.h"
#include <cstring>
namespace netx{

HttpServer::HttpServer(EventLoop *loop, const InetAddress& addr, int io_threads, bool reuse_port)
:server_(loop, addr, io_threads, reuse_port){
    {
        server_.SetConnectionCallback(
            [this](const ConnectionPtr &c)
            { OnConnection(c); });
        server_.SetMessageCallback(
            [this](const ConnectionPtr &c, Buffer *b)
            { OnMessage(c, b); });
    }
}
void HttpServer::GetStatic(const std::string& path, std::string body, const std::string& content_type = "text/plain"){
    // 预构建两份响应：keep-alive 与 close
    auto build = [&](bool keep)
    {
        std::string res;
        res.reserve(128 + body.size());
            // 直接拼接完整 HTTP 响应字符串
        res.append("HTTP/1.1 200 OK\r\n");
        res.append("Content-Type: ").append(content_type).append("\r\n");
        res.append("Content-Length: ")
            .append(std::to_string(body.size()))
            .append("\r\n");
        res.append(keep ? "Connection: keep-alive\r\n\r\n"
                        : "Connection: close\r\n\r\n");
        res.append(body);
        return res;
    };
    StaticResponse sr{build(true), build(false)};
    if (path.size() == 1 && path[0] == '/')
    {
        static_root_ = sr;
        static_root_enabled_ = true;
    }
    else
    {
        static_get_[path] = std::move(sr);
    }
}

// 启动服务器（内部调用 server_.Start()，开始监听 + 接受连接）
void HttpServer::Start(){server_.Start();}

void HttpServer::OnConnection(const ConnectionPtr& c){}

void HttpServer::OnMessage(const ConnectionPtr&c, Buffer *buf){
    while(true){
        //请求解析
        HttpRequest req;
        if (!parser_.Parse(buf, &req))
            return; 
        
        //路由分发：URL->处理函数的映射

        //GET   
        if(req.method()==HttpRequest::Method::kGet){
            std::string_view path_sv = req.path_view();
            //其中"/"直接比较字节 
            if (static_root_enabled_ && path_sv.size() == 1 && path_sv[0] == '/')
            {
                const bool ka = req.keep_alive();
                const std::string &out = ka ? static_root_.keep : static_root_.close;
                c->Send(out);
                if (!ka)
                    c->Shutdown();
                return;
            }
            //静态GET路由表查找预构建响应
            auto sit = static_get_.find(req.path());
            if (sit != static_get_.end())
            {
                const bool ka = req.keep_alive();
                const std::string &out = ka ? sit->second.keep : sit->second.close;
                c->Send(out);
                if (!ka)
                    c->Shutdown();
                return;
            }
        }

        //Post /echo 手写 header + 零拷贝 body 直发
        if (req.method() == HttpRequest::Method::kPost && req.path() == "/echo"){
            std::string_view body_sv = req.body_view();
            if (body_sv.data() == nullptr)
                body_sv = std::string_view();
            // 手写 HTTP 响应 header 到栈缓冲区，避免 HttpResponse 构造 + 序列化
            thread_local char hdr[256];
            size_t hdr_len = 0;
            HttpResponse tmp;
            tmp.set_status(200, "OK");
            tmp.set_header("Content-Type", "text/plain");
            tmp.set_keep_alive(req.keep_alive());
            tmp.set_body(""); // body 不放到对象里，避免复制
            {
                size_t used = 0;
                auto put = [&](const char *s)
                {
                    size_t n = strlen(s);
                    if (used + n > sizeof(hdr))
                        return false;
                    memcpy(hdr + used, s, n);
                    used += n;
                    return true;
                };
                auto putn = [&](const char *s, size_t n)
                {
                    if (used + n > sizeof(hdr))
                        return false;
                    memcpy(hdr + used, s, n);
                    used += n;
                    return true;
                };
                if (!put("HTTP/1.1 200 OK\r\n"))
                { /* fallback below */
                }
                if (!put("Content-Type: text/plain\r\n"))
                { /* fallback below */
                }
                if (!put("Connection: "))
                { /* fallback below */
                }
                if (!put(req.keep_alive() ? "keep-alive\r\n"
                                            : "close\r\n"))
                { /* fallback below */
                }
                if (!put("Content-Length: "))
                { /* fallback below */
                }
                char lenbuf[32];
                int ln = snprintf(lenbuf, sizeof(lenbuf), "%zu", body_sv.size());
                if (ln < 0)
                    ln = 0;
                if (!putn(lenbuf, (size_t)ln))
                { /* fallback below */
                }
                if (!put("\r\n\r\n"))
                { /* fallback below */
                }
                hdr_len = used;
            }

            // 环节三（热点路径）：响应发送
            // SendVec 用 writev 一次发 header + body，只调一次系统调用
            if (hdr_len == 0)
            {
                // 退化路径：hdr 256 字节不够，用 string 构造 header
                std::string header;
                header.reserve(128);
                header.append("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n");
                header.append("Connection: ");
                header.append(req.keep_alive() ? "keep-alive" : "close");
                header.append("\r\n");
                header.append("Content-Length: ");
                header.append(std::to_string(body_sv.size()));
                header.append("\r\n\r\n");
                c->SendVec(header.data(), header.size(), body_sv.data(),
                            body_sv.size());
            }
            else
            {
                c->SendVec(hdr, hdr_len, body_sv.data(), body_sv.size());
            }
            if (!req.keep_alive())
                c->Shutdown();
            // body 是零拷贝视图，用完后必须从 Buffer 消费掉
            if (!body_sv.empty())
                buf->retrieve(body_sv.size());
            continue; // 回到 while 顶部，尝试解析下一个 pipeline 请求
        }

        // 通用动态路由：查 get_handlers_ / post_handlers_ 路由表
        HttpResponse resp;
        resp.set_keep_alive(req.keep_alive());

        auto method = req.method();
        std::string_view path_sv = req.path_view();
        bool handled = false;
        if (method == HttpRequest::Method::kGet)
        {
            auto it = get_handlers_.find(std::string(path_sv));
            if (it != get_handlers_.end())
            {
                it->second(req, &resp); // 调用户注册的 handler，handler 填充 resp
                handled = true;
            }
        }
        else if (method == HttpRequest::Method::kPost)
        {
            auto it = post_handlers_.find(std::string(path_sv));
            if (it != post_handlers_.end())
            {
                it->second(req, &resp); // 调用户注册的 handler，handler 填充 resp
                handled = true;
            }
        }
        if (!handled)
        {
            resp.set_status(404, "Not Found");
            resp.set_body("not found");
        }

        thread_local char hdr[512];
            size_t hdr_len = 0;
            if (resp.format_header_into(hdr, sizeof(hdr), &hdr_len))
            {
                const std::string &body = resp.body();
                c->SendVec(hdr, hdr_len, body.data(), body.size());
            }
            else
            {
                // 退化路径：512 字节不够，用 string 构造 header
                std::string header = resp.to_header_string();
                const std::string &body = resp.body();
                c->SendVec(header.data(), header.size(), body.data(), body.size());
            }
            if (!req.keep_alive())
                c->Shutdown();
            // body 是零拷贝视图（body_view_ 指向 Buffer），用完后必须消费掉
            // 不 retrieve 的话，下次 Parse 会把 body 数据当成新请求的头部
            if (!req.body_view().empty())
                buf->retrieve(req.body_view().size());
            // 继续 while 循环，尝试解析下一个 pipeline 请求
    }
}


}