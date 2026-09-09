#ifndef NETX_HTTP_REQUEST_H_
#define NETX_HTTP_REQUEST_H_

#include <string>
#include <string_view>
#include <unordered_map>

namespace netx {

// HttpRequest 表示一次 HTTP 请求，封装方法、路径、头部以及可选的零拷贝 body 视图
class HttpRequest {
 public:
  enum class Method { kGet, kPost, kPut, kDelete, kHead, kUnsupported };

  void set_method(Method m) { method_ = m; }
  Method method() const { return method_; }
  // 兼容旧接口：如未显式设置 path_，将按需从 path_view_ 物化
  void set_path(std::string s) { path_ = std::move(s); path_view_ = std::string_view(); }
  const std::string& path() const;
  void set_path_view(const char* p, size_t n) { path_view_ = std::string_view(p, n); path_.clear(); }
  void set_path_view(std::string_view sv) { path_view_ = sv; path_.clear(); }
  std::string_view path_view() const { return path_view_; }
  void set_version(int major, int minor) { http_major_ = major; http_minor_ = minor; }
  void set_header(std::string key, std::string value) { headers_[NormalizeHeaderKey(key)] = std::move(value); }
  const std::unordered_map<std::string, std::string>& headers() const { return headers_; }// 获取所有头部
  const std::string* header(std::string_view key) const;// 获取指定头部
  // 兼容旧接口：如未显式设置 body_，将按需从 body_view_ 物化（复制）
  void set_body(std::string b) { body_ = std::move(b); body_view_ = std::string_view(); }
  const std::string& body() const;

  // 零拷贝视图：指向底层 Buffer 的数据，生命周期由上层保证在发送期间有效
  void set_body_view(const char* p, size_t n) { body_view_ = std::string_view(p, n); body_.clear(); }
  void set_body_view(std::string_view sv) { body_view_ = sv; body_.clear(); }
  std::string_view body_view() const { return body_view_; }
  bool keep_alive() const;
  void set_keep_alive_hint(bool has, bool value) { keep_alive_cached_ = has; keep_alive_value_ = value; }

 private:
  Method method_ = Method::kUnsupported;
  std::string path_;
  std::string_view path_view_{};
  int http_major_ = 1;// HTTP 主版本号（通常都是 1）
  int http_minor_ = 1;// HTTP 次版本号（0 或 1）
  std::unordered_map<std::string, std::string> headers_; // 存储的头部
  std::string body_;
  std::string_view body_view_{};
  bool keep_alive_cached_ = false; // 是否已缓存了 keep-alive 判断
  bool keep_alive_value_ = true;// 缓存的值：true=keep-alive, false=close
  static std::string NormalizeHeaderKey(std::string_view key);// 头部 key 格式化（首字母大写）
};

}  // namespace netx

#endif  // NETX_HTTP_REQUEST_H_


