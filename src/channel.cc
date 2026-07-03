#include "netx/channel.h"
#include "netx/event_loop.h"
#include <sys/epoll.h>

namespace netx
{
    //绑定事件循环和文件描述符
    Channel::Channel(EventLoop *loop, int fd):loop_(loop),fd_(fd){}
    
    // void Channel::enable_reading() { events_ |= (EPOLLIN | EPOLLPRI | EPOLLET | EPOLLONESHOT); update(); }
    // void Channel::enable_writing() { events_ |= (EPOLLOUT | EPOLLET | EPOLLONESHOT); update(); }
    void Channel::enable_reading()
    {
        events_ |=(EPOLLIN |EPOLLPRI |EPOLLRDHUP);
        update();
    }

    void Channel::enable_writing()
    {
        events_ |=(EPOLLOUT);
        update();
    }

    void Channel::disable_writing()
    {
        events_ &=~EPOLLOUT;
        update();
    }

    void Channel::disable_all()
    {
        events_ =0;
        update();
    }
    //用weak_ptr观察对象，防止在处理事件时对象已销毁
    void Channel::tie(const std::shared_ptr<void> & obj)
    {
        tie_=obj;
        tied_ =true;
    }

    void Channel::update()
    {
        if(!registered_ ||events_!=registered_events_)
        {
            loop_->UpdateChannel(this);
        }
    }

    void Channel::remove()
    {
        registered_events_=0;
        loop_->RemoveChannel(this);
    }

    void Channel::handle_event()
    {
        if(tied_)
        {
            auto guard=tie_.lock();
            if(guard)
            {
                HandleEventGuarded();
            }
        }
        else
        {
            HandleEventGuarded();
        }
    }
        // 实际的事件处理逻辑：按优先级调用回调函数
    void Channel::HandleEventGuarded()
    {
        handling_event_ = true;
        // Prefer read/write before close to avoid use-after-free if close destroys owner
        if ((revents_ & (EPOLLERR)) && error_cb_)
            error_cb_();
        if ((revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) && read_cb_)
            read_cb_();
        if ((revents_ & (EPOLLOUT)) && write_cb_)
            write_cb_();
        if ((revents_ & (EPOLLHUP)) && close_cb_)
            close_cb_();
        handling_event_ = false;
    }



}