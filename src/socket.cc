#include "netx/socket.h"

#include "netx/inet_address.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>

namespace netx {

Socket::Socket(int fd):fd_(fd){}

Socket::~Socket(){
    if(fd_>=0) ::close(fd_);
}

int Socket::CreateNonblocking(){
    int fd = ::socket(AF_INET, SOCK_STREAM |SOCK_NONBLOCK |SOCK_CLOEXEC, 0);
    return fd;
}
int Socket::CreateNonblockingReusePort(){
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return fd;
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    return fd;
}
void Socket::Bind(const InetAddress &addr){
    if(::bind(fd_,addr.sockaddr(),addr.length())<0){
        throw std::runtime_error("bind failed");
    }
}
void Socket::Listen(int backlog = 1024){
    if(::listen(fd_,backlog)<0){
        throw std::runtime_error("listen failed");
    }
}
int Socket::Accept(InetAddress * peer){
    socklen_t len= peer?peer->length():sizeof(sockaddr_in);
    sockaddr_in addr;
    sockaddr * sa =peer ?peer->sockaddr():reinterpret_cast<sockaddr*>(&addr);
    int cfd = ::accept4(fd_,sa,&len,SOCK_NONBLOCK | SOCK_CLOEXEC);
    return cfd;
}

void Socket::SetReuseAddr(bool on){
    int v = on ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
}
void Socket::SetReusePort(bool on){
    int v = on ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &v, sizeof(v));
}

}


