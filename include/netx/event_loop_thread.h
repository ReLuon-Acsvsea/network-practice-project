#ifndef NETX_EVENT_LOOP_THREAD_H_
#define NETX_EVENT_LOOP_THREAD_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace netx {

class EventLoop;

// EventLoopThread 管理一个独立线程并在其中运行单独的 EventLoop
class EventLoopThread {
 public:
  // cpu >= 0 表示绑定到指定 CPU；默认 -1 不绑定。
  explicit EventLoopThread(int cpu = -1);
  // 析构：停止线程并清理资源
  ~EventLoopThread();

  // 启动内部线程并创建 EventLoop，返回该线程内的 EventLoop 指针
  EventLoop* StartLoop();

 private:
  // 线程函数，在新线程中运行 EventLoop::Loop
  void ThreadFunc();

  // 工作线程对象
  std::thread th_;
  // 保护 loop_ 与 started_ 的互斥量
  std::mutex mu_;
  // 用于等待 EventLoop 创建完毕的条件变量
  std::condition_variable cv_;
  // 线程中实际运行的 EventLoop 指针
  EventLoop* loop_ = nullptr;
  // 是否已经启动
  bool started_ = false;
  // 绑定的 CPU 编号（-1 表示不绑核）
  int cpu_ = -1;
};

}  // namespace netx

#endif  // NETX_EVENT_LOOP_THREAD_H_


