#ifndef NETX_WEBSOCKET_SERVER_H_
#define NETX_WEBSOCKET_SERVER_H_

#include "netx/tcp_server.h"
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace netx{
class Buffer;
class EventLoop;
class InetAddress;
class TcpConnection;

class WebSocketServer{

 public:
    using ConnectionPtr = std::shared_ptr<TcpConnection>;
    WebSocketServer(EventLoop*loop, const InetAddress &addr, int io_thrads);
    ~WebSocketServer();
    void Start();

     // 将一条文本消息广播给订阅了指定 ID 的 WebSocket 客户端
    void PublishTo(uint32_t id, std::string_view payload);
 private:
    /* Session每个连接上的"用户档案"，
    通过 conn->set_context(sess) 存着，
    需要时conn->get_context<Session>() 取出来*/
    struct Session{
        enum class State {kHandshaking,kOpen,kClosing};
        State state=State::kHandshaking;
        std::string user_id;
        std::vector<uint32_t> subscribed;
        std::chrono::steady_clock::time_point last_heartbeat;//最后一次收到消息时间
    };

    void OnConnection(const ConnectionPtr &c);
    void OnMessage(const ConnectionPtr& c, Buffer *buf);
    //握手，成功后状态改成kOpen
    bool HandleHandshake(const ConnectionPtr &c, Session *sess, Buffer* buf);
    
    //握手完成后

    //循环解析 buf 中的 WebSocket 帧，按 opcode 分发到下面三个 handle
    void HandleFrames(const ConnectionPtr&c, Session *sess,Buffer* buf);
    //解析帧 三个处理帧
    bool ParseFrame(Buffer *buf, bool* fin, uint8_t* opcode, std::string*payload);
    void HandleTextFrame(const ConnectionPtr&c, Session *sess, std::string_view payload);
    void HandlePingFrame(const ConnectionPtr&c, std::string_view payload);
    void HandleCloseFrame(const ConnectionPtr&c, Session *sess, std::string_view payload);
    //订阅管理
    void UpdateSubscriptions(const ConnectionPtr &c, Session *sess,
                             const std::vector<uint32_t>& new_ids);
    void RemoveConnection(const ConnectionPtr &c);

    //Send 的目标是参数 c 指向的那个 WebSocket 客户端
    void SendText(const ConnectionPtr& c,std::string_view payload);
    void SendJsonMessage(const ConnectionPtr& c, const std::string& type,
                         const std::string & body_kv="");
    void SendClose(const ConnectionPtr & c, uint16_t code, std::string_view reason);
    void SendPong(const ConnectionPtr &c, std::string_view payload);
    static std::string BuildFrame(uint8_t opcode, std::string_view payload,
                                  bool fin =true);                    

    EventLoop * base_loop_;
    TcpServer server_;
    mutable std::mutex subs_mu_;
    std::unordered_map<uint32_t,std::vector<std::weak_ptr<TcpConnection>>> subs_;

};
}

#endif