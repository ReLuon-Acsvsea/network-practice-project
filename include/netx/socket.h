#ifndef NETX_SOCKET_H_
#define NETX_SOCKET_H_

#include <cstdint>

namespace netx {

class InetAddress;

// Socket 封装 TCP 套接字常用操作，负责 fd 生命周期管理与选项设置
class Socket {
 public:
  explicit Socket(int fd);
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  int fd() const { return fd_; }

  static int CreateNonblocking();
  static int CreateNonblockingReusePort();

  void Bind(const InetAddress& addr);
  void Listen(int backlog = 65535);
  int Accept(InetAddress* peer);

  void SetReuseAddr(bool on);
  void SetReusePort(bool on);

 private:
  int fd_;
};

}  // namespace netx

#endif  // NETX_SOCKET_H_


