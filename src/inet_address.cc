#include "netx/inet_address.h"

#include <arpa/inet.h>
#include <cstring>

namespace netx {

// 默认构造函数：初始化为 IPv4 地址结构
InetAddress::InetAddress() {
  std::memset(&addr_, 0, sizeof(addr_));
  addr_.sin_family = AF_INET;
}

// 构造函数：指定端口，监听所有地址
InetAddress::InetAddress(uint16_t port) : InetAddress() {
  addr_.sin_addr.s_addr = htonl(INADDR_ANY);
  addr_.sin_port = htons(port);
}

// 构造函数：指定 IP 地址和端口
InetAddress::InetAddress(std::string_view ip, uint16_t port) : InetAddress() {
  ::inet_pton(AF_INET, std::string(ip).c_str(), &addr_.sin_addr);
  addr_.sin_port = htons(port);
}

// 获取 sockaddr 指针（const 版本）
const struct sockaddr* InetAddress::sockaddr() const {
  return reinterpret_cast<const struct sockaddr*>(&addr_);
}

// 获取 sockaddr 指针（非 const 版本）
struct sockaddr* InetAddress::sockaddr() {
  return reinterpret_cast<struct sockaddr*>(&addr_);
}

// 获取地址结构大小
socklen_t InetAddress::length() const { return sizeof(addr_); }

// 转换为 IP 字符串
std::string InetAddress::to_ip() const {
  char buf[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, const_cast<in_addr*>(&addr_.sin_addr), buf, sizeof(buf));
  return std::string(buf);
}

// 转换为 IP:Port 字符串
std::string InetAddress::to_ip_port() const {
  return to_ip() + ":" + std::to_string(ntohs(addr_.sin_port));
}

}  // namespace netx


