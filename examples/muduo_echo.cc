#include <boost/bind.hpp>
#include <iostream>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>
class EchoServer {
public:
  EchoServer(muduo::net::EventLoop *loop,
             const muduo::net::InetAddress &listenAddr)
      : server_(loop, listenAddr, "EchoServer") {
    server_.setConnectionCallback(
        boost::bind(&EchoServer::onConnection, this, _1));
    server_.setMessageCallback(
        boost::bind(&EchoServer::onMessage, this, _1, _2, _3));
  }
  void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
  void start() { server_.start(); }

private:
  void onConnection(const muduo::net::TcpConnectionPtr &conn) {
    LOG_INFO << "Connection " << (conn->connected() ? "UP" : "DOWN");
  }
  void onMessage(const muduo::net::TcpConnectionPtr &conn,
                 muduo::net::Buffer *buf, muduo::Timestamp time) {
    muduo::string msg(buf->retrieveAllAsString());
    conn->send(msg);
  }
  muduo::net::TcpServer server_;
};
// 编译 g++ muduo_echo.cc -lmuduo_net -lmuduo_base -lpthread -std=c++11 -o
// muduo_echo

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <thread_number>\n";
    std::cerr << "Example: " << argv[0] << " 4\n";
    return -1; // Indicate error
  }
  int thread_num = std::atoi(argv[1]);
  if (thread_num < 0) { // Basic validation
    std::cerr << "Error: Number of threads must be non-negative.\n";
    return -1;
  }
  LOG_INFO << "Starting EchoServer with " << thread_num << " thread(s)...";
  muduo::net::EventLoop loop;
  muduo::net::InetAddress listenAddr(8082);
  EchoServer server(&loop, listenAddr);
  server.setThreadNum(thread_num);
  server.start();
  loop.loop();
}