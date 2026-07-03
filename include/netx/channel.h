#ifndef NETX_CHANNEL_H_
#define NETX_CHANNEL_H_

#include <cstdint>
#include <functional>
#include <memory>

namespace netx
{
    class EventLoop;

    /*Channel 封装某个fd的事件注册与回调
    是EventLoop 和具体的IO对象之间的桥梁*/
    class Channel
    {
    public:
        //事件回调类型(无参函数对象)
        using EventCallback=std::function<void()>;

        //loop:所属EventLoop ; fd:要监控的文件描述符
        Channel(EventLoop *loop, int fd);

        //返回底层 fd
        int fd() const{return fd_; }

        //当前关注事件掩码
        uint32_t events() const {return events_;}
        //设置本次触发的实际事件
        inline void set_revents(uint32_t ev){revents_= ev;}
        bool registered() const {return registered_;}
        void set_registered(bool r){registered_ = r;}
        //最近一次在Poller中注册的事件
        uint32_t registered_events() const {return registered_events_;}
        //更新缓存的注册事件掩码
        void set_registered_events(uint32_t ev) {registered_events_ =ev;}
        //设置四种事件回调
        void set_read_callback(EventCallback cb) { read_cb_ = std::move(cb); }
        void set_write_callback(EventCallback cb) { write_cb_ = std::move(cb); }
        void set_close_callback(EventCallback cb) { close_cb_ = std::move(cb); }
        void set_error_callback(EventCallback cb) { error_cb_ = std::move(cb); }
        //绑定生命周期对象，防止回调期间对象被销毁
        void tie(const std::shared_ptr<void> &obj);
        
        void enable_reading();
        void enable_writing();
        void disable_writing();
        void disable_all();

        void update();//向Poller提交更新
        void remove();//从Poller中移除
        void handle_event();//由EventLoop在事件就绪时调用，分配到具体回调
        void HandleEventGuarded();//带tie守护的事件处理函数

    private:
        
        EventLoop *loop_;                  //所属的EventLoop
        int fd_;                            //被监控的文件描述符
        uint32_t events_=0;                 //期望关注的事件掩码
        uint32_t revents_=0;                //实际返回就绪事件掩码
        bool registered_=false;             //是否已注册到Poller
        uint32_t registered_events_ =0;     //最近一次注册到Poller的事件掩码
        bool handling_event_=false;         //当前是否处于事件回调处理中
        bool tied_ =false;                  //是否启用了tie机制
        std::weak_ptr<void> tie_;           //绑定的上层对象(通常是TcpConnection)

        EventCallback read_cb_;             //事件回调四种
        EventCallback write_cb_;
        EventCallback close_cb_;
        EventCallback error_cb_;
    };
}//name space netx

#endif