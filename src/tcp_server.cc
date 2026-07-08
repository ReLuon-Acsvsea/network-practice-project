#include "netx/tcp_server.h"
#include "netx/io_thread_pool.h"
#include "netx/event_loop.h"
#include "netx/tcp_connection.h"
#include "netx/acceptor.h"
namespace netx{
TcpServer::TcpServer(EventLoop* base_loop, const InetAddress& listen_addr,
            int io_threads, bool reuse_port)
:base_loop_(base_loop),io_pool_(new IoThreadPool(io_threads)),
listen_addr_(listen_addr),reuse_port_(reuse_port){}

TcpServer::~TcpServer() = default;

void TcpServer::RemoveConnectionInternal(const ConnectionPtr& conn) {
  RemoveConnection(conn);
}

 void TcpServer::Start(){
    io_pool_->Start();
    for(auto * loop:io_pool_->Loops()){
        loop_conns_.emplace(loop,std::unordered_map<int, ConnectionPtr>{});
    }
    if (reuse_port_){
        int n= io_pool_->Size();
        acceptors_.reserve(n);
        for(int i=0;i<n;++i){
            EventLoop *loop = io_pool_->NextLoop();
            auto acc= std::make_unique<Acceptor>(loop,listen_addr_,true);
            acc->SetNewConnCallback([this,loop](int fd,const InetAddress & peer){
                NewConnectionInLoop(loop, fd, peer);
            });
            acc->Listen();
            acceptors_.push_back(std::move(acc));
        }
    }else{
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

void TcpServer::NewConnectionInLoop(EventLoop* io, int fd, const InetAddress& /*peer*/) {

    
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
}