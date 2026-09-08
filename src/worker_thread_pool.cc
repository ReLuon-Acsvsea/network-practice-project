#include "netx/worker_thread_pool.h"

#include <cstdlib>
#include <string>
#include <sstream>
#include <algorithm>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace netx {

// 析构函数：停止所有工作线程
WorkerThreadPool::~WorkerThreadPool() { Stop(); }

// 启动指定数量的工作线程，支持 CPU 亲和性绑定
void WorkerThreadPool::Start(int n) {
  std::lock_guard<std::mutex> lk(mu_);
  stopping_ = false;
  // 解析 CPU 亲和（可选）：NETX_BIZ_AFFINITY="0,1,2,3"
  std::vector<int> cpus;
  if (const char* env = std::getenv("NETX_BIZ_AFFINITY")) {
    std::string s(env);
    std::replace(s.begin(), s.end(), ';', ',');
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (!item.empty()) cpus.push_back(std::atoi(item.c_str()));
    }
  }
  for (int i = 0; i < n; ++i) {
    int cpu = (i < static_cast<int>(cpus.size())) ? cpus[i] : -1;
    workers_.emplace_back([this, cpu]() {
#ifdef __linux__
      if (cpu >= 0) {
        cpu_set_t set;//创建cpu集合
        CPU_ZERO(&set);//清空集合
        CPU_SET(static_cast<unsigned>(cpu), &set);//把指定核心加入集合
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);//把当前线程绑定到集合里的核心
      }
#endif
      Worker();
    });
  }
}

// 停止线程池：等待所有线程退出
void WorkerThreadPool::Stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_) if (t.joinable()) t.join();
  workers_.clear();
}

// 提交任务到队列
void WorkerThreadPool::Post(Task t) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    q_.emplace_back(std::move(t));
  }
  cv_.notify_one();
}

// 工作线程函数：循环取任务并执行
void WorkerThreadPool::Worker() {
  while (true) {
    Task task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this]() { return stopping_ || !q_.empty(); });
      if (stopping_ && q_.empty()) break;// 只有"关闭 + 队列空"才退出
      task = std::move(q_.front());
      q_.pop_front();
    }
    task();
  }
}

}  // namespace netx


