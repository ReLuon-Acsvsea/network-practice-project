#include "netx/worker_thread_pool.h"
#include <string>
#include <algorithm>
#include <sstream>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
namespace netx{

WorkerThreadPool::~WorkerThreadPool(){Stop();}

void WorkerThreadPool::Start(int n){
    std::lock_guard<std::mutex> lk(mu_);
    stopping_ =false;
    std::vector<int> cpus;
    if(const char* env = std::getenv("NETX_BIZ_AFFINITY")){
        std::string s(env);
        std::replace(s.begin(),s.end(),';',',');
        std::stringstream ss(s);
        std::string item;
        while(std::getline(ss,item,',')){
            if(!item.empty()) cpus.push_back(std::atoi(item.c_str()));
        }
    }
    for( int i=0; i<n; ++i){
        int cpu =(i<static_cast<int>(cpus.size()))?cpus[i]:-1;
        workers_.emplace_back([this,cpu](){
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

void WorkerThreadPool::Stop(){
    {
        std::lock_guard<std::mutex>lk(mu_);
        stopping_ =true;
    }
    cv_.notify_all();
    for(auto & t:workers_) if(t.joinable()) t.join();
    workers_.clear();
}
void WorkerThreadPool::Post(Task t) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.emplace_back(std::move(t));
    }
    cv_.notify_one();
}

void WorkerThreadPool::Worker() {
    while(true){
        Task task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk,[this](){return stopping_ || !q_.empty();});
            if(stopping_ && q_.empty()) break;
            task = std::move(q_.front());
            q_.pop_front();
        }
        task();
    }
}
}