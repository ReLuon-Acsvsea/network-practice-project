#ifndef NETX_EPOLL_POLLER_H_
#define NETX_EPOLL_POLLER_H_

#include <sys/epoll.h>
#include <vector>
#include <cstdlib> 
namespace netx {

class Channel;

// EpollPoller 封装 epoll 的增删改与等待接口，为 EventLoop 提供统一轮询能力
class EpollPoller {
 public:
  // 构造函数：创建 epoll 实例并预分配事件数组
  EpollPoller();
  // 析构函数：关闭 epoll fd
  ~EpollPoller();

  EpollPoller(const EpollPoller&) = delete;
  EpollPoller& operator=(const EpollPoller&) = delete;

  // 将 Channel 注册到 epoll 中，关注指定事件
  void Add(Channel* ch, uint32_t events);
  // 修改已注册 Channel 的关注事件
  void Mod(Channel* ch, uint32_t events);
  // 从 epoll 中删除 Channel
  void Del(Channel* ch);

  // 返回事件个数，并通过 out_events 返回指向线程局部事件数组的指针（只在本次调用有效）
  int Poll(int timeout_ms, const epoll_event** out_events);

 private:
  // epoll 实例的文件描述符
  int epfd_;
  // 复用使用的事件数组缓冲区
  std::vector<epoll_event> events_;
};

}  // namespace netx

#endif  // NETX_EPOLL_POLLER_H_
