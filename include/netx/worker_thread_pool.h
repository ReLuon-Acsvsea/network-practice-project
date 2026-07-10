#ifndef NETX_WORKER_THREAD_POOL_H_
#define NETX_WORKER_THREAD_POOL_H_

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace netx{

class WorkerThreadPool{
public:
    using Task = std::function<void()>;
    WorkerThreadPool()=default;
    ~WorkerThreadPool();
    void Start(int n);
    void Stop();
    void Post(Task t);
private:
    void Worker();
    std::vector<std::thread> workers_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task>q_;
    bool stopping_ = false;//控制woker线程退出的标志位
};
}//netx
#endif