#ifndef NETX_SOCKET_H_
#define NETX_SOCKET_H_

#include <cstdint>

namespace netx {

class InetAddress;

class Socket{
  private:
    int fd_;
  public:
    explicit Socket(int fd);
    ~Socket();

    Socket(const Socket &) =delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const { return fd_;}
    static int CreateNonblocking();
    static int CreateNonblockingReusePort();

    void Bind(const InetAddress &addr);
    void Listen(int backlog = 1024);
    int Accept(InetAddress * peer);

    void SetReuseAddr(bool on);
    void SetReusePort(bool on);

};

}
#endif