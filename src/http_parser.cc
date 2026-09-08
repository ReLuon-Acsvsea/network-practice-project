#include "netx/http_parser.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <cstring>
#include <strings.h>  // for strncasecmp（大小写不敏感比较）

namespace netx {

// 解析 HTTP 方法字符串为枚举
// GET    → 获取资源（浏览器打开网页、获取数据）
// POST   → 提交数据（提交表单、上传文件）
// PUT    → 替换/更新资源（更新用户信息）
// DELETE → 删除资源（删除一篇文章）
// HEAD   → 只获取响应头（检查资源是否存在，不下载 Body）
static HttpRequest::Method ParseMethod(const std::string& m) {
  if (m == "GET") return HttpRequest::Method::kGet;
  if (m == "POST") return HttpRequest::Method::kPost;
  if (m == "PUT") return HttpRequest::Method::kPut;
  if (m == "DELETE") return HttpRequest::Method::kDelete;
  if (m == "HEAD") return HttpRequest::Method::kHead;
  return HttpRequest::Method::kUnsupported;
}

// 解析 HTTP 请求的总入口
// 返回值：true = 解析出一个完整请求，false = 数据不够等下次，throw = 格式错误
// 增量解析：Buffer 里可能只有半个请求，返回 false 等 read_fd 补齐后再 Parse
bool HttpParser::Parse(Buffer* buf, HttpRequest* req) {
  const char* begin = buf->peek();
  const char* end = begin + buf->readable_bytes();

  // ── 第一步：解析请求行 ──
  // HTTP 请求格式：GET /index.html HTTP/1.1\r\n
  // 用 memchr 找第一个 \r 定位请求行结尾
  const char* crlf = static_cast<const char*>(memchr(begin, '\r', end - begin));
  // \r 后面没有 \n，或者 \r 后面没有更多数据 → 数据不够
  if (!crlf || crlf + 1 >= end || *(crlf + 1) != '\n') return false;
  size_t consumed = 0;
  // 解析请求行：方法、路径、版本
  if (!ParseRequestLine(begin, crlf, req, &consumed)) throw std::runtime_error("bad request line");
  // retrieve 消费掉请求行 + \r\n（2字节）
  buf->retrieve(consumed + 2);

  // ── 第二步：解析头部 ──
  // 头部格式：Key: Value\r\n，多个头部逐行排列，空行（\r\n）表示头部结束
  // 只提取关键头部：Content-Length（Body 长度）、Connection（keep-alive）、WebSocket 相关
  // 其他头部忽略，避免不必要的 string 构造和 map 插入
  size_t content_len = 0;// 记录 Body 有多长
  bool has_content_len = false;// 有没有 Content-Length 头
  bool has_connection = false; // 有没有 Connection 头
  bool keep_alive_value = true;  // Connection 的值是 keep-alive 还是 close，默认 keep-alive

  // lambda：检查头部 key 是否在需要存储的列表里，是则存入 req
  // 只存 WebSocket 握手和静态文件需要的几个头部，其他忽略
  auto maybe_store_header = [&](const char* key_begin, size_t key_len,
                                const char* val_begin,
                                const char* val_end) {
    static constexpr const char* kTracked[] = {
        "Connection",        "Upgrade",      "Host",
        "Origin",            "Sec-WebSocket-Key",
        "Sec-WebSocket-Version", "Sec-WebSocket-Protocol",
        "Sec-WebSocket-Extensions"};
    for (auto literal : kTracked) {
      size_t lit_len = std::strlen(literal);
      if (key_len == lit_len &&
          ::strncasecmp(key_begin, literal, lit_len) == 0) {
        req->set_header(std::string(key_begin, key_len),
                        std::string(val_begin, val_end));
        return;
      }
    }
  };

  while (true) {
    const char* b = buf->peek();
    const char* e = b + buf->readable_bytes();
    // 找 \r 定位当前行结尾
    const char* cr = static_cast<const char*>(memchr(b, '\r', e - b));
    if (!cr || cr + 1 >= e) return false;  // 数据不够，等下次 read_fd
    // 行首就是 \r → 空行 → 头部结束
    if (cr == b) {
      buf->retrieve(2);  // 消费掉 \r\n
      break;
    }

    // 解析一行头部：key: value\r\n
    const char* line_end = cr;
    // 找 : 分割 key 和 value
    const char* colon = static_cast<const char*>(memchr(b, ':', line_end - b));
    if (colon) {
      size_t key_len = static_cast<size_t>(colon - b);
      // value 从冒号后面开始，跳过前导空格
      const char* v = colon + 1;
      while (v < line_end && std::isspace(static_cast<unsigned char>(*v))) ++v;
      // value 结尾，去掉尾部空格
      const char* v_end = line_end;
      while (v_end > v && std::isspace(static_cast<unsigned char>(*(v_end - 1)))) --v_end;

      // Content-Length 头：解析 Body 长度（十进制数字）
      if (key_len == 14 && ::strncasecmp(b, "Content-Length", 14) == 0) {
        size_t len = 0;
        for (const char* p = v; p < v_end; ++p) {
          unsigned char ch = static_cast<unsigned char>(*p);
          if (ch < '0' || ch > '9') { len = 0; break; }
          len = len * 10 + (ch - '0');  // 逐字符转数字
        }
        content_len = len;
        has_content_len = true;
        maybe_store_header(b, key_len, v, v_end);
      }
      // Connection 夥：判断 keep-alive 还是 close
      else if (key_len == 10 && ::strncasecmp(b, "Connection", 10) == 0) {
        has_connection = true;
        if ((v_end - v) == 5 && ::strncasecmp(v, "close", 5) == 0) keep_alive_value = false;
        else if ((v_end - v) == 10 && ::strncasecmp(v, "keep-alive", 10) == 0) keep_alive_value = true;
        maybe_store_header(b, key_len, v, v_end);
      }
      // 其他头部：只存 WebSocket/静态文件需要的
      else {
        maybe_store_header(b, key_len, v, v_end);
      }
    }
    // 消费掉这一行（不含 \r\n 的长度 + 2 字节 \r\n）
    buf->retrieve((line_end - b) + 2);
  }

  // ── 第三步：解析 Body ──
  // 只支持 Content-Length 方式（固定长度），不支持 chunked
  // 零拷贝：用 string_view 指向 Buffer，不拷贝数据
  if (has_content_len) {
    size_t need = content_len;
    if (buf->readable_bytes() < need) return false;  // Body 数据不够
    req->set_body_view(buf->peek(), need);            // 零拷贝视图
  } else {
    req->set_body_view(nullptr, 0);  // 没有 Body
  }
  // 缓存 keep-alive 判断，避免后续查 headers map
  if (has_connection) req->set_keep_alive_hint(true, keep_alive_value);
  return true;
}

// 解析请求行：GET /index.html HTTP/1.1
// 用 memchr 找两个空格分割三段：方法、路径、版本
bool HttpParser::ParseRequestLine(const char* begin, const char* end, HttpRequest* req, size_t* consumed) {
  /*      const char* begin,     // 请求行起始位置
      const char* end,       // 请求行结束位置（不含 \r）
      HttpRequest* req,      // 传出参数：解析结果存到哪
      size_t* consumed       // 传出参数：消费了多少字节*/
  // 找第一个空格 → 方法结束
  const char* sp1 = static_cast<const char*>(memchr(begin, ' ', end - begin));
  if (!sp1) return false;
  // 找第二个空格 → 路径结束
  const char* sp2 = static_cast<const char*>(memchr(sp1 + 1, ' ', end - sp1 - 1));
  if (!sp2) return false;

  // 方法：[begin, sp1) → 构造 string 转枚举
  std::string m(begin, sp1);
  req->set_method(ParseMethod(m));

  // 路径：[sp1+1, sp2) → 零拷贝 string_view 指向 Buffer
  req->set_path_view(sp1 + 1, static_cast<size_t>(sp2 - (sp1 + 1)));

  // 版本：[sp2+1, end) → 期望 "HTTP/1.x"
  const char* ver = sp2 + 1;
  const size_t vlen = static_cast<size_t>(end - ver);
  if (vlen < 8) return false;  // 最短 "HTTP/1.0" 是 8 字符
  // 检查 "HTTP/1." 前缀
  if (!(ver[0] == 'H' && ver[1] == 'T' && ver[2] == 'T' && ver[3] == 'P' && ver[4] == '/' && ver[5] == '1' && ver[6] == '.')) {
    // 非 HTTP/1.x：仍接受，但不设置版本（HTTP/2 等交给其他处理器）
  } else {
    // 取小版本号（1.0 → 0，1.1 → 1）
    int minor = (ver[7] >= '0' && ver[7] <= '9') ? (ver[7] - '0') : 1;
    req->set_version(1, minor);
  }

  // 返回请求行消费的字节数（不含 \r\n）
  *consumed = static_cast<size_t>(end - begin);
  return true;
}

}  // namespace netx


