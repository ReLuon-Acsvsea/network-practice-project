// 连接超时测试
//
// 测试功能：
// 1. TcpConnection 的超时机制
// 2. TcpServer 的连接超时配置
// 3. 心跳检测机制
//
// 测试方法：
// 1. 启动服务器，设置 5 秒超时
// 2. 客户端连接后不发送数据
// 3. 观察服务器是否在 5 秒后关闭连接

#include <chrono>
#include <iostream>
#include <thread>

#include "netx/event_loop.h"
#include "netx/inet_address.h"
#include "netx/logging.h"
#include "netx/tcp_connection.h"
#include "netx/tcp_server.h"

using netx::EventLoop;
using netx::InetAddress;
using netx::TcpConnection;
using netx::TcpServer;

int main() {
  LOG_INFO << "Connection timeout test started";

  EventLoop loop;
  InetAddress addr(9000);
  TcpServer server(&loop, addr, 1, false);

  // 设置连接超时：5 秒
  server.SetConnectionTimeout(5000);

  // 连接回调
  server.SetConnectionCallback([](const TcpServer::ConnectionPtr& conn) {
    if (conn->IsConnected()) {
      LOG_INFO << "New connection fd=" << conn->fd()
               << " timeout will be in 5 seconds";
    } else {
      LOG_INFO << "Connection closed fd=" << conn->fd();
    }
  });

  // 消息回调
  server.SetMessageCallback(
      [](const TcpServer::ConnectionPtr& conn, netx::Buffer* buf) {
        std::string msg(buf->peek(), buf->readable_bytes());
        buf->retrieve_all();
        LOG_INFO << "Received from fd=" << conn->fd() << ": " << msg;

        // 收到数据后重置超时
        conn->ResetTimeout();
        LOG_INFO << "Timeout reset for fd=" << conn->fd();

        // 回显数据
        conn->Send("Echo: " + msg);
      });

  server.Start();
  LOG_INFO << "Server started on port 9000, timeout=5s";
  LOG_INFO << "Connect with: nc localhost 9000";
  LOG_INFO << "Wait 5 seconds without sending data to see timeout";

  loop.Loop();
  return 0;
}
