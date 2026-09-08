// 程序员老廖：https://space.bilibili.com/3494351095204205
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

// TcpConnection 表示一条已建立的 TCP 连接，是整个库的核心枢纽：
//   - 组装了 Socket（管理 fd）、Channel（事件分发）、Buffer（收发缓冲）
//   - 通过回调通知上层（TcpServer/HttpServer）数据到达、连接关闭等事件
//   - 生命周期由 shared_ptr 管理（继承 enable_shared_from_this），
//     因为连接可能被多个地方同时使用（TcpServer 的 map、Channel 的回调、
//     上层的业务逻辑），谁最后一个用完谁负责销毁
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
  // 标准回调类型：上层（用户代码）使用，灵活但有 std::function 堆分配开销
  // 参数 shared_ptr 确保回调执行期间对象不被析构
  using MessageCallback =
      std::function<void(const std::shared_ptr<TcpConnection> &, Buffer *)>;
  using CloseCallback =
      std::function<void(const std::shared_ptr<TcpConnection> &)>;

  // 轻量回调类型：内部 hot path（TcpServer/HttpServer）使用
  // 函数指针 + void* 上下文，零开销，避免 std::function 的堆分配
  // HandleRead 里优先调 raw_msg_fn_，没有才调 message_cb_
  using RawMessageFn =
      void (*)(void *ctx, const std::shared_ptr<TcpConnection> &, Buffer *);
  using RawCloseFn =
      void (*)(void *ctx, const std::shared_ptr<TcpConnection> &);

  // 构造时注册 4 个 Channel 回调（read/write/close/error）并开始监听可读事件
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

  // SendInLoop：在所属 EventLoop 线程中执行实际发送
  //   快路径：output_ 为空 → 直接 write，省掉拷贝到 Buffer
  //   慢路径：output_ 非空 → writev 聚合写（旧数据 + 新数据）
  //   部分写：没写完 → 追加到 output_，enable_writing 等下次 EPOLLOUT
  void SendInLoop(const char *data, size_t len);
  void SendVecInLoop(const char *data1, size_t len1, const char *data2,
                     size_t len2);

  EventLoop *loop_;                       // 所属事件循环（一对一关系）
  std::unique_ptr<Socket> socket_;        // RAII 管理 fd
  std::unique_ptr<Channel> channel_;      // 事件分发器，注册到 loop_ 的 epoll
  Buffer input_;                          // 接收缓冲区（read_fd 写入，上层读取）
  Buffer output_;                         // 发送缓冲区（SendInLoop 写入，EPOLLOUT 发出）

  enum class State { kConnected, kDisconnected };
  State state_ = State::kConnected;

  // 类型安全的 void*：上层各层（HTTP/WebSocket/业务）可存自己的数据
  std::any context_;

  // 标准回调（用户代码设置，灵活）
  MessageCallback message_cb_;
  CloseCallback close_cb_;

  // 轻量回调（TcpServer/HttpServer 内部设置，零开销）
  // 函数指针 + void* 上下文，hot path 优先使用
  void *raw_msg_ctx_ = nullptr;
  RawMessageFn raw_msg_fn_ = nullptr;
  void *raw_close_ctx_ = nullptr;
  RawCloseFn raw_close_fn_ = nullptr;

  // writing_ 控制是否关注 EPOLLOUT：
  //   没数据要发 → false，不关注（否则 EPOLLOUT 空转浪费 CPU）
  //   有数据没发完 → true，关注 EPOLLOUT，等可写时 HandleWrite 继续发
  bool writing_ = false;
};

} // namespace netx

#endif // NETX_TCP_CONNECTION_H_
