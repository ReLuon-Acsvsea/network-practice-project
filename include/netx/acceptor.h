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
        // 新连接到来时的回调：参数为新连接 fd 和对端地址
        using NewConnCallback = std::function<void(int, const InetAddress &)>;

        // loop: 所属 EventLoop；listen_addr: 监听地址；reuse_port: 是否开启 SO_REUSEPORT
        Acceptor(EventLoop *loop, const InetAddress &listen_addr, bool reuse_port);
        ~Acceptor();

        // 设置新连接回调，由上层（通常是 TcpServer）提供
        void SetNewConnCallback(NewConnCallback cb) { new_conn_cb_ = std::move(cb); }
        // 启动监听（listen + 注册读事件）
        void Listen();

    private:
        // 监听 fd 可读时被回调，执行 accept 并触发 NewConnCallback
        void HandleRead();

        // 所属 EventLoop（通常为主 Reactor）
        EventLoop *loop_;
        // 监听 socket 封装
        std::unique_ptr<Socket> listen_sock_;
        // 负责监听 fd 读写事件的 Channel
        std::unique_ptr<Channel> listen_channel_;
        // 监听地址，仅作记录
        InetAddress listen_addr_;
        // 是否已经开始 listen
        bool listening_ = false;
        // 新连接回调
        NewConnCallback new_conn_cb_;
    };

} // namespace netx

#endif // NETX_ACCEPTOR_H_
