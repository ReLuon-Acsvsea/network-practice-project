#ifndef NETX_TCP_CONNECTION_H_
#define NETX_TCP_CONNECTION_H_

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "netx/buffer.h"

namespace netx {

class EventLoop;
class Channel;
class Socket;

/*TcpConnection 表示一条已建立的 TCP 连接，
组装了 Socket、Channel、Buffer*/
class TcpConnection : public std::enable_shared_from_this<TcpConnection>{
public:
    using MessageCallback =
        std::function<void(const std::shared_ptr<TcpConnection> &, Buffer *)>;
    using CloseCallback =
        std::function<void(const std::shared_ptr<TcpConnection> &)>;
    
        //函数指针用法，不像std::function 指向lambda 使用void * ctx
    using RawMessageFn=
        void(*)(void *ctx, const std::shared_ptr<TcpConnection>&,Buffer *);
    using RawCloseFn=
        void(*)(void *ctx, const std::shared_ptr<TcpConnection>&);
    TcpConnection(EventLoop *loop, int fd);
    ~TcpConnection();
    int fd() const;
    EventLoop *loop() const { return loop_; }
        // 设置标准回调（用户代码使用）
    void set_message_callback(MessageCallback cb) { message_cb_ = std::move(cb); }
    void set_close_callback(CloseCallback cb) { close_cb_ = std::move(cb); }

    // 设置轻量回调（TcpServer/HttpServer 内部使用，性能敏感路径）
    void set_raw_message_handler(void *ctx, RawMessageFn fn) {
        raw_msg_ctx_ = ctx;
        raw_msg_fn_ = fn;
    }
    void set_raw_close_handler(void *ctx, RawCloseFn fn) {
        raw_close_ctx_ = ctx;
        raw_close_fn_ = fn;
    }

    bool IsConnected() const;

    // context_ 是类型安全的 void*，让上层在连接上挂任意数据而不用改 TcpConnection 定义
    // 例如 HttpServer 存 HttpRequest，WebSocket 层存 WebSocketSession
    // any_cast<T> 类型不对返回 nullptr（指针版），不会崩
    void set_context(std::any ctx);
    const std::any &context() const { return context_; }
    template <typename T> T *get_context() { return std::any_cast<T>(&context_); }
    template <typename T> const T *get_context() const {
        return std::any_cast<T>(&context_);
    }

    // 发送数据：自动处理跨线程安全
    //   同线程 → 直接调 SendInLoop
    //   跨线程 → 拷贝数据 + QueueInLoop 投递到所属 EventLoop
    void Send(const std::string &s);        // 跨线程时拷贝 string
    void Send(std::string &&s);             // 跨线程时移动 string，避免拷贝
    void Send(const char *data, size_t len); // 跨线程时拷贝到 string 再移动
    void Send(std::string_view sv) { Send(sv.data(), sv.size()); }
    // 分段发送：使用 writev 一次系统调用发两段数据（如 HTTP header + body）
    void SendVec(const char *data1, size_t len1, const char *data2, size_t len2);
    void SendVec(std::string_view s1, std::string_view s2) {
        SendVec(s1.data(), s1.size(), s2.data(), s2.size());
    }

    // 半关闭：发 FIN 包，关闭写端但还能读（HTTP 响应发完后调用）
    void Shutdown();

    // 绑定自身生命周期到 Channel：Channel::handle_event() 先 lock() 检查
    // 对象是否还活着，再调回调。防止 epoll 事件触发时对象已被销毁
    // 和 shared_from_this 是两层保护：
    //   Tie 保护"进入回调前对象是否还在"
    //   shared_from_this 保护"回调执行期间对象不被销毁"
    void Tie(const std::shared_ptr<TcpConnection>& self);

private:
  // Channel 回调：epoll 事件驱动调用
  void HandleRead();   // EPOLLIN → read_fd 到 input_ → 回调上层
  void HandleWrite();  // EPOLLOUT → 写 output_ 到 fd → 写完取消关注
  void HandleClose();  // 对端关闭（read 返回 0）→ 从 epoll 移除 → 回调上层
  void HandleError();  // 读写错误 → 转 HandleClose

  void SendInLoop(const char *data, size_t len);
  void SendVecInLoop(const char *data1, size_t len1, const char * data2,size_t len2);

  EventLoop * loop_;//所属事件循环
  std::unique_ptr<Socket> socket_;        // RAII 管理 fd
  std::unique_ptr<Channel> channel_;      // 事件分发器，注册到 loop_ 的 epoll
  Buffer input_;                          // 接收缓冲区（read_fd 写入，上层读取）
  Buffer output_;                         // 发送缓冲区（SendInLoop 写入，EPOLLOUT 发出）
  enum class State {kConnected,kDisconnected};
  State state_ =State::kConnected;

  std::any context_;
  MessageCallback message_cb_;
  CloseCallback close_cb_;

  void * raw_msg_ctx_=nullptr;
  RawMessageFn raw_msg_fn_=nullptr;
  void * raw_close_ctx_=nullptr;
  RawCloseFn raw_close_fn_=nullptr;
  bool writing_ = false; //是否关注EPOLLOUT
};
}
#endif