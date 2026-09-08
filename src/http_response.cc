#include "netx/http_response.h"

#include <cstring>
#include <string>

namespace netx {

// 生成 HTTP 响应头部字符串
std::string HttpResponse::to_header_string() const {
  std::string res;
  res.reserve(128);
  res.append("HTTP/1.1 ")
      .append(std::to_string(status_code_))
      .append(" ")
      .append(reason_)
      .append("\r\n");
  for (const auto &kv : headers_) {
    res.append(kv.first).append(": ").append(kv.second).append("\r\n");
  }
  if (keep_alive_) {
    res.append("Connection: keep-alive\r\n");
  } else {
    res.append("Connection: close\r\n");
  }
  res.append("Content-Length: ")
      .append(std::to_string(body_.size()))
      .append("\r\n\r\n");
  return res;
}

// 将响应头部格式化到指定缓冲区：零拷贝优化，避免堆分配
bool HttpResponse::format_header_into(char *buf, size_t cap,
                                      size_t *out_len) const {
  // 简单的边界检查 + 逐段写入，避免分配
  size_t used = 0;

  // put lambda：往 buf 里写 n 个字节，超容量就返回 false
  auto put = [&](const char *s, size_t n) -> bool {
    if (used + n > cap)
      return false;
    std::memcpy(buf + used, s, n);
    used += n;
    return true;
  };
  // Status line
  {
    const char *pre = "HTTP/1.1 ";
    if (!put(pre, 9))
      return false;
    std::string code = std::to_string(status_code_);
    if (!put(code.data(), code.size()))
      return false;
    if (!put(" ", 1))
      return false;
    if (!put(reason_.data(), reason_.size()))
      return false;
    if (!put("\r\n", 2))
      return false;
  }
  // Headers
  for (const auto &kv : headers_) {
    if (!put(kv.first.data(), kv.first.size()))
      return false;
    if (!put(": ", 2))
      return false;
    if (!put(kv.second.data(), kv.second.size()))
      return false;
    if (!put("\r\n", 2))
      return false;
  }
  // Connection
  {
    const char *line =
        keep_alive_ ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    if (!put(line, std::strlen(line)))
      return false;
  }
  // Content-Length
  {
    const char *pre = "Content-Length: ";
    if (!put(pre, 16))
      return false;
    std::string len = std::to_string(body_.size());
    if (!put(len.data(), len.size()))
      return false;
    if (!put("\r\n\r\n", 4))
      return false;
  }
  if (out_len)
    *out_len = used;
  return true;
}

} // namespace netx
