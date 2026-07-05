#include "netx/iouring_poller.h"

#if defined(NETX_USE_IOURING)

#include "netx/channel.h"
#include "netx/logging.h"

#include <errno.h>
#include <linux/time_types.h>
#include <poll.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace netx {

namespace {
constexpr unsigned kQueueDepth = 256;

enum class RequestKind : uint8_t { kPoll = 0, kRemove = 1 };

uint64_t EncodeUserData(uint32_t id, uint32_t generation, RequestKind kind) {
  constexpr uint64_t kKindMask = 0x3ULL;
  constexpr uint64_t kGenMask = (1ULL << 30) - 1;  // 30 bits for generation
  uint64_t data = (static_cast<uint64_t>(id) << 32);
  data |= (static_cast<uint64_t>(generation & kGenMask) << 2);
  data |= (static_cast<uint64_t>(kind) & kKindMask);
  return data;
}

void DecodeUserData(uint64_t data, uint32_t* id, uint32_t* generation, RequestKind* kind) {
  *kind = static_cast<RequestKind>(data & 0x3ULL);
  *generation = static_cast<uint32_t>((data >> 2) & ((1ULL << 30) - 1));
  *id = static_cast<uint32_t>(data >> 32);
}

uint32_t EpollToPoll(uint32_t ev) {
  uint32_t mask = POLLERR | POLLHUP | POLLRDHUP;
  if (ev & EPOLLIN) mask |= POLLIN;
  if (ev & EPOLLPRI) mask |= POLLPRI;
  if (ev & EPOLLOUT) mask |= POLLOUT;
  return mask;
}

uint32_t PollMaskToEpoll(uint32_t mask) {
  uint32_t ev = 0;
  if (mask & (POLLIN | POLLPRI)) ev |= EPOLLIN;
  if (mask & POLLOUT) ev |= EPOLLOUT;
  if (mask & POLLERR) ev |= EPOLLERR;
  if (mask & POLLHUP) ev |= EPOLLHUP;
  if (mask & POLLRDHUP) ev |= EPOLLRDHUP;
  return ev;
}

std::string IoErrorString(int ret) {
  int err = ret < 0 ? -ret : ret;
  return std::string(std::strerror(err));
}
}  // namespace

IoUringPoller::IoUringPoller() : ready_events_(128) {
  int rc = ::io_uring_queue_init(kQueueDepth, &ring_, 0);
  if (rc < 0) {
    throw std::runtime_error("io_uring_queue_init failed: " + IoErrorString(rc));
  }
  LOG_INFO << "io_uring initialized, depth=" << kQueueDepth;
}

IoUringPoller::~IoUringPoller() { ::io_uring_queue_exit(&ring_); }

uint32_t IoUringPoller::AllocId() {
  uint32_t id = next_id_++;
  if (id == 0) {
    id = next_id_++;
  }
  return id;
}

struct io_uring_sqe* IoUringPoller::GetSqeOrThrow(const char* ctx) {
  io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
  if (sqe) return sqe;
  int submit_rc = ::io_uring_submit(&ring_);
  if (submit_rc < 0) {
    throw std::runtime_error(std::string("io_uring_submit failed (") + ctx + "): " +
                             IoErrorString(submit_rc));
  }
  sqe = ::io_uring_get_sqe(&ring_);
  if (!sqe) {
    throw std::runtime_error(std::string("io_uring_get_sqe failed (") + ctx + ")");
  }
  return sqe;
}

void IoUringPoller::SubmitPollAdd(Watcher* watcher) {
  if (watcher->events == 0 || watcher->pending_delete) return;
  auto* sqe = GetSqeOrThrow("poll_add");
  watcher->generation += 1;
  if (watcher->generation == 0) watcher->generation = 1;
  uint64_t user_data = EncodeUserData(watcher->id, watcher->generation, RequestKind::kPoll);
  watcher->poll_user_data = user_data;
  uint32_t poll_mask = EpollToPoll(watcher->events);
  ::io_uring_prep_poll_add(sqe, watcher->fd, poll_mask);
  sqe->user_data = user_data;
  watcher->armed = true;
  LOG_DEBUG << "uring poll_add fd=" << watcher->fd
            << " mask=" << std::hex << poll_mask << std::dec
            << " id=" << watcher->id << " gen=" << watcher->generation
            << " user=" << user_data;
}

void IoUringPoller::SubmitPollRemove(Watcher* watcher) {
  if (!watcher->armed || watcher->poll_user_data == 0) return;
  auto* sqe = GetSqeOrThrow("poll_remove");
  ::io_uring_prep_poll_remove(sqe, watcher->poll_user_data);
  sqe->user_data = EncodeUserData(watcher->id, watcher->generation, RequestKind::kRemove);
  LOG_DEBUG << "uring poll_remove fd=" << watcher->fd
            << " id=" << watcher->id << " gen=" << watcher->generation
            << " user=" << watcher->poll_user_data;
}

void IoUringPoller::Add(Channel* ch, uint32_t events) {
  if (events == 0) return;
  if (watchers_.count(ch)) {
    Mod(ch, events);
    return;
  }
  auto watcher = std::make_unique<Watcher>();
  watcher->ch = ch;
  watcher->fd = ch->fd();
  watcher->events = events;
  watcher->pending_delete = false;
  watcher->armed = false;
  watcher->generation = 0;
  watcher->poll_user_data = 0;
  watcher->id = AllocId();
  Watcher* raw = watcher.get();
  watchers_[ch] = std::move(watcher);
  id_index_[raw->id] = raw;
  SubmitPollAdd(raw);
  LOG_DEBUG << "uring add ch=" << ch << " fd=" << raw->fd << " events=" << events;
}

void IoUringPoller::Mod(Channel* ch, uint32_t events) {
  auto it = watchers_.find(ch);
  if (it == watchers_.end()) {
    Add(ch, events);
    return;
  }
  auto* watcher = it->second.get();
  if (events == 0) {
    Del(ch);
    return;
  }
  watcher->events = events;
  SubmitPollRemove(watcher);
  SubmitPollAdd(watcher);
  LOG_DEBUG << "uring mod fd=" << watcher->fd << " events=" << events;
}

void IoUringPoller::Del(Channel* ch) {
  auto it = watchers_.find(ch);
  if (it == watchers_.end()) return;
  auto* watcher = it->second.get();
  watcher->events = 0;
  watcher->pending_delete = true;
  SubmitPollRemove(watcher);
  if (!watcher->armed) {
    MaybeCleanup(watcher);
  }
  LOG_DEBUG << "uring del fd=" << watcher->fd;
}

void IoUringPoller::ProcessCqe(io_uring_cqe* cqe) {
  uint32_t id = 0;
  uint32_t generation = 0;
  RequestKind kind = RequestKind::kPoll;
  DecodeUserData(cqe->user_data, &id, &generation, &kind);
  auto it = id_index_.find(id);
  if (it == id_index_.end()) {
    return;
  }
  auto* watcher = it->second;
  LOG_DEBUG << "uring cqe kind=" << (kind == RequestKind::kPoll ? "poll" : "remove")
            << " id=" << id << " gen=" << generation << " res=" << cqe->res
            << " fd=" << watcher->fd;
  if (kind == RequestKind::kPoll) {
    HandlePollCqe(watcher, generation, cqe->res);
  } else {
    HandleRemoveCqe(watcher, generation, cqe->res);
  }
}

void IoUringPoller::HandlePollCqe(Watcher* watcher, uint32_t generation, int32_t res) {
  if (generation != watcher->generation) {
    return;  // 旧的 poll 事件，忽略
  }
  watcher->armed = false;
  if (res < 0) {
    // 负返回表示错误（常见为 -EBADF/-ENOENT）。这通常出现在 fd 已关闭或被取消时。
    // 为避免对已关闭 Channel 分发错误事件而产生悬空指针，这里不向上层投递事件。
    LOG_WARN << "io_uring poll error fd=" << watcher->fd << " err=" << IoErrorString(res);
    MaybeCleanup(watcher);
    return;
  }
  uint32_t events = PollMaskToEpoll(static_cast<uint32_t>(res));
  if (events == 0) {
    // 无有效事件时不投递
    MaybeCleanup(watcher);
    return;
  }
  LOG_DEBUG << "uring poll_cqe fd=" << watcher->fd
            << " res=" << std::hex << res << " -> ev=" << events << std::dec;
  if (!watcher->pending_delete) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = watcher->ch;
    ready_events_.push_back(ev);
    // 仅在正常事件时重新武装；错误已在上面 return
    if (watcher->events != 0) {
      SubmitPollAdd(watcher);
    }
  } else {
    MaybeCleanup(watcher);
  }
}

void IoUringPoller::HandleRemoveCqe(Watcher* watcher, uint32_t generation, int32_t res) {
  if (res < 0 && res != -ENOENT) {
    LOG_WARN << "io_uring poll_remove fd=" << watcher->fd
             << " err=" << IoErrorString(res);
  }
  if (generation == watcher->generation) {
    watcher->armed = false;
  }
  MaybeCleanup(watcher);
}

void IoUringPoller::MaybeCleanup(Watcher* watcher) {
  if (!watcher->pending_delete || watcher->armed) return;
  // 在从容器中擦除之前先缓存必要信息，避免 use-after-free
  int fd = watcher->fd;
  Channel* ch = watcher->ch;
  id_index_.erase(watcher->id);
  watchers_.erase(ch);
  LOG_DEBUG << "uring cleanup fd=" << fd;
}

int IoUringPoller::Poll(int timeout_ms, const epoll_event** out_events) {
  ready_events_.clear();
  int submit_rc = ::io_uring_submit(&ring_);
  if (submit_rc < 0) {
    LOG_ERROR << "io_uring_submit error: " << IoErrorString(submit_rc);
  }

  io_uring_cqe* cqe = nullptr;
  int wait_rc = 0;
  do {
    if (timeout_ms >= 0) {
      __kernel_timespec ts{};
      ts.tv_sec = timeout_ms / 1000;
      ts.tv_nsec = static_cast<long long>(timeout_ms % 1000) * 1000000LL;
      wait_rc = ::io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
    } else {
      wait_rc = ::io_uring_wait_cqe(&ring_, &cqe);
    }
  } while (wait_rc == -EINTR);

  if (wait_rc == -ETIME || cqe == nullptr) {
    LOG_DEBUG << "uring poll timeout ms=" << timeout_ms;
    *out_events = nullptr;
    return 0;
  }
  if (wait_rc < 0) {
    LOG_WARN << "io_uring wait error: " << IoErrorString(wait_rc);
    *out_events = nullptr;
    return 0;
  }

  ProcessCqe(cqe);
  ::io_uring_cqe_seen(&ring_, cqe);
  while (::io_uring_peek_cqe(&ring_, &cqe) == 0) {
    ProcessCqe(cqe);
    ::io_uring_cqe_seen(&ring_, cqe);
  }

  if (ready_events_.empty()) {
    *out_events = nullptr;
    return 0;
  }
  *out_events = ready_events_.data();
  LOG_DEBUG << "uring poll ready n=" << ready_events_.size();
  return static_cast<int>(ready_events_.size());
}

}  // namespace netx

#else

#include "netx/epoll_poller.h"

namespace netx {

IoUringPoller::IoUringPoller() = default;
IoUringPoller::~IoUringPoller() = default;

void IoUringPoller::Add(Channel* ch, uint32_t events) { fallback_.Add(ch, events); }
void IoUringPoller::Mod(Channel* ch, uint32_t events) { fallback_.Mod(ch, events); }
void IoUringPoller::Del(Channel* ch) { fallback_.Del(ch); }

int IoUringPoller::Poll(int timeout_ms, const epoll_event** out_events) {
  return fallback_.Poll(timeout_ms, out_events);
}

}  // namespace netx

#endif  // NETX_USE_IOURING
