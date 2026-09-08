#include "netx/socket.h"

#include "netx/inet_address.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>

namespace netx {

// 构造函数：接管文件描述符
Socket::Socket(int fd) : fd_(fd) {}

// 析构函数：关闭文件描述符
Socket::~Socket() {
  if (fd_ >= 0) ::close(fd_);
}

// 创建非阻塞 TCP socket
int Socket::CreateNonblocking() {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  return fd;
}

// 创建非阻塞 TCP socket 并启用 SO_REUSEPORT
int Socket::CreateNonblockingReusePort() {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return fd;
  int on = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
  return fd;
}

// 绑定地址
void Socket::Bind(const InetAddress& addr) {
  if (::bind(fd_, addr.sockaddr(), addr.length()) < 0) {
    throw std::runtime_error("bind failed");
  }
}

// 开始监听
void Socket::Listen(int backlog) {
  if (::listen(fd_, backlog) < 0) {
    throw std::runtime_error("listen failed");
  }
}

// 接受新连接：返回非阻塞的客户端 fd
int Socket::Accept(InetAddress* peer) {
  socklen_t len = peer ? peer->length() : sizeof(sockaddr_in);
  sockaddr_in addr;
  sockaddr* sa = peer ? peer->sockaddr() : reinterpret_cast<sockaddr*>(&addr);
  int cfd = ::accept4(fd_, sa, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
  return cfd;
}

// 设置 SO_REUSEADDR 选项
void Socket::SetReuseAddr(bool on) {
  int v = on ? 1 : 0;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
}

// 设置 SO_REUSEPORT 选项
void Socket::SetReusePort(bool on) {
  int v = on ? 1 : 0;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &v, sizeof(v));
}

}  // namespace netx


