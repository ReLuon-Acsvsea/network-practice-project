#ifndef NETX_IOURING_POLLER_H_
#define NETX_IOURING_POLLER_H_

#include <sys/epoll.h>

#include "netx/epoll_poller.h"  // WSL2 等环境下的回退实现

#if defined(NETX_USE_IOURING)
#include <liburing.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#else
#include <vector>
#endif

namespace netx {

class Channel;

#if defined(NETX_USE_IOURING)
// io_uring 版本的 Poller，实现与 EpollPoller 相同的接口，供 EventLoop 透明切换
class IoUringPoller {
 public:
  IoUringPoller();
  ~IoUringPoller();

  IoUringPoller(const IoUringPoller&) = delete;
  IoUringPoller& operator=(const IoUringPoller&) = delete;

  void Add(Channel* ch, uint32_t events);
  void Mod(Channel* ch, uint32_t events);
  void Del(Channel* ch);

  // 与 EpollPoller 对齐：返回事件个数，并输出指向线程局部事件数组的指针
  int Poll(int timeout_ms, const epoll_event** out_events);

 private:
  struct Watcher {
    Channel* ch;
    int fd;
    uint32_t id;
    uint32_t generation;
    uint32_t events;
    bool pending_delete;
    bool armed;
    uint64_t poll_user_data;
  };

  uint32_t AllocId();
  void SubmitPollAdd(Watcher* watcher);
  void SubmitPollRemove(Watcher* watcher);
  void HandlePollCqe(Watcher* watcher, uint32_t generation, int32_t res);
  void HandleRemoveCqe(Watcher* watcher, uint32_t generation, int32_t res);
  void MaybeCleanup(Watcher* watcher);
  struct io_uring_sqe* GetSqeOrThrow(const char* ctx);
  void ProcessCqe(struct io_uring_cqe* cqe);

  struct io_uring ring_;
  std::unordered_map<Channel*, std::unique_ptr<Watcher>> watchers_;
  std::unordered_map<uint32_t, Watcher*> id_index_;
  std::vector<epoll_event> ready_events_;
  uint32_t next_id_{1};
  bool use_epoll_fallback_{false};  // WSL2 等环境下 io_uring 对 TCP 不支持时回退
  EpollPoller fallback_;            // 回退用的 epoll poller
};

#else
// 未开启 NETX_USE_IOURING 时，保留 epoll 后备实现以便顺利编译链接
class IoUringPoller {
 public:
  IoUringPoller();
  ~IoUringPoller();

  IoUringPoller(const IoUringPoller&) = delete;
  IoUringPoller& operator=(const IoUringPoller&) = delete;

  void Add(Channel* ch, uint32_t events);
  void Mod(Channel* ch, uint32_t events);
  void Del(Channel* ch);

  int Poll(int timeout_ms, const epoll_event** out_events);

 private:
  EpollPoller fallback_;
};
#endif

}  // namespace netx

#endif  // NETX_IOURING_POLLER_H_


