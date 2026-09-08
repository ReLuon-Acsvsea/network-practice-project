#include "netx/epoll_poller.h"

#include "netx/channel.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "netx/logging.h"

namespace netx {

namespace {
// 将 epoll 事件标志转换为可读字符串
std::string EventsToString(uint32_t ev) {
  std::string s;
  auto add = [&](const char* name) {
    if (!s.empty()) s += "|";
    s += name;
  };
  if (ev & EPOLLIN) add("IN");
  if (ev & EPOLLPRI) add("PRI");
  if (ev & EPOLLOUT) add("OUT");
  if (ev & EPOLLERR) add("ERR");
  if (ev & EPOLLHUP) add("HUP");
  if (ev & EPOLLRDHUP) add("RDHUP");
  if (ev & EPOLLET) add("ET");
  if (ev & EPOLLONESHOT) add("ONESHOT");
  if (s.empty()) s = "0";
  return s;
}
#if defined(SYS_epoll_pwait2)
std::atomic<int> g_try_pwait2{-1};
#endif
} // namespace

// 构造函数：创建 epoll 实例，初始事件数组大小 128
EpollPoller::EpollPoller()
    : epfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(128) { // Initial size 128, grow as needed
  if (epfd_ < 0) throw std::runtime_error("epoll_create1 failed");
}

// 析构函数：关闭 epoll 实例
EpollPoller::~EpollPoller() { if (epfd_ >= 0) ::close(epfd_); }

// 添加 Channel 到 epoll：失败时自动降级为 MOD
void EpollPoller::Add(Channel* ch, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.ptr = ch;
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, ch->fd(), &ev) < 0) {
    if (errno == EEXIST) {
      LOG_WARN << "epoll_ctl ADD exists fd=" << ch->fd() << ", fallback MOD";
      if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, ch->fd(), &ev) < 0) {
        LOG_ERROR << "epoll_ctl MOD after ADD fail fd=" << ch->fd() << " err=" << std::strerror(errno);
      }
    } else {
      LOG_ERROR << "epoll_ctl ADD fail fd=" << ch->fd() << " err=" << std::strerror(errno);
    }
  } else {
    LOG_DEBUG << "epoll add fd=" << ch->fd() << " ev=" << EventsToString(ev.events) << " (" << ev.events << ")";
  }
}

// 修改 Channel 的事件：失败时自动降级为 ADD
void EpollPoller::Mod(Channel* ch, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.ptr = ch;
  if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, ch->fd(), &ev) < 0) {
    if (errno == ENOENT) {
      LOG_WARN << "epoll_ctl MOD noentry fd=" << ch->fd() << ", fallback ADD";
      if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, ch->fd(), &ev) < 0) {
        LOG_ERROR << "epoll_ctl ADD after MOD fail fd=" << ch->fd() << " err=" << std::strerror(errno);
      }
    } else {
      LOG_ERROR << "epoll_ctl MOD fail fd=" << ch->fd() << " err=" << std::strerror(errno);
    }
  } else {
    LOG_DEBUG << "epoll mod fd=" << ch->fd() << " ev=" << EventsToString(ev.events) << " (" << ev.events << ")";
  }
}

// 从 epoll 中删除 Channel
void EpollPoller::Del(Channel* ch) {
  if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, ch->fd(), nullptr) < 0) {
    if (errno != ENOENT) {
      LOG_WARN << "epoll_ctl DEL fail fd=" << ch->fd() << " err=" << std::strerror(errno);
    }
  } else {
    LOG_DEBUG << "epoll del fd=" << ch->fd();
  }
}

// 等待事件：优先尝试 epoll_pwait2（更高精度），降级为 epoll_wait,out_events传出参数
int EpollPoller::Poll(int timeout_ms, const epoll_event** out_events) {
  int num_events = 0;
#if defined(SYS_epoll_pwait2)
  if (g_try_pwait2.load(std::memory_order_relaxed) != 0) {
    struct timespec ts;
    struct timespec* tsp = nullptr;
    if (timeout_ms >= 0) {
      ts.tv_sec = timeout_ms / 1000;
      ts.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000L);
      tsp = &ts;
    }
    while (true) {
      // Use vector data
      long n = ::syscall(SYS_epoll_pwait2, epfd_, events_.data(), static_cast<int>(events_.size()), tsp, nullptr, static_cast<size_t>(0));
      if (n >= 0) {
        g_try_pwait2.store(1, std::memory_order_relaxed);
        num_events = static_cast<int>(n);
        goto finish;
      }
      if (errno == ENOSYS) {
        g_try_pwait2.store(0, std::memory_order_relaxed);
        break;
      }
      if (errno == EINTR) {
        continue; // interrupted, retry
      }
      LOG_WARN << "epoll_pwait2 error: " << std::strerror(errno) << ", fallback epoll_wait";
      g_try_pwait2.store(0, std::memory_order_relaxed);
      break;
    }
  }
#endif
  while (true) { //ubuntu20.04实际是用epoll_wait
    int n = ::epoll_wait(epfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
    if (n < 0) {
      if (errno == EINTR) {
        continue;  // interrupted by signal, retry
      }
      LOG_WARN << "epoll_wait error: " << std::strerror(errno);
      *out_events = nullptr;
      return 0;
    }
    num_events = n;
    break;
  }

finish:
  if (num_events > 0) {
    if (num_events == static_cast<int>(events_.size())) {
      events_.resize(events_.size() * 2);
    }
    *out_events = events_.data();
    LOG_DEBUG << "epoll_wait returned n=" << num_events;
  }
  return num_events;
}

}  // namespace netx
