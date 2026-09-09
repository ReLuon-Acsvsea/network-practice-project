#ifndef NETX_TCP_SERVER_H_
#define NETX_TCP_SERVER_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "netx/acceptor.h"
#include "netx/inet_address.h"
#include "netx/buffer.h"

namespace netx {

class EventLoop;
class IoThreadPool;
class TcpConnection;

// TcpServer 使用主从 Reactor 模型管理多条 TcpConnection，支持多 IO 线程
class TcpServer {
 public:
  using ConnectionPtr = std::shared_ptr<TcpConnection>;
  using ConnectionCallback = std::function<void(const ConnectionPtr&)>;
  using MessageCallback = std::function<void(const ConnectionPtr&, Buffer*)>;

  //构造函数：传入主线程的 EventLoop、监听地址、IO 线程数、是否 reuse_port。不传 IO 线程数的话默认用单线程（主线程既 accept 又处理 IO）。
  TcpServer(EventLoop* base_loop, const InetAddress& listen_addr,
            int io_threads, bool reuse_port);
  ~TcpServer();

  void SetConnectionCallback(ConnectionCallback cb) { connection_cb_ = std::move(cb); }
  void SetMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }

  // 设置连接超时：新连接创建后，如果在 timeout_ms 内没有数据，自动关闭
  // timeout_ms: 超时时间（毫秒），0 表示不超时
  void SetConnectionTimeout(uint64_t timeout_ms) { connection_timeout_ms_ = timeout_ms; }

  // 内部辅助：供轻量回调包装调用，转发到私有 RemoveConnection
  void RemoveConnectionInternal(const ConnectionPtr& conn);

  // 遍历所有连接：用于心跳检测等场景
  // callback: 回调函数，参数为 ConnectionPtr
  using ConnectionVisitor = std::function<void(const ConnectionPtr&)>;
  void ForEachConnection(ConnectionVisitor callback);

  void Start();

  ConnectionCallback connection_cb_;
  MessageCallback message_cb_;
 private:
  void NewConnection(int fd, const InetAddress& peer);//单 Acceptor 模式的入口，round-robin 选 IO 线程后调 NewConnectionInLoop
  void NewConnectionInLoop(EventLoop* io, int fd, const InetAddress& peer);//QueueInLoop 投递到目标线程，创建 TcpConnection
  void RemoveConnection(const ConnectionPtr& conn);//QueueInLoop 投递到所属线程，从 map 删除连接

  EventLoop* base_loop_;// 主线程 EventLoop：不拥有，只持有指针。Acceptor 挂在这个 loop 上（单 Acceptor模式）。生命周期由外部管理（main 函数里创建）。
  std::unique_ptr<IoThreadPool> io_pool_;// IO 线程池：独占所有权，TcpServer 销毁时自动销毁线程池
  std::vector<std::unique_ptr<Acceptor>> acceptors_;//Acceptor 列表：单 Acceptor 模式时只有一个，reuse_port 模式时每个 IO 线程一个。unique_ptr 独占所有权
  InetAddress listen_addr_;//监听地址：值拷贝存储，不引用外部
  bool reuse_port_;//是否 reuse_port：决定 Start() 时创建一个还是多个 Acceptor

  // 每个 IO 线程独立维护自己的连接表，数据面不跨线程共享
  std::unordered_map<EventLoop*, std::unordered_map<int, ConnectionPtr>> loop_conns_;

  // 连接超时时间（毫秒），0 表示不超时
  uint64_t connection_timeout_ms_ = 0;

};

}  // namespace netx

#endif  // NETX_TCP_SERVER_H_


