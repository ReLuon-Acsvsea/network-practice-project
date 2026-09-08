#include "netx/channel.h"

#include "netx/event_loop.h"

#include <sys/epoll.h>

namespace netx
{

    // 构造函数：绑定事件循环和文件描述符
    Channel::Channel(EventLoop *loop, int fd) : loop_(loop), fd_(fd) {}

    // void Channel::enable_reading() { events_ |= (EPOLLIN | EPOLLPRI | EPOLLET | EPOLLONESHOT); update(); }
    // void Channel::enable_writing() { events_ |= (EPOLLOUT | EPOLLET | EPOLLONESHOT); update(); }

    // 启用可读事件：使用水平触发（LT）模式
    void Channel::enable_reading()
    {
        // Remove EPOLLET to switch to Level Triggered (LT) mode, matching Muduo
        events_ |= (EPOLLIN | EPOLLPRI | EPOLLRDHUP);
        update();
    }

    // 启用可写事件
    void Channel::enable_writing()
    {
        // Remove EPOLLET
        events_ |= (EPOLLOUT);
        update();
    }

    // 禁用可写事件
    void Channel::disable_writing()
    {
        events_ &= ~EPOLLOUT;
        update();
    }

    // 禁用所有事件
    void Channel::disable_all()
    {
        events_ = 0;
        update();
    }

    // 绑定对象生命周期：防止在事件处理时对象被销毁
    void Channel::tie(const std::shared_ptr<void> &obj)
    {
        tie_ = obj;
        tied_ = true;
    }

    // 更新事件注册：通知事件循环
    void Channel::update()
    {
        if (!registered_ || events_ != registered_events_)
        {
            loop_->UpdateChannel(this);
        }
    }

    // 从事件循环中移除
    void Channel::remove()
    {
        registered_events_ = 0;
        loop_->RemoveChannel(this);
    }

    // 处理事件：检查生命周期绑定后分发
    void Channel::handle_event()
    {
        if (tied_)
        {
            auto guard = tie_.lock();
            if (guard)
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
        // Prefer read/write before close to avoid use-after-free if close destroys owner
        if ((revents_ & (EPOLLERR)) && error_cb_)
            error_cb_();
        if ((revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) && read_cb_)
            read_cb_();
        if ((revents_ & (EPOLLOUT)) && write_cb_)
            write_cb_();
        if ((revents_ & (EPOLLHUP)) && close_cb_)
            close_cb_();
    }

} // namespace netx
