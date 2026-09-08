#include "netx/io_thread_pool.h"

#include "netx/event_loop_thread.h"
#include <cstdlib>
#include <string>
#include <sstream>
#include <algorithm>

namespace netx {

// 构造函数：创建指定数量的 IO 线程，支持 CPU 亲和性绑定
IoThreadPool::IoThreadPool(int num_threads) {
  threads_.reserve(num_threads);
  loops_.reserve(num_threads);
  // 通过环境变量 NETX_IO_AFFINITY="0,1,2,3" 指定每个线程的 CPU 绑定；数量不足则不绑定剩余线程
  std::vector<int> cpus;
  if (const char* env = std::getenv("NETX_IO_AFFINITY")) {
    std::string s(env);
    std::replace(s.begin(), s.end(), ';', ',');
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (!item.empty()) {
        cpus.push_back(std::atoi(item.c_str()));
      }
    }
  }
  for (int i = 0; i < num_threads; ++i) {
    int cpu = (i < static_cast<int>(cpus.size())) ? cpus[i] : -1;
    threads_.emplace_back(new EventLoopThread(cpu));
  }
}

// 析构函数
IoThreadPool::~IoThreadPool() = default;

// 启动所有 IO 线程
void IoThreadPool::Start() {
  loops_.clear();
  for (auto& t : threads_) {
    loops_.push_back(t->StartLoop());
  }
}

// 轮询获取下一个事件循环
EventLoop* IoThreadPool::NextLoop() {
  if (loops_.empty()) return nullptr;
  EventLoop* loop = loops_[next_];
  next_ = (next_ + 1) % static_cast<int>(loops_.size());//循环使用
  return loop;
}

}  // namespace netx


