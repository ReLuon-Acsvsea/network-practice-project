#include "netx/event_loop.h"

#include "netx/channel.h"

#include <chrono>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace netx {

namespace {
// 获取当前线程 ID（缓存以提高性能）
unsigned long Tid() {
  thread_local unsigned long cached = 0;
  if (cached == 0) {
#ifdef SYS_gettid
    cached = static_cast<unsigned long>(::syscall(SYS_gettid));
#else
    cached = static_cast<unsigned long>(
        ::syscall(186)); // fallback for older headers
#endif
  }
  return cached;
}
} // namespace

// 构造函数：创建 eventfd 用于跨线程唤醒
EventLoop::EventLoop(bool /*single_thread_mode*/)
    : wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      wakeup_channel_(nullptr),
      // events_(),
      tid_(Tid()),
      timer_wheel_(this) {
  wakeup_channel_ = new Channel(this, wakeup_fd_);
  wakeup_channel_->set_read_callback([this]() { HandleWakeup(); });
  wakeup_channel_->enable_reading();

  // Pre-reserve some space for tasks to avoid initial allocs
  pending_tasks_.reserve(16);
  pending_buffer_.reserve(16);
}

// 析构函数：清理资源
EventLoop::~EventLoop() {
  if (wakeup_channel_) {
    wakeup_channel_->disable_all();
    wakeup_channel_->remove();
    delete wakeup_channel_;
  }
  if (wakeup_fd_ >= 0)
    ::close(wakeup_fd_);
}

// 判断当前线程是否为事件循环所属线程
bool EventLoop::IsInLoopThread() const { return Tid() == tid_; }

// 事件循环主函数：等待并处理 IO 事件和待执行任务
void EventLoop::Loop() {
  while (!quit_) {
    // events_.clear();
    // 队列非空则立刻返回（timeout=0），否则给一个较长超时以减少空转
    bool empty;
    {
      std::lock_guard<std::mutex> lk(pending_mu_);
      empty = pending_tasks_.empty();//检查队列里有没有任务
    }

    // 计算超时时间：考虑定时器
    int timeout_ms = empty ? -1 : 0;
    if (empty) {
      // 获取最近的定时器超时时间
      int64_t timer_timeout = timer_wheel_.GetNextTimeout();
      if (timer_timeout >= 0) {
        // 使用定时器超时时间（最小 1ms，避免忙等）
        timeout_ms = static_cast<int>(std::max(static_cast<int64_t>(1), timer_timeout));
      }
    }

    const epoll_event* evs = nullptr;
    int n = poller_.Poll(timeout_ms, &evs);
    for (int i = 0; i < n; ++i) {
      const auto &ev = evs[i];
      auto *ch = static_cast<Channel *>(ev.data.ptr);
      ch->set_revents(ev.events);
      ch->handle_event();
    }

    // 处理定时器
    ProcessTimers();

    DoPendingTasks();//执行任务队列
  }
}

// 退出事件循环
void EventLoop::Quit() {
  quit_ = true;
  Wakeup();//把 epoll_wait 从阻塞中叫醒，让它有机会检查 quit_ 并退出
}

// 在事件循环中执行任务：同线程则立即执行，否则加入队列
void EventLoop::RunInLoop(Task cb) {
  if (IsInLoopThread()) {
    cb();
  } else {
    QueueInLoop(std::move(cb));
  }
}

// 将任务加入队列：跨线程时唤醒事件循环
void EventLoop::QueueInLoop(Task cb) {
  {
    std::lock_guard<std::mutex> lk(pending_mu_);
    pending_tasks_.emplace_back(std::move(cb));
  }

  // 跨线程入队：合并唤醒，避免重复写 eventfd
  // 或者如果正在处理 PendingTasks，也需要唤醒以确保立刻执行新任务
  if (!IsInLoopThread() || calling_pending_) {
    bool expected = false;
    if (wakeup_pending_.compare_exchange_strong(expected, true,
                                                std::memory_order_relaxed)) {
      Wakeup();
    }
  }
}

// 更新 Channel 的事件注册
void EventLoop::UpdateChannel(Channel *ch) {
  if (!ch->registered()) {
    if (ch->events() != 0) {
      poller_.Add(ch, ch->events());
      ch->set_registered(true);
      ch->set_registered_events(ch->events());
    }
  } else {
    if (ch->events() == 0) {
      poller_.Del(ch);
      ch->set_registered(false);
      ch->set_registered_events(0);
    } else {
      poller_.Mod(ch, ch->events());
      ch->set_registered_events(ch->events());
    }
  }
}

// 从 epoll 中移除 Channel
void EventLoop::RemoveChannel(Channel *ch) {
  if (ch->registered()) {
    poller_.Del(ch);
    ch->set_registered(false);
    ch->set_registered_events(0);
  }
}

// 唤醒事件循环：写入 eventfd
void EventLoop::Wakeup() {
  uint64_t one = 1;
  ::write(wakeup_fd_, &one, sizeof(one));
}

// 处理唤醒事件：读取 eventfd 并清除标志
void EventLoop::HandleWakeup() {
  uint64_t x;
  while (::read(wakeup_fd_, &x, sizeof(x)) > 0) {
  }
  // 清除挂起标志，允许后续唤醒
  wakeup_pending_.store(false, std::memory_order_relaxed);
}

// 执行待处理任务：批量交换并执行，减少锁竞争
void EventLoop::DoPendingTasks() {
  {
    std::lock_guard<std::mutex> lk(pending_mu_);
    if (pending_tasks_.empty())
      return;
    pending_tasks_.swap(pending_buffer_);
  }

  calling_pending_ = true;
// 批量处理所有待处理任务（无锁）
  for (auto &task : pending_buffer_) {
    task();
  }
  pending_buffer_.clear(); // clear content but keep capacity
  calling_pending_ = false;
}

// 添加延迟定时器
EventLoop::TimerId EventLoop::RunAfter(uint64_t delay_ms, Task cb) {
  return timer_wheel_.AddTimer(delay_ms, std::move(cb), false);
}

// 添加重复定时器
EventLoop::TimerId EventLoop::RunEvery(uint64_t interval_ms, Task cb) {
  return timer_wheel_.AddRepeatTimer(interval_ms, std::move(cb));
}

// 取消定时器
void EventLoop::CancelTimer(TimerId timer_id) {
  timer_wheel_.RemoveTimer(timer_id);
}

// 处理定时器
void EventLoop::ProcessTimers() {
  // 获取当前时间（毫秒）
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();
  timer_wheel_.Tick(static_cast<uint64_t>(ms));
}

} // namespace netx
