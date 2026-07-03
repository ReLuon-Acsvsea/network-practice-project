#ifndef NETX_EPOLL_POLLER_H_
#define NETX_EPOLL_POLLER_H_

#include <sys/epoll.h>
#include <vector>
#include <cstdlib> 
namespace netx {

class Channel;

class EpollPoller{
public:
EpollPoller();
~EpollPoller();
EpollPoller(const EpollPoller &) = delete;
EpollPoller &operator=(const EpollPoller &)=delete;

void Add(Channel *ch, uint32_t events);
void Mod(Channel* ch, uint32_t events);
void Del(Channel* ch);

int Poll(int timeout_ms,const epoll_event** out_events);

private:
int epfd_;//epoll实例的文件描述符
std::vector<epoll_event> events_;//复用使用的事件数组缓冲区


};



}  // namespace netx

#endif  // NETX_EPOLL_POLLER_H_