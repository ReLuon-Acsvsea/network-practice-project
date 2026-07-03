#include "netx/event_loop.h"

#include "netx/channel.h"

#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace netx{

namespace {

//获取当前线程id
unsigned long Tid(){
    thread_local unsigned long cached =0;
    if(cached==0){
#ifdef SYS_gettid
    cached = static_cast<unsigned long>(::syscall(SYS_gettid));
#else
    cached = static_cast<unsigned long>(
        ::syscall(186)); // fallback for older headers
#endif
    }
    return cached;
}

}//namespace

EventLoop::EventLoop(bool /*single_thread_mode*/)
    :wakeup_fd_(::eventfd(0,EFD_NONBLOCK | EFD_CLOEXEC)),
    wakeup_channel_(nullptr),
    tid_(Tid()){
    wakeup_channel_=new Channel(this,wakeup_fd_);
    wakeup_channel_->set_read_callback([this](){HandleWakeup();});
    wakeup_channel_->enable_reading();

    pending_tasks_.reserve(16);
    pending_buffer_.reserve(16);
}

EventLoop::~EventLoop(){
    if(wakeup_channel_){
        wakeup_channel_->disable_all();
        wakeup_channel_->remove();
        delete wakeup_channel_;
    }
    if(wakeup_fd_>=0)
    {
        ::close(wakeup_fd_);
    }
}

bool EventLoop::IsInLoopThread() const {return Tid()==tid_;}

void EventLoop::Loop(){
    while(!quit_){
        bool empty;
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            empty=pending_tasks_.empty();
        }
        int timeout_ms =empty?-1:0;
        const epoll_event* evs =nullptr;
        int n =poller_.Poll(timeout_ms,&evs);
        for(int i=0;i<n;++i)
        {
            const auto &ev=evs[i];
            auto *ch=static_cast<Channel*>(ev.data.ptr);
            ch->set_revents(ev.events);
            ch->handle_event();
        }
        DoPendingTasks();
    }
}
void EventLoop::Quit(){
    quit_ = true;
    Wakeup();//退出时也要唤醒方便完成剩余的下班
}

void EventLoop::RunInLoop(Task cb)
{
    if(IsInLoopThread()){
        cb();
    }else{
        QueueInLoop(std::move(cb));
    }
}

void EventLoop::QueueInLoop(Task cb){
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_tasks_.emplace_back(std::move(cb));
    }
    if(!IsInLoopThread() || calling_pending_){
        bool expected = false;
        if(wakeup_pending_.compare_exchange_strong(expected,true,
        std::memory_order_relaxed))
        {
            Wakeup();
        }
    }
}

void  EventLoop::UpdateChannel(Channel *ch){
    if(!ch->registered()){
        if(ch->events()!=0)
        {
            poller_.Add(ch,ch->events());
            ch->set_registered(true);
            ch->set_registered_events(ch->events());
        }
    }else{
        if(ch->events()==0){
            poller_.Del(ch);
            ch->set_registered(false);
            ch->set_registered_events(0);
        }else{
            poller_.Mod(ch,ch->events());
            ch->set_registered_events(ch->events());
        }
    }
}

void EventLoop::RemoveChannel(Channel *ch){
    if(ch->registered()){
        poller_.Del(ch);
        ch->set_registered(false);
        ch->set_registered_events(0);
    }
}


void EventLoop::Wakeup(){
    uint64_t one =1;
    ::write(wakeup_fd_,&one,sizeof(one));
}

void EventLoop::HandleWakeup(){
    uint64_t x;
    while(::read(wakeup_fd_,&x,sizeof(x))>0){
    }
    wakeup_pending_.store(false,std::memory_order_relaxed);
}

void EventLoop::DoPendingTasks(){
  {
    std::lock_guard<std::mutex> lk(pending_mu_);
    if(pending_tasks_.empty()) return;
    pending_tasks_.swap(pending_buffer_);
  }
  calling_pending_ = true;
  for(auto &task:pending_buffer_){
    task();
  }
  pending_buffer_.clear();
  calling_pending_ = false;

}



}