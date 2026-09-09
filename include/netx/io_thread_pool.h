#ifndef NETX_IO_THREAD_POOL_H_
#define NETX_IO_THREAD_POOL_H_

#include <memory>
#include <vector>

namespace netx {

class EventLoop;
class EventLoopThread;

// IoThreadPool 管理一组 IO 线程，每个线程内运行一个独立的 EventLoop
class IoThreadPool {
 public:
  explicit IoThreadPool(int num_threads);
  ~IoThreadPool();

  void Start();
  EventLoop* NextLoop();
  int Size() const { return static_cast<int>(threads_.size()); }
  const std::vector<EventLoop*>& Loops() const { return loops_; }

 private:
  std::vector<std::unique_ptr<EventLoopThread>> threads_;// N 个线程
  std::vector<EventLoop*> loops_; // N 个 EventLoop
  int next_ = 0;// 轮询计数器
};

}  // namespace netx

#endif  // NETX_IO_THREAD_POOL_H_


