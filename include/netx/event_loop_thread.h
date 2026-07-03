#ifndef NETX_EVENT_LOOP_THREAD_H_
#define NETX_EVENT_LOOP_THREAD_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace netx {

class EventLoop;

// EventLoopThread 管理一个独立线程并在其中运行单独的 EventLoop
class EventLoopThread
{
private:
    void ThreadFunc();//在新线程中运行EventLoop::Loop
    std::thread th_;//工作线程对象
    std::mutex mu_;//保护loop_与started_的胡质量
    std::condition_variable cv_;//用于等待EventLoop创建完毕的条件变量
    EventLoop *loop_ =nullptr;//线程中实际运行的EventLoop指针
    bool started_ = false;//是否已经启动
    int cpu_=-1;

public:
    explicit EventLoopThread(int cpu = -1);
    ~EventLoopThread();
    EventLoop * StartLoop();
};

}


#endif