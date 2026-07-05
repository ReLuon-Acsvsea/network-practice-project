#ifndef NETX_HTTP_RESPONSE_H_
#define NETX_HTTP_RESPONSE_H_

#include <string>
#include <unordered_map>

namespace netx {
//HttpResponse 表示一次 HTTP 响应，
//负责生成状态行、头部以及可选的响应体
class HttpResponse{
public:
    void set_status(int code, std::string reason){
        status_code_=code;
        reason_=reason;
    }
    void set_header(std::string k,std::string v){
        headers_.emplace(std::move(k),std::move(v));
    }
    void set_body(std::string b) { body_ = std::move(b); }
    void set_keep_alive(bool ka) { keep_alive_ = ka; }

    std::string to_header_string() const; // 仅序列化响应行与头（不含 body）
    
    /*将响应行与头写入外部缓冲，不分配内存；返回是否成功（缓冲不足则返回 false）
    cap = 缓冲区容量 out_len = 返回写了多少字节*/
    bool format_header_into(char *buf, size_t cap, size_t *out_len) const;
    
    const std::string &body() const { return body_; }

 
private:
    int status_code_= 200;
    std::string reason_="OK";//状态描述
    std::unordered_map<std::string,std::string>headers_;// 自定义头部
    std::string body_;//相应体
    bool keep_alive_ =true;
};
}
#endif