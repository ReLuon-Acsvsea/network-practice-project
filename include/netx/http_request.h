#ifndef NETX_HTTP_REQUEST_H_
#define NETX_HTTP_REQUEST_H_

#include <string>
#include <string_view>
#include <unordered_map>

namespace netx {
class HttpRequest {
public:
    enum class Method {kGet, kPost, kPut, kDelete, kHead, kUnsupported};
    void set_method(Method m){method_ = m;}
    Method method() const {return method_;}
    void set_path(std::string s)
    {
        path_=std::move(s);
        path_view_=std::string_view();
    }
    const std::string & path() const;
    void set_path_view(const char *p ,size_t n){
        path_view_=std::string_view(p,n);
        path_.clear();
    }
    std::string_view path_view() const {return path_view_;}
    void set_version(int major, int minor){
        http_major_=major;
        http_minor_=minor;
    }
    void set_header(std::string key,std::string value){headers_[NormalizeHeaderKey(key)]=std::move(value);}
    const std::unordered_map<std::string,std::string>& headers() const{return headers_;}
    const std::string* header(std::string_view key) const;
    void set_body(std::string b){
        body_=std::move(b);
        body_view_=std::string_view();
    }
    const std::string & body() const;

    //零拷贝视图，指向底层Buffer的数据 生命周期由上层保证在发送期间有效
    void set_body_view(const char *p,size_t n){
        body_view_=std::string_view(p,n);
        body_.clear();
    }
    void set_body_view(std::string_view sv){
        body_view_=sv;
        body_.clear();
    }
    std::string_view body_view() const { return body_view_; }
    bool keep_alive() const;
    void set_keep_alive_hint(bool has, bool value) { keep_alive_cached_ = has; keep_alive_value_ = value; }




private:
    Method method_=Method::kUnsupported;
    mutable std::string path_;
    std::string_view path_view_{};
    int http_major_ = 1;//HTTP major version
    int http_minor_ = 1;//HTTP second version
    std::unordered_map<std::string,std::string> headers_;//存储的头部
    mutable std::string body_;
    std::string_view body_view_{};
    bool keep_alive_cached_ =false;//是否已缓存了keep-alive判断
    bool keep_alive_value_ = true;//缓存的值true keep-alive false close
    static std::string NormalizeHeaderKey(std::string_view key);//头部key格式化
};


}

#endif  // NETX_HTTP_REQUEST_H_