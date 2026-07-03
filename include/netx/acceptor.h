#ifndef NETX_ACCEPTOR_H_
#define NETX_ACCEPTOR_H_

#include <functional>
#include <memory>

#include "netx/inet_address.h"

namespace netx
{
    class EventLoop;
    class Channel;
    class Socket;

    // Acceptor 封装监听 socket，负责接受新连接并通知上层（如 TcpServer）
    class Acceptor
    {
    public:
        //新连接来时的回调，参数为新连接fd和对端地址
        using NewConnCallback =std::function<void(int, const InetAddress &)>;
    
        Acceptor(EventLoop *loop,const InetAddress& listen_addr,bool reuse_port);
        ~Acceptor();

        void SetNewConnCallback(NewConnCallback cb){new_conn_cb_=std::move(cb);}
        void Listen();

    private:
        void HandleRead();//回调函数

        EventLoop *loop_;
        std::unique_ptr<Socket> listen_sock_;
        std::unique_ptr<Channel> listen_channel_;
        InetAddress listen_addr_;
        bool listening_ =false;

        NewConnCallback new_conn_cb_;

        

    };
}
#endif