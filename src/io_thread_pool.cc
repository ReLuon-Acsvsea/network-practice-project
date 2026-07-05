#include "netx/io_thread_pool.h"
#include "netx/event_loop_thread.h"
#include <string>
#include <algorithm>
#include <sstream>
namespace netx{
IoThreadPool::IoThreadPool(int num_threads){
    threads_.reserve(num_threads);
    loops_.reserve(num_threads);

    std::vector<int>cpus;
    //将“0;1;2"编程cpus=[0,1,2]
    if(const char *env = std::getenv("ETX_IO_AFFINITY")){
        std::string s(env);
        std::replace(s.begin(),s.end(),';',',');
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss,item,',')){
            if(!item.empty()){
                cpus.push_back(std::atoi(item.c_str()));
            }
        }
    }
    for (int i = 0; i < num_threads; ++i) {
        int cpu = (i < static_cast<int>(cpus.size())) ? cpus[i] : -1;
        threads_.emplace_back(new EventLoopThread(cpu));
    }
}
IoThreadPool::~IoThreadPool()=default;

void IoThreadPool::Start(){
    loops_.clear();
    for(auto & t: threads_){
        loops_.push_back(t->StartLoop());
    }
}

EventLoop* IoThreadPool::NextLoop(){
    if(loops_.empty()) return nullptr;
    EventLoop * loop =loops_[next_];
    next_ = (next_+1)%static_cast<int>(loops_.size());
    return loop;
}
    
}