// 程序员老廖：https://space.bilibili.com/3494351095204205
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
//
// 组合而非继承：内部持有一个 TcpServer，把自己的回调注册上去
// TcpServer 负责管理连接和收发数据，HttpServer 负责 HTTP 协议解析和路由分发
//
// 请求处理流程：
//   epoll_wait → Channel → TcpServer::OnMessage
//     → HttpServer::OnMessage（这里做 HTTP 解析 + 路由）
//       → HttpParser::Parse（从 Buffer 解析出 HttpRequest）
//       → 查路由表 / 静态快路径
//       → 调 handler(req, &resp)
//       → format_header_into + SendVec（writev 发送）
//
// 路由分发优先级（从快到慢）：
//   1. GET "/" 静态根路径（直接比较，不查 map）
//   2. 其他 GetStatic 注册的静态路径（预构建响应，查 map 直接发）
//   3. POST /echo 热点路径（零拷贝手写 header）
//   4. 通用动态路由（Get/Post 注册的 handler）
class HttpServer {
 public:
  using ConnectionPtr = std::shared_ptr<TcpConnection>;
  // Handler 类型：接收只读请求 + 可写响应，处理完后 HttpServer 自动序列化发送
  using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;

  // 构造函数：创建 HttpServer
  // 参数和 TcpServer 一样，内部会创建一个 TcpServer 实例
  // 构造时把自己的 OnConnection/OnMessage 注册到 server_ 上
  HttpServer(EventLoop* loop, const InetAddress& addr, int io_threads, bool reuse_port);

  // 注册动态路由：按 path 存入 hash map，运行时 O(1) 查找
  void Get(const std::string& path, Handler h) { get_handlers_[path] = std::move(h); }
  void Post(const std::string& path, Handler h) { post_handlers_[path] = std::move(h); }

  // 注册静态 GET 响应：预构建两份完整 HTTP 响应（keep-alive 和 close）
  // 运行时直接取出发送，跳过 handler 调用 + HttpResponse 构造 + 序列化
  // 适合内容不变的热点路径（首页、健康检查、静态文件）
  void GetStatic(const std::string& path, std::string body, const std::string& content_type = "text/plain");

  // 启动服务器（内部调用 server_.Start()，开始监听 + 接受连接）
  void Start();

 private:
  // 连接回调：有新连接建立或断开时被调用（当前为空实现）
  void OnConnection(const ConnectionPtr& c);

  // 消息回调：有数据到达时被调用，核心处理逻辑在这里
  // 循环从 Buffer 解析 HTTP 请求 → 路由分发 → 构造响应 → 发送
  // 支持 HTTP pipeline（一个 TCP 包里多个请求）
  void OnMessage(const ConnectionPtr& c, Buffer* buf);

  TcpServer server_;           // 内部持有的 TcpServer，负责连接管理和 IO
  HttpParser parser_;          // HTTP 协议解析器，从 Buffer 解析出 HttpRequest

  // 动态路由表：按 method 分成两张 map
  std::unordered_map<std::string, Handler> get_handlers_;   // GET 路由表
  std::unordered_map<std::string, Handler> post_handlers_;  // POST 路由表

  // 静态响应：预构建好的完整 HTTP 响应字符串
  struct StaticResponse {
    std::string keep;   // Connection: keep-alive 版本
    std::string close;  // Connection: close 版本
  };
  std::unordered_map<std::string, StaticResponse> static_get_;  // 静态 GET 路由表

  // GET "/" 的极致快路径：用 bool 判断跳过 hash 查找
  // 根路径是最热的（健康检查、首页），值得单独优化
  bool static_root_enabled_ = false;
  StaticResponse static_root_;
};

}  // namespace netx

#endif  // NETX_HTTP_SERVER_H_


