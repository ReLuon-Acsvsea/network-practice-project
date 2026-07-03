#ifndef NETX_HTTP_PARSER_H_
#define NETX_HTTP_PARSER_H_

#include <optional>
#include <string>

#include "netx/buffer.h"
#include "netx/http_request.h"

namespace netx
{
//HttpParser 从buffer中按HTTP协议解析出请求
//不完整时返回false
class HttpParser{
    public:
        bool Parse(Buffer *buf, HttpRequest*req);
    private:
        bool ParseRequesetLine(const char* begin ,const char* end, HttpRequest* req,size_t * consumed);
};
    
} // namespace netx


#endif