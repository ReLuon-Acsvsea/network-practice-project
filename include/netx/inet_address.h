#ifndef NETX_INET_ADDRESS_H_
#define NETX_INET_ADDRESS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <netinet/in.h>

namespace netx {

// InetAddress 封装 IPv4 地址与端口，提供便捷的字符串与 sockaddr 转换
class InetAddress {
 public:
  InetAddress();
  explicit InetAddress(uint16_t port);              // 0.0.0.0:port
  InetAddress(std::string_view ip, uint16_t port);  // ip:port

  const struct sockaddr* sockaddr() const;
  struct sockaddr* sockaddr();
  socklen_t length() const;

  
  std::string to_ip_port() const;

 private:
  std::string to_ip() const;
  struct sockaddr_in addr_{};
};

}  // namespace netx

#endif  // NETX_INET_ADDRESS_H_