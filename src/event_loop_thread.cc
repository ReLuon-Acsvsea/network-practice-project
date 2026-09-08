#include "netx/event_loop_thread.h"

#include "netx/event_loop.h"

namespace netx {

// 构造函数：指定 CPU 亲和性（-1 表示不绑定）
EventLoopThread::EventLoopThread(int cpu) : cpu_(cpu) {}

// 析构函数：停止事件循环并等待线程退出
EventLoopThread::~EventLoopThread() {
  if (loop_) loop_->Quit();
  if (th_.joinable()) th_.join();
}

// 启动线程并返回事件循环指针
EventLoop* EventLoopThread::StartLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  if (started_) return loop_;
  th_ = std::thread([this]() { ThreadFunc(); });
  cv_.wait(lk, [this]() { return loop_ != nullptr; });
  started_ = true;
  return loop_;
}

// 线程函数：创建事件循环并运行
void EventLoopThread::ThreadFunc() {
  // 可选：绑定 CPU 亲和性（构造参数优先），否则可通过环境变量 NETX_BIND_CPU 指定单个 CPU
  int cpu_to_bind = cpu_;
  if (cpu_to_bind < 0) {
    if (const char* env = std::getenv("NETX_BIND_CPU")) {
      cpu_to_bind = std::atoi(env);
      if (cpu_to_bind < 0) cpu_to_bind = -1;
    }
  }
#ifdef __linux__
  if (cpu_to_bind >= 0) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<unsigned>(cpu_to_bind), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);// 把当前线程绑到指定 CPU 核心
  }
#endif
  EventLoop loop; // 栈上创建 EventLoop
  {
    std::lock_guard<std::mutex> lk(mu_);
    loop_ = &loop; // 把地址存到成员变量，让主线程能看到
  }
  cv_.notify_one(); // 通知主线程：loop_ 有值了，你可以用了
  loop.Loop(); // 开始事件循环（阻塞，一直跑到 Quit）
  {
    std::lock_guard<std::mutex> lk(mu_);
    loop_ = nullptr;// 循环结束，清空指针
  }
}

}  // namespace netx


