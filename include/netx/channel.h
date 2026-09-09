#ifndef NETX_CHANNEL_H_
#define NETX_CHANNEL_H_

#include <cstdint>
#include <functional>
#include <memory>

namespace netx
{

    class EventLoop;

    // Channel 封装某个 fd 的事件注册与回调，是 EventLoop 和具体 IO 对象之间的桥梁
    class Channel
    {
    public:
        // 事件回调类型（无参函数对象）
        using EventCallback = std::function<void()>;

        // loop：所属 EventLoop；fd：要监控的文件描述符
        Channel(EventLoop *loop, int fd);

        // 返回底层 fd
        int fd() const { return fd_; }
        // 当前关注的事件掩码（EPOLLIN / EPOLLOUT 等）
        uint32_t events() const { return events_; }
        // 设置本次触发的实际事件（由 Poller 填充）
        inline void set_revents(uint32_t ev) { revents_ = ev; }
        // 是否已经向 Poller 注册过
        bool registered() const { return registered_; }
        // 设置注册标记（仅供 EventLoop/Poller 内部维护）
        void set_registered(bool r) { registered_ = r; }
        // 最近一次在 Poller 中注册的事件掩码
        uint32_t registered_events() const { return registered_events_; }
        // 更新缓存的注册事件掩码
        void set_registered_events(uint32_t ev) { registered_events_ = ev; }

        // 设置读事件回调（EPOLLIN 等）
        void set_read_callback(EventCallback cb) { read_cb_ = std::move(cb); }
        // 设置写事件回调（EPOLLOUT）
        void set_write_callback(EventCallback cb) { write_cb_ = std::move(cb); }
        // 设置关闭回调（对端关闭或本端关闭）
        void set_close_callback(EventCallback cb) { close_cb_ = std::move(cb); }
        // 设置错误回调（EPOLLERR）
        void set_error_callback(EventCallback cb) { error_cb_ = std::move(cb); }
        // 绑定生命周期对象，防止回调期间对象被销毁
        void tie(const std::shared_ptr<void> &obj);

        // 关注读事件
        void enable_reading();
        // 关注写事件
        void enable_writing();
        // 取消关注写事件
        void disable_writing();
        // 取消关注所有事件
        void disable_all();

        // 向 Poller 提交更新（add/mod）
        void update();
        // 从 Poller 中移除
        void remove();

        // 由 EventLoop 在事件就绪时调用，分派到具体回调
        void handle_event();
        // 带 tie 守护的事件处理函数
        void HandleEventGuarded();

    private:
        // 所属的 EventLoop
        EventLoop *loop_;
        // 被监控的文件描述符
        int fd_;
        // 期望关注的事件掩码
        uint32_t events_ = 0;
        // 实际返回的就绪事件掩码
        uint32_t revents_ = 0;
        // 是否已经注册到 Poller
        bool registered_ = false;
        // 最近一次注册到 Poller 的事件掩码
        uint32_t registered_events_ = 0;
        // 是否启用了 tie 机制
        bool tied_ = false;
        // 绑定的上层对象（通常是 TcpConnection）
        std::weak_ptr<void> tie_;

        // 读事件回调
        EventCallback read_cb_;
        // 写事件回调
        EventCallback write_cb_;
        // 关闭事件回调
        EventCallback close_cb_;
        // 错误事件回调
        EventCallback error_cb_;
    };

} // namespace netx

#endif // NETX_CHANNEL_H_
