#ifndef NETX_WEBSOCKET_SERVER_H_
#define NETX_WEBSOCKET_SERVER_H_

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "netx/tcp_server.h"

namespace netx {

class Buffer;
class EventLoop;
class InetAddress;
class TcpConnection;

// WebSocketServer 提供最小化的 WebSocket 接入能力，用于将内部的红绿灯推送
// 转发给浏览器等 Web 客户端。
class WebSocketServer {
 public:
  using ConnectionPtr = std::shared_ptr<TcpConnection>;

  WebSocketServer(EventLoop* loop, const InetAddress& addr, int io_threads);
  ~WebSocketServer();

  void Start();

  // 将一条文本消息广播给订阅了指定 ID 的 WebSocket 客户端
  void PublishTo(std::string_view id, std::string_view payload);

 private:
 /* Session每个连接上的"用户档案"，
 通过 conn->set_context(sess) 存着，
 需要时conn->get_context<Session>() 取出来*/
  struct Session {
    enum class State { kHandshaking, kOpen, kClosing };
    /*state  当前状态
    kHandshaking：还没握手，按 HTTP 处理
    kOpen：握手完成，按 WebSocket 帧处理
    kClosing：正在关闭 */
    State state = State::kHandshaking;
    std::string user_id;// 用户标识（如 "张三"）
    std::vector<std::string> subscribed; // 订阅了哪些红绿灯ID
    std::chrono::steady_clock::time_point last_heartbeat; //最后一次收到消息的时间，用于检测死连接
  };

  void OnConnection(const ConnectionPtr& c);
  void OnMessage(const ConnectionPtr& c, Buffer* buf);
  bool HandleHandshake(const ConnectionPtr& c, Session* sess, Buffer* buf);
  void HandleFrames(const ConnectionPtr& c, Session* sess, Buffer* buf);
  bool ParseFrame(Buffer* buf, bool* fin, uint8_t* opcode, std::string* payload);
  void HandleTextFrame(const ConnectionPtr& c, Session* sess, std::string_view payload);
  void HandlePingFrame(const ConnectionPtr& c, std::string_view payload);
  void HandleCloseFrame(const ConnectionPtr& c, Session* sess, std::string_view payload);
  void UpdateSubscriptions(const ConnectionPtr& c, Session* sess,
                           const std::vector<std::string>& new_ids);
  void RemoveConnection(const ConnectionPtr& c);

  void SendText(const ConnectionPtr& c, std::string_view payload);
  void SendJsonMessage(const ConnectionPtr& c, const std::string& type,
                       const std::string& body_kv = "");
  void SendClose(const ConnectionPtr& c, uint16_t code, std::string_view reason);
  void SendPong(const ConnectionPtr& c, std::string_view payload);
  static std::string BuildFrame(uint8_t opcode, std::string_view payload,
                                bool fin = true);

  EventLoop* base_loop_;
  TcpServer server_;

  mutable std::mutex subs_mu_;
  std::unordered_map<std::string, std::vector<std::weak_ptr<TcpConnection>>> subs_;
};

}  // namespace netx

#endif  // NETX_WEBSOCKET_SERVER_H_


