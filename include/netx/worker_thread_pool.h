// 程序员老廖：https://space.bilibili.com/3494351095204205
#ifndef NETX_WORKER_THREAD_POOL_H_
#define NETX_WORKER_THREAD_POOL_H_

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace netx {

// WorkerThreadPool 管理一组通用工作线程，用于执行计算或业务逻辑任务
class WorkerThreadPool {
public:
  using Task = std::function<void()>;

  WorkerThreadPool() = default;
  ~WorkerThreadPool();

  void Start(int n);
  void Stop();
  void Post(Task t);

private:
  void Worker();

  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Task> q_;
  bool stopping_ = false;//控制 worker 线程退出的标志位，true 表示正在停止，worker 线程看到这个标志后会退出循环并结束线程。Post() 里也会检查这个标志，拒绝在停止过程中提交新任务。
};

} // namespace netx

#endif // NETX_WORKER_THREAD_POOL_H_
