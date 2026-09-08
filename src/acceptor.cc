#include "netx/acceptor.h"

#include "netx/channel.h"
#include "netx/event_loop.h"
#include "netx/socket.h"
#include "netx/logging.h"
#include <unistd.h>
#include <netinet/tcp.h>

namespace netx {

// 构造函数：创建监听套接字并绑定地址
// loop: 所属的事件循环
// listen_addr: 监听地址
// reuse_port: 是否启用 SO_REUSEPORT（多线程accept优化）
Acceptor::Acceptor(EventLoop* loop, const InetAddress& listen_addr, bool reuse_port)
    : loop_(loop), listen_addr_(listen_addr) {
  int fd = reuse_port ? Socket::CreateNonblockingReusePort() : Socket::CreateNonblocking();
  listen_sock_ = std::make_unique<Socket>(fd);
  listen_sock_->SetReuseAddr(true);
  if (reuse_port) listen_sock_->SetReusePort(true);
  listen_sock_->Bind(listen_addr_);
  listen_channel_ = std::make_unique<Channel>(loop_, listen_sock_->fd());
  listen_channel_->set_read_callback([this]() { HandleRead(); });
}

// 析构函数
Acceptor::~Acceptor() {}

// 开始监听：调用 listen() 并启用可读事件监听
void Acceptor::Listen() {
  listening_ = true;
  listen_sock_->Listen();
  listen_channel_->enable_reading();
  LOG_INFO << "Acceptor listening fd=" << listen_sock_->fd()
           << " addr=" << listen_addr_.to_ip_port();
}

// 处理可读事件：循环 accept 新连接并回调上层
void Acceptor::HandleRead() {
  while (true) {
    InetAddress peer;
    int cfd = listen_sock_->Accept(&peer);
    if (cfd < 0) {
      LOG_DEBUG << "accept returns cfd=" << cfd << ", stop accepting";
      break;
    }
    // 默认开启 TCP_NODELAY 以降低小包延迟
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    LOG_DEBUG << "accepted fd=" << cfd << " from " << peer.to_ip_port();
    if (new_conn_cb_) new_conn_cb_(cfd, peer);
    else ::close(cfd);
  }
}

}  // namespace netx


