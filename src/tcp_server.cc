#include "netx/tcp_server.h"

#include "netx/acceptor.h"
#include "netx/event_loop.h"
#include "netx/io_thread_pool.h"
#include "netx/tcp_connection.h"

#include <unistd.h>

namespace netx {

// 构造函数：创建 TCP 服务器
TcpServer::TcpServer(EventLoop* base_loop, const InetAddress& listen_addr,
                     int io_threads, bool reuse_port)
    : base_loop_(base_loop),
      io_pool_(new IoThreadPool(io_threads)),
      listen_addr_(listen_addr),
      reuse_port_(reuse_port) {}

// 析构函数
TcpServer::~TcpServer() = default;

// 内部连接移除接口
void TcpServer::RemoveConnectionInternal(const ConnectionPtr& conn) {
  RemoveConnection(conn);
}

// 启动服务器：启动 IO 线程池，创建 Acceptor
void TcpServer::Start() {
  io_pool_->Start();
  // 预初始化每线程连接表，避免运行期间对外层容器的并发写
  for (auto* loop : io_pool_->Loops()) {
    loop_conns_.emplace(loop, std::unordered_map<int, ConnectionPtr>{});
  }
  if (reuse_port_) {
    // Each IO thread has one acceptor bound with SO_REUSEPORT
    int n = io_pool_->Size();
    acceptors_.reserve(n);
    for (int i = 0; i < n; ++i) {
      EventLoop* loop = io_pool_->NextLoop();
      auto acc = std::make_unique<Acceptor>(loop, listen_addr_, true);
      acc->SetNewConnCallback([this, loop](int fd, const InetAddress& peer) {
        NewConnectionInLoop(loop, fd, peer);
      });
      acc->Listen();
      acceptors_.push_back(std::move(acc));
    }
  } else {
    // Single acceptor on base loop
    auto acc = std::make_unique<Acceptor>(base_loop_, listen_addr_, false);
    acc->SetNewConnCallback([this](int fd, const InetAddress& peer) { NewConnection(fd, peer); });
    acc->Listen();
    acceptors_.push_back(std::move(acc));
  }
}

// 新连接回调（单 Acceptor 模式）：分发到 IO 线程
void TcpServer::NewConnection(int fd, const InetAddress& /*peer*/) {
  EventLoop* io = io_pool_->NextLoop();
  NewConnectionInLoop(io, fd, /*peer*/ InetAddress());
}

namespace {
// 轻量级消息分发回调：避免 std::function 开销
void TcpServerMessageDispatch(void* ctx,
                              const std::shared_ptr<TcpConnection>& c,
                              Buffer* b) {
  auto* server = static_cast<TcpServer*>(ctx);
  if (server && server->message_cb_) {
    server->message_cb_(c, b);
  }
}

// 轻量级关闭分发回调
void TcpServerCloseDispatch(void* ctx,
                            const std::shared_ptr<TcpConnection>& c) {
  auto* server = static_cast<TcpServer*>(ctx);
  if (server) {
    server->RemoveConnectionInternal(c);
  }
}
}  // namespace

// 在指定 IO 线程中创建连接对象
void TcpServer::NewConnectionInLoop(EventLoop* io, int fd, const InetAddress& /*peer*/) {
  // Ensure construction and channel registration happen on the target io loop
  io->QueueInLoop([this, fd, io]() {
    auto conn = std::make_shared<TcpConnection>(io, fd);// 引用计数 = 1
    conn->Tie(conn);// Channel 持有 weak_ptr
    // 内部使用轻量级函数指针回调，避免为每个连接构造 std::function lambda
    conn->set_raw_message_handler(this, &TcpServerMessageDispatch);// 注册回调
    conn->set_raw_close_handler(this, &TcpServerCloseDispatch);
    // 仅在所属 IO 线程的本地连接表中登记
    loop_conns_.at(io).emplace(conn->fd(), conn);// map 持有 → 引用计数 = 2
    if (connection_cb_) connection_cb_(conn);

  // 之后某个时刻，回调 lambda 里可能也持有 conn → 引用计数 = 3
  // 再之后对端断开 → map erase → 引用计数 = 2
  // 回调执行完 → 引用计数 = 1
  // lambda 销毁 → 引用计数 = 0 → 对象析构
  //如果用裸指针，map erase 后对象就没了，但回调可能还在用。shared_ptr保证最后一个使用者放手才销毁。
  });
}

// 移除连接：在所属 IO 线程中执行
void TcpServer::RemoveConnection(const ConnectionPtr& conn) {
  EventLoop* io = conn->loop();
  // 在所属 IO 线程中移除连接并回调
  io->QueueInLoop([this, conn, io]() {
    auto it_map = loop_conns_.find(io);
    if (it_map != loop_conns_.end()) {
      it_map->second.erase(conn->fd());
    }
    if (connection_cb_) connection_cb_(conn);
  });
}

}  // namespace netx


