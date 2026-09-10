// 简单红绿灯推送客户端 Demo
//
// 功能：
// - 连接服务器
// - 发送 LOGIN 与 SUBSCRIBE 请求
// - 周期性发送 PING
// - 接收并打印 LIGHT_UPDATE 推送条数（可选择打印详细信息）
//
// 注意：本文件是纯 C 风格的 socket 编程，不依赖 NetX 库
// 对比服务器端用 C++ 封装（EventLoop/TcpConnection/Buffer），
// 这里用原始的 socket/connect/send/recv，展示客户端视角

// ==================== 头文件 ====================
// 以下都是 C 的网络头文件，和你以前 Mydir 里写的一样
#include <arpa/inet.h>    // htonl/htons/ntohl/ntohs 字节序转换
#include <fcntl.h>        // fcntl O_NONBLOCK
#include <netdb.h>        // getaddrinfo/addrinfo DNS 解析
#include <sys/select.h>   // select 超时
#include <sys/socket.h>   // socket/connect/send/recv
#include <unistd.h>       // close

// C++ 头文件（用 C++ 的 string/thread 简化代码，但网络部分是 C 风格）
#include <chrono>         // 时间相关（sleep_for）
#include <csignal>        // signal/sig_atomic_t（Ctrl+C 信号处理）
#include <cstdint>        // uint32_t 等定长整数类型
#include <cstring>        // memcpy
#include <iostream>       // cout/cerr（C++ 的输出，替代 printf）
#include <string>         // string（C++ 的字符串，替代 char[]）
#include <thread>         // thread（C++ 的线程，替代 pthread_create）
#include <vector>         // vector（C++ 的动态数组，替代手动 malloc）

namespace {

// ==================== 协议定义 ====================
// 和服务器端完全一样，消息类型枚举
// C++ 的 enum class 比 C 的 enum 更安全：不会隐式转 int，必须用 MsgType::kLogin
enum class MsgType : uint16_t {
  kLogin = 1,         // 登录：客户端告诉服务器"我是谁"
  kSubscribe = 2,     // 订阅：客户端告诉服务器"我要看哪些灯"
  kPing = 3,          // 心跳请求：客户端问服务器"你还活着吗"
  kPong = 4,          // 心跳回复：服务器回答"活着呢"
  kLightUpdate = 100, // 推送：服务器主动发给客户端"灯状态变了"
};

// ==================== MakePacket ====================
// 功能：把消息类型 + body 打包成协议格式的字节流
// 协议格式：[4字节总长度][2字节类型][body]
// 和服务器端的 MakePacket 代码完全一样（复制过来的，因为不共享头文件）
//
// C 写法对比：C 里会用 char buf[1024] + memcpy，返回 void* + len
// C++ 写法：用 std::string 当字节容器，返回 string 更方便（自带长度、自动释放）
std::string MakePacket(MsgType type, const void* body, size_t body_len) {
  const uint32_t total_len = static_cast<uint32_t>(sizeof(uint32_t) +
                                                   sizeof(uint16_t) + body_len);
  std::string out;                    // C++ 的 string 当字节缓冲区（不是字符串）
  out.resize(total_len);              // 分配 total_len 字节
  char* p = out.data();               // data() 返回 char* 指针，和 C 的 buf 一样

  // 写入总长度（网络字节序 = 大端）
  uint32_t len_n = htonl(total_len);
  std::memcpy(p, &len_n, sizeof(len_n));   // memcpy 和 C 一样

  // 写入消息类型（网络字节序）
  uint16_t type_n = htons(static_cast<uint16_t>(type));
  std::memcpy(p + sizeof(uint32_t), &type_n, sizeof(type_n));

  // 写入 body（如果有的话）
  if (body_len > 0 && body) {
    std::memcpy(p + sizeof(uint32_t) + sizeof(uint16_t), body, body_len);
  }
  return out;  // NRVO 优化：编译器直接在调用者的位置构造，不拷贝
}

// ==================== ConnectTo ====================
// 功能：创建 socket 并连接服务器，返回 fd（失败返回 -1）
//
// C 写法对比：
//   int fd = socket(AF_INET, SOCK_STREAM, 0);
//   struct sockaddr_in addr;
//   addr.sin_family = AF_INET;
//   addr.sin_port = htons(port);        // 手动填端口
//   addr.sin_addr.s_addr = inet_addr("127.0.0.1");  // 手动填 IP
//   connect(fd, (struct sockaddr*)&addr, sizeof(addr));
//
// C++ 写法（本文件）：
//   用 getaddrinfo 自动解析，支持 IPv4/IPv6，支持域名
//   getaddrinfo("127.0.0.1", "9000", &hints, &res) 自动填充地址结构
//   返回的 res 是链表，遍历尝试连接，哪个成功用哪个
int ConnectTo(const std::string& host, int port) {
  // hints 告诉 getaddrinfo 我们想要什么类型的地址
  addrinfo hints{};                       // {} 零初始化（C++ 写法，C 用 memset(&hints,0,...)）
  hints.ai_family = AF_UNSPEC;            // IPv4 或 IPv6 都行
  hints.ai_socktype = SOCK_STREAM;        // TCP
  hints.ai_protocol = IPPROTO_TCP;

  // getaddrinfo 要求端口是字符串，snprintf 转换
  char port_str[16];
  std::snprintf(port_str, sizeof(port_str), "%d", port);

  // DNS 解析：把 host + port 转成 socket 地址
  // res 是链表头，可能有多个结果（IPv4、IPv6 各一个）
  addrinfo* res = nullptr;
  int rc = ::getaddrinfo(host.c_str(), port_str, &hints, &res);
  // 注意 :: 前缀：调用全局命名空间的 getaddrinfo（系统函数）
  // C 里直接调 getaddrinfo()，C++ 里加 :: 避免和命名空间里的同名函数冲突
  if (rc != 0) {
    std::cerr << "getaddrinfo failed: " << gai_strerror(rc) << "\n";
    return -1;
  }

  // 遍历所有地址尝试连接（阻塞，无超时，等内核 TCP 重传）
  int fd = -1;
  for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

// ==================== ClientOptions ====================
// 命令行参数结构体，带默认值
// C 写法：全局变量或 main 里手动赋默认值
// C++ 写法：结构体带默认值，更清晰
struct ClientOptions {
  std::string host = "127.0.0.1";        // 服务器地址
  int port = 9000;                       // 服务器端口（TcpServer 的端口）
  std::string user_id = "user1";         // 用户名
  std::vector<std::string> light_ids{"LIGHT-001", "LIGHT-002", "LIGHT-003"};  // 要订阅的灯ID列表
  int ping_interval_sec = 10;            // 心跳间隔（秒）
  bool verbose = false;                  // 是否打印每条更新详情
};

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog
            << " [--host <host>] [--port <port>] [--user <id>]\n"
               "           [--lights id1,id2,...] [--ping-interval <sec>]\n"
               "           [--verbose]\n";
}

// ==================== ParseArgs ====================
// 功能：解析命令行参数，填充 opts 结构体
// 和服务器端的 ParseArgs 逻辑一样，解析 --host --port 等参数
bool ParseArgs(int argc, char* argv[], ClientOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    // lambda：取下一个参数转 int（C++11 特性，C 里要写单独的函数）
    auto next_int = [&](int& dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = std::stoi(argv[++i]);       // stoi = string to int（C++ 函数）
      return true;
    };
    // lambda：取下一个参数当字符串
    auto next_str = [&](std::string& dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    if (arg == "--host") {
      if (!next_str(opts->host)) return false;
    } else if (arg == "--port") {
      if (!next_int(opts->port)) return false;
    } else if (arg == "--user") {
      if (!next_str(opts->user_id)) return false;
    } else if (arg == "--lights") {
      // --lights LIGHT-001,LIGHT-002 → 解析成 vector<string>
      std::string v;
      if (!next_str(v)) return false;
      opts->light_ids.clear();
      std::string cur;
      for (char c : v) {
        if (c == ',') {
          if (!cur.empty()) {
            opts->light_ids.push_back(cur);
            cur.clear();
          }
        } else {
          cur.push_back(c);
        }
      }
      if (!cur.empty()) {
        opts->light_ids.push_back(cur);
      }
    } else if (arg == "--ping-interval") {
      if (!next_int(opts->ping_interval_sec)) return false;
    } else if (arg == "--verbose") {
      opts->verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return false;
    } else {
      std::cout << "Unknown arg: " << arg << "\n";
      PrintUsage(argv[0]);
      return false;
    }
  }
  if (opts->light_ids.empty()) {
    opts->light_ids = {"LIGHT-001", "LIGHT-002", "LIGHT-003"};
  }
  return true;
}

// ==================== BuildLogin ====================
// 功能：构造 LOGIN 消息的完整协议包
// LOGIN body 格式：[2字节 user_id 长度][user_id 字符串]
// 比如 user_id = "user1"，body = [0x00,0x05, 'u','s','e','r','1']
//
// C 写法：char buf[256]; memcpy(buf, &len, 2); memcpy(buf+2, user, len);
// C++ 写法：用 string.resize() 分配空间，data() 取指针操作
std::string BuildLogin(const std::string& user) {
  uint16_t len = static_cast<uint16_t>(user.size());
  uint16_t len_n = htons(len);         // 转网络字节序
  std::string body;
  body.resize(sizeof(len_n) + user.size());  // 分配 2 + 用户名长度 字节
  std::memcpy(body.data(), &len_n, sizeof(len_n));                        // 写长度
  std::memcpy(body.data() + sizeof(len_n), user.data(), user.size());     // 写用户名
  return MakePacket(MsgType::kLogin, body.data(), body.size());
  // 返回完整包：[4字节总长度][2字节type=1][body]
}

// ==================== BuildSubscribe ====================
// 功能：构造 SUBSCRIBE 消息的完整协议包
// SUBSCRIBE body 格式：[2字节 灯数量][2字节ID长度][ID字符串]...
// 比如订阅灯 LIGHT-001：body = [0x00,0x01, 0x00,0x09, 'L','I','G','H','T','-','0','0','1']
std::string BuildSubscribe(const std::vector<std::string>& ids) {
  uint16_t cnt = static_cast<uint16_t>(ids.size());
  uint16_t cnt_n = htons(cnt);         // 灯数量，网络字节序

  // 计算总长度
  size_t body_len = sizeof(cnt_n);
  for (const auto& id : ids) {
    body_len += sizeof(uint16_t) + id.size();  // 2字节长度 + 字符串
  }

  std::string body;
  body.resize(body_len);
  char* p = body.data();
  std::memcpy(p, &cnt_n, sizeof(cnt_n));   // 写数量
  p += sizeof(cnt_n);

  for (const auto& id : ids) {
    uint16_t id_len = static_cast<uint16_t>(id.size());
    uint16_t id_len_n = htons(id_len);
    std::memcpy(p, &id_len_n, sizeof(id_len_n));  // 写ID长度
    p += sizeof(id_len_n);
    std::memcpy(p, id.data(), id.size());          // 写ID字符串
    p += id.size();
  }
  return MakePacket(MsgType::kSubscribe, body.data(), body.size());
}

// ==================== BuildPing ====================
// 功能：构造 PING 消息（没有 body，只有头部）
std::string BuildPing() {
  return MakePacket(MsgType::kPing, nullptr, 0);
}

// ==================== 全局停止标志 ====================
// volatile：告诉编译器不要优化这个变量（可能被信号处理函数修改）
// sig_atomic_t：保证信号安全的整数类型（Ctrl+C 触发 SIGINT 信号时能正确读写）
//
// C 写法一样：volatile sig_atomic_t g_stop = 0;
// 两个线程共享这个变量：主线程检查 g_stop 控制接收循环，心跳线程检查 g_stop 控制发送循环
volatile std::sig_atomic_t g_stop = 0;

// Ctrl+C 信号处理函数：收到 SIGINT 时把 g_stop 设为 1
// C 写法：signal(SIGINT, handler);  一样
void SigIntHandler(int) { g_stop = 1; }

}  // namespace

// ==================== main ====================
int main(int argc, char* argv[]) {
  // 第一步：解析命令行参数
  ClientOptions opts;
  if (!ParseArgs(argc, argv, &opts)) {
    return 1;
  }

  // 注册 Ctrl+C 信号处理
  std::signal(SIGINT, SigIntHandler);

  // 第二步：连接服务器
  std::cout << "Connecting to " << opts.host << ":" << opts.port << " ...\n";
  int fd = ConnectTo(opts.host, opts.port);
  if (fd < 0) {
    std::cerr << "connect failed\n";
    return 1;
  }
  std::cout << "Connected.\n";

  // 第三步：预先构造三个包（复用，不每次重建）
  std::string login = BuildLogin(opts.user_id);       // 登录包
  std::string sub = BuildSubscribe(opts.light_ids);   // 订阅包
  std::string ping = BuildPing();                     // 心跳包

  // ==================== send_all ====================
  // 功能：确保一整包数据全部发完
  // 为什么需要：单次 send() 可能只发了一部分（TCP 发送缓冲区满了）
  // 所以用 while 循环：发了多少就前移指针，剩余的继续发
  //
  // C 写法：完全一样，while 循环 + send
  // 和 2.5 TcpConnection 的 SendInLoop 快路径逻辑相同
  auto send_all = [&](const std::string& s) -> bool {
    const char* p = s.data();               // 取数据指针
    size_t left = s.size();                 // 剩余要发的字节数
    while (left > 0) {
      ssize_t n = ::send(fd, p, left, 0);   // 尝试发送，返回实际发了多少
      if (n > 0) {
        p += n;                              // 指针前移（跳过已发的）
        left -= static_cast<size_t>(n);      // 剩余量减少
      } else if (n == -1 && (errno == EINTR)) {
        continue;                            // 被信号中断不算错误，重试
      } else {
        return false;                        // 真正的错误，发送失败
      }
    }
    return true;                             // 全部发完
  };

  // 第四步：发送登录和订阅
  if (!send_all(login) || !send_all(sub)) {
    std::cerr << "send login/subscribe failed\n";
    ::close(fd);
    return 1;
  }

  std::cout << "Sent LOGIN user=" << opts.user_id << " and SUBSCRIBE for "
            << opts.light_ids.size() << " lights.\n";

  // ==================== 心跳线程 ====================
  // 功能：每 10 秒发一次 PING，保持连接活跃
  // 为什么单独线程：主线程在 recv 阻塞等数据，不能同时 send
  // 所以开一个线程专门发心跳
  //
  // C 写法对比：
  //   pthread_t tid;
  //   pthread_create(&tid, NULL, ping_func, &arg);
  //   // 需要写一个单独的函数 ping_func
  //
  // C++ 写法（本文件）：
  //   std::thread ping_thread([&]() { ... });
  //   // lambda 直接写在线程构造里，[&] 捕获外部变量（fd, ping, g_stop 等）
  //   // 不用写单独函数，不用传 void* arg
  std::thread ping_thread([&]() {
    while (!g_stop) {
      std::this_thread::sleep_for(
          std::chrono::seconds(opts.ping_interval_sec));  // 睡 10 秒
      if (g_stop) break;              // 睡醒后检查是否要退出
      if (!send_all(ping)) {          // 发 PING
        std::cerr << "send PING failed, stop.\n";
        g_stop = 1;                   // 发送失败，通知主线程退出
        break;
      }
    }
  });

  // ==================== 主线程接收循环 ====================
  // 功能：接收服务器推送的数据，解析协议，统计更新条数
  //
  // C 写法：char buf[4096]; recv(fd, buf, ...);
  // C++ 写法：用 std::string 当缓冲区（自动扩容，append 追加方便）
  std::string buf;
  buf.reserve(4096);                  // 预分配 4096 字节，减少扩容次数
  uint64_t update_count = 0;          // 统计收到多少条更新

  while (!g_stop) {
    char tmp[4096];                   // 临时缓冲区（栈上，每轮循环复用）
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);   // 阻塞等数据
    if (n > 0) {
      buf.append(tmp, static_cast<size_t>(n));      // 追加到 string 缓冲区

      // ==================== 粘包解析 ====================
      // 和服务器 OnMessage 一模一样的逻辑：
      // while 循环 → peek 读长度 → 够不够 → 解析 → erase 消费
      while (buf.size() >= sizeof(uint32_t) + sizeof(uint16_t)) {
        const char* data = buf.data();              // 取数据指针
        uint32_t len_n = 0;
        std::memcpy(&len_n, data, sizeof(len_n));   // 读 4 字节长度
        uint32_t total_len = ntohl(len_n);           // 网络字节序转主机字节序
        if (total_len < sizeof(uint32_t) + sizeof(uint16_t)) {
          std::cerr << "invalid packet len=" << total_len << "\n";
          g_stop = 1;                               // 非法包，退出
          break;
        }
        if (buf.size() < total_len) break;           // 数据不够一整包，等下次 recv

        // 读消息类型
        uint16_t type_n = 0;
        std::memcpy(&type_n, data + sizeof(uint32_t), sizeof(type_n));
        uint16_t type_v = ntohs(type_n);
        MsgType type = static_cast<MsgType>(type_v);

        // body 指针和长度
        const char* body = data + sizeof(uint32_t) + sizeof(uint16_t);
        size_t body_len = total_len - sizeof(uint32_t) - sizeof(uint16_t);

        // 根据消息类型处理
        // 解析 LIGHT_UPDATE body：[2字节ID长度][ID字符串][1字节状态][4字节倒计时]
        if (type == MsgType::kLightUpdate) {
          size_t pos = 0;
          if (body_len >= sizeof(uint16_t)) {
            // 读ID长度
            uint16_t id_len_n = 0;
            std::memcpy(&id_len_n, body + pos, sizeof(id_len_n));
            uint16_t id_len = ntohs(id_len_n);
            pos += sizeof(uint16_t);

            // 读ID字符串
            if (pos + id_len + 1 + sizeof(uint32_t) <= body_len) {
              std::string light_id(body + pos, id_len);
              pos += id_len;

              // 读状态（1字节）
              uint8_t state = static_cast<uint8_t>(body[pos]);
              pos += 1;

              // 读倒计时（4字节）
              uint32_t countdown_n = 0;
              std::memcpy(&countdown_n, body + pos, sizeof(countdown_n));
              uint32_t countdown = ntohl(countdown_n);

              ++update_count;
              if (opts.verbose) {
                std::cout << "[LIGHT_UPDATE] id=" << light_id
                          << " state=" << static_cast<int>(state)
                          << " countdown=" << countdown << "s\n";
              }
            }
          }
        } else if (type == MsgType::kPong) {
          // 收到 PONG，说明服务器还活着，不需要处理
        } else {
          // 其他消息类型暂不处理
        }

        buf.erase(0, total_len);       // 消费掉这一整包数据
        // erase 把 buf 前面 total_len 字节删掉，剩余数据前移
        // 和服务器端 buf->retrieve(total_len) 效果一样
      }
    } else if (n == 0) {
      // recv 返回 0：对端关闭连接（收到 FIN 包）
      std::cout << "server closed connection\n";
      break;
    } else if (errno == EINTR) {
      // recv 返回 -1 且 errno == EINTR：被信号中断（比如 Ctrl+C）
      // 不是真正的错误，重试
      continue;
    } else {
      // recv 返回 -1 且不是 EINTR：真正的错误
      std::perror("recv");
      break;
    }
  }

  // ==================== 退出清理 ====================
  // C 写法对比：pthread_cancel(tid); pthread_join(tid, NULL); close(fd);
  // C++ 写法：设标志位让线程自己退出，join 等线程结束，close 关 socket
  // C++ 更优雅——不强制杀线程，让线程自然退出
  g_stop = 1;                                      // 通知心跳线程停止
  if (ping_thread.joinable()) ping_thread.join();   // 等心跳线程结束（C++ 的 join = C 的 pthread_join）
  ::close(fd);                                      // 关闭 socket（和 C 的 close 一样）
  std::cout << "Total LIGHT_UPDATE received: " << update_count << "\n";
  return 0;
}
