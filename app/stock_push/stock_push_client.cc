// 股票推送客户端 Demo
//
// 功能：
// - 连接服务器
// - 发送 LOGIN 与 SUBSCRIBE 请求
// - 周期性发送 PING
// - 接收并打印 STOCK_UPDATE 推送条数（可选择打印详细信息）
//
// 注意：本文件是纯 C 风格的 socket 编程，不依赖 NetX 库
// 对比服务器端用 C++ 封装（EventLoop/TcpConnection/Buffer），
// 这里用原始的 socket/connect/send/recv，展示客户端视角

// ==================== 头文件 ====================
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

// ==================== 协议定义 ====================
enum class MsgType : uint16_t {
  kLogin = 1,
  kSubscribe = 2,
  kPing = 3,
  kPong = 4,
  kStockUpdate = 100,
};

// ==================== MakePacket ====================
std::string MakePacket(MsgType type, const void* body, size_t body_len) {
  const uint32_t total_len = static_cast<uint32_t>(sizeof(uint32_t) +
                                                   sizeof(uint16_t) + body_len);
  std::string out;
  out.resize(total_len);
  char* p = out.data();

  uint32_t len_n = htonl(total_len);
  std::memcpy(p, &len_n, sizeof(len_n));

  uint16_t type_n = htons(static_cast<uint16_t>(type));
  std::memcpy(p + sizeof(uint32_t), &type_n, sizeof(type_n));

  if (body_len > 0 && body) {
    std::memcpy(p + sizeof(uint32_t) + sizeof(uint16_t), body, body_len);
  }
  return out;
}

// ==================== ConnectTo ====================
int ConnectTo(const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  char port_str[16];
  std::snprintf(port_str, sizeof(port_str), "%d", port);

  addrinfo* res = nullptr;
  int rc = ::getaddrinfo(host.c_str(), port_str, &hints, &res);
  if (rc != 0) {
    std::cerr << "getaddrinfo failed: " << gai_strerror(rc) << "\n";
    return -1;
  }

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
struct ClientOptions {
  std::string host = "127.0.0.1";
  int port = 9000;
  std::string user_id = "user1";
  std::vector<std::string> codes{"STOCK1", "STOCK2", "STOCK3"};
  int ping_interval_sec = 10;
  bool verbose = false;
};

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog
            << " [--host <host>] [--port <port>] [--user <id>]\n"
               "           [--codes code1,code2,...] [--ping-interval <sec>]\n"
               "           [--verbose]\n";
}

// ==================== ParseArgs ====================
bool ParseArgs(int argc, char* argv[], ClientOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next_int = [&](int& dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = std::stoi(argv[++i]);
      return true;
    };
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
    } else if (arg == "--codes") {
      std::string v;
      if (!next_str(v)) return false;
      opts->codes.clear();
      std::string cur;
      for (char c : v) {
        if (c == ',') {
          if (!cur.empty()) {
            opts->codes.push_back(cur);
            cur.clear();
          }
        } else {
          cur.push_back(c);
        }
      }
      if (!cur.empty()) {
        opts->codes.push_back(cur);
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
  if (opts->codes.empty()) {
    opts->codes = {"STOCK1", "STOCK2", "STOCK3"};
  }
  return true;
}

// ==================== BuildLogin ====================
std::string BuildLogin(const std::string& user) {
  uint16_t len = static_cast<uint16_t>(user.size());
  uint16_t len_n = htons(len);
  std::string body;
  body.resize(sizeof(len_n) + user.size());
  std::memcpy(body.data(), &len_n, sizeof(len_n));
  std::memcpy(body.data() + sizeof(len_n), user.data(), user.size());
  return MakePacket(MsgType::kLogin, body.data(), body.size());
}

// ==================== BuildSubscribe ====================
std::string BuildSubscribe(const std::vector<std::string>& codes) {
  uint16_t cnt = static_cast<uint16_t>(codes.size());
  uint16_t cnt_n = htons(cnt);
  
  size_t total_body_len = sizeof(cnt_n);
  for (const auto& code : codes) {
    total_body_len += sizeof(uint16_t) + code.size();
  }
  
  std::string body;
  body.resize(total_body_len);
  char* p = body.data();
  
  std::memcpy(p, &cnt_n, sizeof(cnt_n));
  p += sizeof(cnt_n);
  
  for (const auto& code : codes) {
    uint16_t code_len = static_cast<uint16_t>(code.size());
    uint16_t code_len_n = htons(code_len);
    std::memcpy(p, &code_len_n, sizeof(code_len_n));
    p += sizeof(code_len_n);
    std::memcpy(p, code.data(), code.size());
    p += code.size();
  }
  
  return MakePacket(MsgType::kSubscribe, body.data(), body.size());
}

// ==================== BuildPing ====================
std::string BuildPing() {
  return MakePacket(MsgType::kPing, nullptr, 0);
}

// ==================== 全局停止标志 ====================
volatile std::sig_atomic_t g_stop = 0;

void SigIntHandler(int) { g_stop = 1; }

}  // namespace

// ==================== main ====================
int main(int argc, char* argv[]) {
  ClientOptions opts;
  if (!ParseArgs(argc, argv, &opts)) {
    return 1;
  }

  std::signal(SIGINT, SigIntHandler);

  std::cout << "Connecting to " << opts.host << ":" << opts.port << " ...\n";
  int fd = ConnectTo(opts.host, opts.port);
  if (fd < 0) {
    std::cerr << "connect failed\n";
    return 1;
  }
  std::cout << "Connected.\n";

  std::string login = BuildLogin(opts.user_id);
  std::string sub = BuildSubscribe(opts.codes);
  std::string ping = BuildPing();

  auto send_all = [&](const std::string& s) -> bool {
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0) {
      ssize_t n = ::send(fd, p, left, 0);
      if (n > 0) {
        p += n;
        left -= static_cast<size_t>(n);
      } else if (n == -1 && (errno == EINTR)) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  };

  if (!send_all(login) || !send_all(sub)) {
    std::cerr << "send login/subscribe failed\n";
    ::close(fd);
    return 1;
  }

  std::cout << "Sent LOGIN user=" << opts.user_id << " and SUBSCRIBE for "
            << opts.codes.size() << " stocks.\n";

  std::thread ping_thread([&]() {
    while (!g_stop) {
      std::this_thread::sleep_for(
          std::chrono::seconds(opts.ping_interval_sec));
      if (g_stop) break;
      if (!send_all(ping)) {
        std::cerr << "send PING failed, stop.\n";
        g_stop = 1;
        break;
      }
    }
  });

  std::string buf;
  buf.reserve(4096);
  uint64_t update_count = 0;

  while (!g_stop) {
    char tmp[4096];
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n > 0) {
      buf.append(tmp, static_cast<size_t>(n));

      while (buf.size() >= sizeof(uint32_t) + sizeof(uint16_t)) {
        const char* data = buf.data();
        uint32_t len_n = 0;
        std::memcpy(&len_n, data, sizeof(len_n));
        uint32_t total_len = ntohl(len_n);
        if (total_len < sizeof(uint32_t) + sizeof(uint16_t)) {
          std::cerr << "invalid packet len=" << total_len << "\n";
          g_stop = 1;
          break;
        }
        if (buf.size() < total_len) break;

        uint16_t type_n = 0;
        std::memcpy(&type_n, data + sizeof(uint32_t), sizeof(type_n));
        uint16_t type_v = ntohs(type_n);
        MsgType type = static_cast<MsgType>(type_v);

        if (type == MsgType::kStockUpdate) {
          const char* body = data + sizeof(uint32_t) + sizeof(uint16_t);
          size_t body_len = total_len - sizeof(uint32_t) - sizeof(uint16_t);
          
          if (body_len >= sizeof(uint16_t)) {
            uint16_t code_len_n = 0;
            std::memcpy(&code_len_n, body, sizeof(code_len_n));
            uint16_t code_len = ntohs(code_len_n);
            
            if (body_len >= sizeof(uint16_t) + code_len + sizeof(double) * 2 + sizeof(uint64_t) * 2) {
              std::string code(body + sizeof(uint16_t), code_len);
              const char* p = body + sizeof(uint16_t) + code_len;
              
              double price, change_pct;
              uint64_t volume, timestamp;
              std::memcpy(&price, p, sizeof(price)); p += sizeof(price);
              std::memcpy(&change_pct, p, sizeof(change_pct)); p += sizeof(change_pct);
              std::memcpy(&volume, p, sizeof(volume)); p += sizeof(volume);
              std::memcpy(&timestamp, p, sizeof(timestamp));
              
              ++update_count;
              if (opts.verbose) {
                std::cout << "[STOCK_UPDATE] code=" << code
                          << " price=" << price
                          << " change=" << change_pct << "%"
                          << " volume=" << volume << "\n";
              }
            }
          }
        } else if (type == MsgType::kPong) {
          // 心跳回复
        }

        buf.erase(0, total_len);
      }
    } else if (n == 0) {
      std::cout << "server closed connection\n";
      break;
    } else if (errno == EINTR) {
      continue;
    } else {
      std::perror("recv");
      break;
    }
  }

  g_stop = 1;
  if (ping_thread.joinable()) ping_thread.join();
  ::close(fd);
  std::cout << "Total STOCK_UPDATE received: " << update_count << "\n";
  return 0;
}
