#include "netx/http_parser.h"

#include <cstring>
#include <strings.h>
#include <stdexcept>
namespace netx{

static HttpRequest::Method ParseMethod(const std::string &m){
  if (m == "GET") return HttpRequest::Method::kGet;
  if (m == "POST") return HttpRequest::Method::kPost;
  if (m == "PUT") return HttpRequest::Method::kPut;
  if (m == "DELETE") return HttpRequest::Method::kDelete;
  if (m == "HEAD") return HttpRequest::Method::kHead;
  return HttpRequest::Method::kUnsupported;
}

bool HttpParser::Parse(Buffer *buf, HttpRequest *req){

    //第一步：调用函数解析请求行
   // HTTP 请求格式：GET /index.html HTTP/1.1\r\n
    const char *begin = buf->peek();
    const char* end = begin+buf->readable_bytes();
    //找第一个 \r
    const char *crlf = static_cast<const char*>(memchr(begin,'\r',end-begin));
    //数据不够
    if(!crlf || crlf+1>=end || *(crlf+1)!='\n') return false;
    size_t consumed =0;
    if(!ParseRequesetLine(begin,crlf,req,&consumed)) throw std::runtime_error("bad request line");
    buf->retrieve(consumed +2);
  // 第二步：解析头部
  // 头部格式：Key: Value\r\n，多个头部逐行排列，空行（\r\n）表示头部结束
  // 只提取关键头部：Content-Length（Body 长度）、Connection（keep-alive）、WebSocket 相关
    size_t content_len =0;
    bool has_content_len = false;
    bool has_connection = false;
    bool keep_alive_value = true;

    //只提取关键头部 lambda
    auto maybe_store_header = [&](const char * key_begin,size_t key_len,
                                  const char * val_begin,
                                  const char * val_end){
        static constexpr const char* kTracked[]={
            "Connection","Upgrade","Host","Origin",
            "Sec-WebSocket-Key","Sec-WebSocket-Version", 
            "Sec-WebSocket-Protocol","Sec-WebSocket-Extensions"
        };
        for (auto literal : kTracked){
            size_t lit_len = std::strlen(literal);
            if(key_len == lit_len &&::strncasecmp(key_begin, literal, lit_len) == 0){
                req->set_header(std::string(key_begin, key_len),std::string(val_begin, val_end));
                return;
            }
        }
    };
    while(true){
        const char *b =buf->peek();
        const char *e=b+buf->readable_bytes();
        const char *cr= static_cast<const char*>(memchr(b,'\r',e-b));
        if (!cr || cr + 1 >= e) return false;  // 数据不够
        if(cr==b){
            buf->retrieve(2);
            break;
        }

        const char *line_end =cr;
        const char *colon = static_cast<const char*>(memchr(b,':',line_end-b));
        if(colon){
            size_t key_len = static_cast<size_t>(colon-b);
            //去除多余空格 找到准确起始
            const char * v =colon+1;
            while(v<line_end && std::isspace(static_cast<unsigned char>(*v))) ++v;
            const char *v_end =line_end;
            while(v_end>v && std::isspace(static_cast<unsigned char>(*v_end))) --v_end;
            
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
        buf->retrieve((line_end-b)+2);
    }
    //第三步：解析 Body ──
    if(has_content_len){
        if(buf->readable_bytes() < content_len) return false;
        req->set_body_view(buf->peek(),content_len);
    }else{
        req->set_body_view(nullptr,0);
    }
    if (has_connection) req->set_keep_alive_hint(true, keep_alive_value);
    return true;
}
bool HttpParser::ParseRequesetLine(const char* begin ,const char* end, HttpRequest* req,size_t * consumed){
    // 例子 HTTP 请求格式：GET /index.html HTTP/1.1\r
    //第一个空格 方法GET后
    const char* sp1 = static_cast<const char*>(memchr(begin, ' ', end - begin));
    if (!sp1) return false;
    // 找第二个空格  路径结束
    const char* sp2 = static_cast<const char*>(memchr(sp1 + 1, ' ', end - sp1 - 1));
    if (!sp2) return false;
    std::string m(begin, sp1);
    req->set_method(ParseMethod(m));
    req->set_path_view(sp1+1,static_cast<size_t>(sp2-(sp1+1)));
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
    *consumed = static_cast<size_t>(end-begin);
    return true;
}
}

