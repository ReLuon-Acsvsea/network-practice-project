#ifndef NETX_EVENT_LOOP_H_
#define NETX_EVENT_LOOP_H_

#include "netx/timer_wheel.h"

#include <atomic>
#include <functional>
#include <vector>
#include <mutex>

#include <sys/epoll.h>

#if defined(NETX_USE_IOURING)
#include "netx/iouring_poller.h"
using PollerImpl = netx::IoUringPoller;
#else
#include "netx/epoll_poller.h"
using PollerImpl = netx::EpollPoller;
#endif

namespace netx {

class Channel;

// EventLoop 为单线程 Reactor，负责轮询 IO 事件并执行回调与任务队列
class EventLoop {
 public:
  // Loop 内部任务类型（可被投递到 EventLoop 执行）
  using Task = std::function<void()>;
  // 定时器 ID 类型
  using TimerId = TimerWheel::TimerId;

  // single_thread_mode: true = 单线程模式（无原子操作，性能更好）
  //                    false = 多线程模式（使用原子操作，线程安全）
  // 创建一个 EventLoop，是否启用单线程优化由 single_thread_mode 决定
  explicit EventLoop(bool single_thread_mode = false);
  // 析构 EventLoop，关闭唤醒 fd 并清理资源
  ~EventLoop();

  // 事件循环主函数：阻塞轮询 IO 与任务队列，直到 Quit 被调用
  void Loop();
  // 请求退出循环：将 quit_ 置为 true 并唤醒
  void Quit();

  // 若当前在本线程，则立即执行任务；否则将任务投递到队列并唤醒
  void RunInLoop(Task cb);
  // 无论线程上下文如何，都将任务加入队列并在稍后执行
  void QueueInLoop(Task cb);

  // For Channel
  // 更新 Channel 的事件注册信息（add/mod）
  void UpdateChannel(Channel* ch);
  // 从 Poller 与内部结构中移除 Channel
  void RemoveChannel(Channel* ch);

  // 定时器接口
  // 添加延迟定时器：delay_ms 毫秒后执行 cb
  // 返回：定时器 ID，可用于取消定时器
  TimerId RunAfter(uint64_t delay_ms, Task cb);
  // 添加重复定时器：每 interval_ms 毫秒执行 cb
  // 返回：定时器 ID
  TimerId RunEvery(uint64_t interval_ms, Task cb);
  // 取消定时器
  void CancelTimer(TimerId timer_id);

  // 判断当前调用线程是否为 EventLoop 所属线程
  bool IsInLoopThread() const;

 private:
  // 向 eventfd 写入一个字节，唤醒 epoll_wait
  void Wakeup();
  // 处理唤醒事件（读取 eventfd，清除可读状态）
  void HandleWakeup();
  // 执行挂起任务队列中的所有任务
  void DoPendingTasks();
  // 处理定时器
  void ProcessTimers();

  // 用于跨线程唤醒 EventLoop 的 eventfd
  int wakeup_fd_;
  // 监听 wakeup_fd_ 的 Channel
  Channel* wakeup_channel_;
  // 底层 Poller 实现（epoll 或 io_uring 封装）
  PollerImpl poller_;
  // std::vector<epoll_event> events_;

  // 任务队列：统一使用 mutex + vector + 双缓冲
  // 互斥量：保护 pending_tasks_ 与 pending_buffer_
  std::mutex pending_mu_;
  // 当前待执行任务队列（生产者写入，消费者读取前交换到 buffer）
  std::vector<Task> pending_tasks_;
  // 临时缓冲队列：与 pending_tasks_ 交换以降低锁竞争
  std::vector<Task> pending_buffer_;

  // 唤醒合并标志：跨线程入队时仅在从 false->true 转变时写 eventfd
  std::atomic<bool> wakeup_pending_{false};

  // 单线程 IO 模型：以下标志只在所属 IO 线程访问，可用普通 bool 减少原子开销
  // 是否正在执行挂起任务
  bool calling_pending_{false};
  // 是否请求退出循环
  bool quit_{false};
  // EventLoop 所属线程的 tid（用于 IsInLoopThread 判断）
  const unsigned long tid_;

  // 定时器系统
  TimerWheel timer_wheel_;
};

}  // namespace netx

#endif  // NETX_EVENT_LOOP_H_
