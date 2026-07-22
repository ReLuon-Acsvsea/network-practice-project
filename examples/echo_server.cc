#include "netx/tcp_server.h"
#include "netx/event_loop.h"
#include "netx/tcp_connection.h"
#include "netx/logging.h"
#include "netx/worker_thread_pool.h"

#include <iostream>

using namespace netx;

int main(int argc, char* argv[]) {
  if (argc < 3 || argc > 4) {
    std::cerr << "Usage: " << argv[0] << " <io_threads> <reuse_port (0 or 1)> [biz_threads]" << std::endl;
    std::cerr << "Example: " << argv[0] << " 4 1 4" << std::endl;
    return -1; // Indicate failure due to incorrect usage
  }

  try {
    int io_threads = std::atoi(argv[1]);
    if (io_threads < 0) {
        std::cerr << "Error: io_threads must be a non-negative integer." << std::endl;
        return EXIT_FAILURE;
    }

    int reuse_port_flag = std::atoi(argv[2]);
    if (reuse_port_flag != 0 && reuse_port_flag != 1) {
        std::cerr << "Error: reuse_port must be 0 (false) or 1 (true)." << std::endl;
        return EXIT_FAILURE;
    }
    bool reuse_port = (reuse_port_flag != 0);

    int biz_threads = 0;
    if (argc == 4) {
      biz_threads = std::atoi(argv[3]);
      if (biz_threads < 0) {
        std::cerr << "Error: biz_threads must be a non-negative integer." << std::endl;
        return EXIT_FAILURE;
      }
    }

    std::cout << "Starting server with io_threads=" << io_threads
              << ", reuse_port=" << (reuse_port ? "true" : "false")
              << ", biz_threads=" << biz_threads << std::endl;

    Logger::set_level(LogLevel::kInfo);
    // 单线程模式优化：当 io_threads == 1 时，使用单线程优化（无原子操作）
    EventLoop loop(io_threads == 1);
    InetAddress addr(8081);
    TcpServer server(&loop, addr, io_threads, reuse_port);
    server.SetConnectionCallback([](const std::shared_ptr<TcpConnection>& c) {
    //   std::cout << "conn fd=" << c->fd() << std::endl;
    });
    if (biz_threads == 0) {
      server.SetMessageCallback([](const std::shared_ptr<TcpConnection>& c, Buffer* b) {
        size_t n = b->readable_bytes();
        // std::cout << "readable_bytes=" << n << std::endl;
        if (n == 0) return;
        c->Send(b->peek(), n);
        b->retrieve(n);
      });
    } else {
      static WorkerThreadPool biz;
      biz.Start(biz_threads);
      WorkerThreadPool* biz_ptr = &biz;
      server.SetMessageCallback([biz_ptr](const std::shared_ptr<TcpConnection>& c, Buffer* b) {
        size_t n = b->readable_bytes();
        if (n == 0) return;
        // 复制数据到独立缓冲，避免 Buffer 被复用覆盖
        auto sp = std::make_shared<std::string>(b->peek(), n);
        b->retrieve(n);
        std::cout << "Post biz_ptr: " << biz_ptr << std::endl;
        biz_ptr->Post([c, sp]() {
          // 业务线程处理（此处直接回显），回包放回到 IO 线程
          c->Send(*sp);  // 内部跨线程安全
        });
      });
    }
    server.Start();
    loop.Loop();
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << std::endl;
  }
}