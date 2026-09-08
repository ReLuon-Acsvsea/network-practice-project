#include "netx/http_request.h"

#include <algorithm>
#include <cctype>

namespace netx {

const std::string* HttpRequest::header(std::string_view key) const {
  auto it = headers_.find(NormalizeHeaderKey(key));
  if (it == headers_.end())
    return nullptr;
  return &it->second;
}

// 判断是否保持连接：优先使用缓存值
bool HttpRequest::keep_alive() const {
  if (keep_alive_cached_)
    return keep_alive_value_;
  auto it = headers_.find("Connection");
  if (it == headers_.end())
    return http_major_ > 1 || (http_major_ == 1 && http_minor_ == 1);
  std::string v = it->second;
  std::transform(v.begin(), v.end(), v.begin(), ::tolower);
  return v == "keep-alive";
}

// 获取请求体：惰性从零拷贝视图物化为字符串
const std::string& HttpRequest::body() const {
  if (body_.empty() && !body_view_.empty()) {
    // 惰性物化：仅在调用者需要 std::string 时复制
    const_cast<HttpRequest*>(this)->body_.assign(body_view_.data(), body_view_.size());
  }
  return body_;
}

// 获取请求路径：惰性从零拷贝视图物化为字符串
const std::string& HttpRequest::path() const {
  if (path_.empty() && !path_view_.empty()) {
    const_cast<HttpRequest*>(this)->path_.assign(path_view_.data(), path_view_.size());
  }
  return path_;
}

std::string HttpRequest::NormalizeHeaderKey(std::string_view key) {
  std::string out;
  out.reserve(key.size());
  for (char ch : key) {
    unsigned char c = static_cast<unsigned char>(ch);
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

} // namespace netx
