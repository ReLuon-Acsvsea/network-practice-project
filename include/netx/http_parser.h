#ifndef NETX_HTTP_PARSER_H_
#define NETX_HTTP_PARSER_H_

#include <optional>
#include <string>

#include "netx/buffer.h"
#include "netx/http_request.h"

namespace netx {

// HttpParser 从 Buffer 中按 HTTP 协议解析出一个 HttpRequest，请求不完整时返回 false
class HttpParser {
 public:
  // Return true if one full request parsed into req; false if need more data; throws on error
  bool Parse(Buffer* buf, HttpRequest* req);

 private:
  bool ParseRequestLine(const char* begin, const char* end, HttpRequest* req, size_t* consumed);
};

}  // namespace netx

#endif  // NETX_HTTP_PARSER_H_


