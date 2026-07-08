#include "netx/websocket_server.h"

#include "netx/tcp_connection.h"
#include "netx/logging.h"
#include "netx/http_parser.h"
#include "netx/http_request.h"
#include <chrono>

namespace netx{
//一些内部函数
namespace{
//string_view 相等判断
bool IEquals(std::string_view lhs, std::string_view rhs){
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i])))
        {
            return false;
        }
    }
    return true;
}
//去掉首尾空白
std::string_view Trim(std::string_view sv){
    size_t begin = 0;
    while (begin < sv.size() &&
            std::isspace(static_cast<unsigned char>(sv[begin])))
    {
        ++begin;
    }
    size_t end = sv.size();
    while (end > begin &&
            std::isspace(static_cast<unsigned char>(sv[end - 1])))
    {
        --end;
    }
    return sv.substr(begin, end - begin);
}
//判断逗号分隔的 header 值是否包含目标词
bool HeaderContains(std::string_view header, std::string_view token){
    header = Trim(header);
    token = Trim(token);
    size_t pos = 0;
    while (pos < header.size())
    {
        size_t comma = header.find(',', pos);
        std::string_view part =
            (comma == std::string_view::npos) ? header.substr(pos)
                                                : header.substr(pos, comma - pos);
        part = Trim(part);
        if (IEquals(part, token))
            return true;
        if (comma == std::string_view::npos)
            break;
        pos = comma + 1;
    }
    return false;
}
//WebSocket 握手用的固定魔术字符串(RFC 6455 规定)
constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 客户端发来的 JSON 命令结构体（订阅/取消订阅）
struct WsCommand{
    std::string action;
    std::string user_id;
    std::vector<uint32_t> lights;
};
// JSON 字符串转义（把 " 和 \ 前面加反斜杠）
std::string EscapeJson(std::string_view in){
    std::string out;
    out.reserve(in.size());
    for(char ch : in){
        switch (ch)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
}

/*Sha1 就是把 client_key + GUID 算出一个 固定20 字节的哈希值，
再 Base64 编码后发回给浏览器*/
class Sha1{
private:
    void Reset(){
        state_[0] = 0x67452301u;
        state_[1] = 0xEFCDAB89u;
        state_[2] = 0x98BADCFEu;
        state_[3] = 0x10325476u;
        state_[4] = 0xC3D2E1F0u;
        bit_count_ = 0;
        buffer_len_ = 0;
    }

    static uint32_t RotateLeft(uint32_t x, uint32_t n){
        return (x << n) | (x >> (32 - n));
    }
    static uint32_t F(uint32_t t, uint32_t b, uint32_t c, uint32_t d)
    {
        if (t < 20)
            return (b & c) | ((~b) & d);
        if (t < 40)
            return b ^ c ^ d;
        if (t < 60)
            return (b & c) | (b & d) | (c & d);
        return b ^ c ^ d;
    }

    static uint32_t K(uint32_t t)
    {
        if (t < 20)
            return 0x5A827999u;
        if (t < 40)
            return 0x6ED9EBA1u;
        if (t < 60)
            return 0x8F1BBCDCu;
        return 0xCA62C1D6u;
    }

    void Transform(const uint8_t block[64])
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (block[i * 4 + 0] << 24) | (block[i * 4 + 1] << 16) |
                    (block[i * 4 + 2] << 8) | (block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i)
        {
            w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        for (int t = 0; t < 80; ++t)
        {
            uint32_t temp =
                RotateLeft(a, 5) + F(t, b, c, d) + e + K(t) + w[t];
            e = d;
            d = c;
            c = RotateLeft(b, 30);
            b = a;
            a = temp;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }
    uint32_t state_[5];
    uint64_t bit_count_ = 0;
    uint8_t buffer_[64];
    size_t buffer_len_ = 0;


public:
    Sha1(){Reset();}
    void Update(const uint8_t *data, size_t len)
    {
        while (len > 0)
        {
            size_t to_copy = std::min(len, sizeof(buffer_) - buffer_len_);
            std::memcpy(buffer_ + buffer_len_, data, to_copy);
            buffer_len_ += to_copy;
            data += to_copy;
            len -= to_copy;
            if (buffer_len_ == sizeof(buffer_))
            {
                Transform(buffer_);
                bit_count_ += 512;
                buffer_len_ = 0;
            }
        }
    }

    std::array<uint8_t, 20> Final()
    {
        uint64_t total_bits = bit_count_ + buffer_len_ * 8;
        buffer_[buffer_len_++] = 0x80;
        if (buffer_len_ > 56)
        {
            while (buffer_len_ < 64)
                buffer_[buffer_len_++] = 0;
            Transform(buffer_);
            buffer_len_ = 0;
        }
        while (buffer_len_ < 56)
            buffer_[buffer_len_++] = 0;
        for (int i = 7; i >= 0; --i)
        {
            buffer_[buffer_len_++] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
        }
        Transform(buffer_);

        std::array<uint8_t, 20> digest{};
        for (int i = 0; i < 5; ++i)
        {
            digest[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
            digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
            digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
            digest[i * 4 + 3] = static_cast<uint8_t>((state_[i]) & 0xFF);
        }
        return digest;
    }
};
// Base64 编码（握手计算 AcceptKey 用）
std::string Base64Encode(const uint8_t *data, size_t len)
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i < len)
    {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back(kTable[(triple >> 6) & 0x3F]);
        out.push_back(kTable[triple & 0x3F]);
    }
    size_t mod = len % 3;
    if (mod > 0)
    {
        out[out.size() - 1] = '=';
        if (mod == 1)
        {
            out[out.size() - 2] = '=';
        }
    }
    return out;
}
/*
计算 AcceptKey 的目的是验证服务器真的懂 WebSocket，不是误打误撞连上的。过程很简单：
浏览器：我生成一个随机 key "dGhlIHNhbXBsZSBub25jZQ=="，发给服务器
服务器：把 key 和固定 GUID 拼起来 → 算 SHA-1 → Base64 编码 → 发回给浏览器
浏览器：自己也算一遍，对比结果是不是一样
    一样 → 服务器确实懂 WebSocket，握手成功
    不一样 → 服务器不懂，断开*/
std::string ComputeAcceptKey(const std::string &client_key)
{
    std::string combined = client_key;
    combined.append(kWebSocketGuid);// 拼上固定 GUID
    Sha1 sha;// SHA-1 哈希
    sha.Update(reinterpret_cast<const uint8_t *>(combined.data()), combined.size());
    auto digest = sha.Final();  // 得到 20 字节哈希值
    return Base64Encode(digest.data(), digest.size());// Base64 编码成字符串
}

// 从 JSON 中提取字符串字段，如 "action":"subscribe" → "subscribe"
bool ExtractStringField(std::string_view json, std::string_view key,
                        std::string *out)
{
    std::string pattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string_view::npos)
        return false;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string_view::npos)
        return false;
    ++pos;
    while (pos < json.size() &&
            std::isspace(static_cast<unsigned char>(json[pos])))
    {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"')
        return false;
    ++pos;
    std::string value;
    while (pos < json.size())
    {
        char ch = json[pos];
        if (ch == '"')
        {
            *out = value;
            return true;
        }
        if (ch == '\\' && pos + 1 < json.size())
        {
            value.push_back(json[pos + 1]);
            pos += 2;
        }
        else
        {
            value.push_back(ch);
            ++pos;
        }
    }
    return false;
}

// 从 JSON 中提取 uint 数组字段，如 "lights":[1,2,3] → {1,2,3}
bool ExtractUintArrayField(std::string_view json, std::string_view key,
                            std::vector<uint32_t> *out)
{
    std::string pattern = "\"" + std::string(key) + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string_view::npos)
        return false;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string_view::npos)
        return false;
    ++pos;
    while (pos < json.size() &&
            std::isspace(static_cast<unsigned char>(json[pos])))
    {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '[')
        return false;
    ++pos;
    out->clear();
    while (pos < json.size())
    {
        while (pos < json.size() &&
                std::isspace(static_cast<unsigned char>(json[pos])))
        {
            ++pos;
        }
        if (pos >= json.size())
            return false;
        if (json[pos] == ']')
        {
            ++pos;
            return true;
        }
        size_t start = pos;
        while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])))
        {
            ++pos;
        }
        if (start == pos)
            return false;
        uint32_t v = static_cast<uint32_t>(std::strtoul(
            std::string(json.substr(start, pos - start)).c_str(), nullptr, 10));
        out->push_back(v);
        while (pos < json.size() &&
                std::isspace(static_cast<unsigned char>(json[pos])))
        {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ',')
        {
            ++pos;
            continue;
        }
    }
    return false;
}

// 解析客户端发来的 JSON 命令，填充 WsCommand 结构体
bool ParseCommand(std::string_view payload, WsCommand *cmd)
{
    cmd->action.clear();
    cmd->user_id.clear();
    cmd->lights.clear();
    if (!ExtractStringField(payload, "action", &cmd->action))
    {
        return false;
    }
    ExtractStringField(payload, "user_id", &cmd->user_id);
    ExtractUintArrayField(payload, "lights", &cmd->lights);
    return true;
}


}//namespace

WebSocketServer::WebSocketServer(EventLoop*loop, const InetAddress &addr, int io_thrads)
: base_loop_(loop),server_(loop,addr,io_thrads,/*reuse_port*/ false){
    server_.SetConnectionCallback(
        [this](const ConnectionPtr &c){
            OnConnection(c);
        });
    server_.SetMessageCallback(
        [this](const ConnectionPtr &c, Buffer *buf){
            OnMessage(c,buf);
        });
}

WebSocketServer::~WebSocketServer()=default;
void WebSocketServer:: Start(){server_.Start();}

void WebSocketServer::PublishTo(uint32_t id, std::string_view payload){
    std::vector<std::shared_ptr<TcpConnection>> targets;
    {
        std::lock_guard<std::mutex> lk(subs_mu_);
        auto it = subs_.find(id);
        if(it == subs_.end()) return;
        auto &vec=it->second;
        for(auto iter = vec.begin();iter!=vec.end();){
            auto sp=iter->lock();
            if(!sp || !sp->IsConnected()){
                iter = vec.erase(iter);
                continue;
            }
            targets.push_back(sp);
            ++iter;
        }
        if(vec.empty()) subs_.erase(it);
    }
    if(targets.empty()) return;
    std::string frame =BuildFrame(/*opcode*/0x1,payload);
    for(auto & conn :targets){
        conn->Send(frame);
    }
}

void WebSocketServer::OnConnection(const ConnectionPtr &c){
    if(c->IsConnected()){
        Session sess;
        sess.state = Session::State::kHandshaking;
        sess.last_heartbeat = std::chrono::steady_clock::now();
        c->set_context(std::move(sess));
        LOG_INFO<<"WebSocket client connected fd="<<c->fd();
    }else{
        LOG_INFO << "WebSocket client closed fd=" << c->fd();
        RemoveConnection(c);
    }
}
void WebSocketServer::OnMessage(const ConnectionPtr& c, Buffer *buf){
    auto *sess = c->get_context<Session>();
    //防御性
    if(!sess){
        Session tmp;
        tmp.last_heartbeat = std::chrono::steady_clock::now();
        c->set_context(std::move(tmp));
        sess = c->get_context<Session>();
    }
    if(sess->state ==Session::State::kHandshaking){
        if(!HandleHandshake(c,sess,buf)) return;
    }
    if(sess->state ==Session::State::kOpen){
        HandleFrames(c,sess,buf);
    }

}
bool WebSocketServer::HandleHandshake(const ConnectionPtr &c, Session *sess, Buffer* buf){
    HttpParser parser;
    HttpRequest req;
    try{
        if(!parser.Parse(buf,&req)) return false;
    }
    catch(const std::exception &ex){
         LOG_WARN << "WebSocket handshake parse failed: " << ex.what();
         c->Shutdown();
         return false;
    }

    if(req.method()!=HttpRequest::Method::kGet){
        c->Send(std::string("HTTP/1.1 405 Method Not Allowed\r\n\r\n"));
        c->Shutdown();
        return false;
    }
    //string * 找不到可以返回nullptr
    const std::string *upgrade = req.header("Upgrade");
    const std::string *connection_hdr = req.header("Connection");
    const std::string *key = req.header("Sec-WebSocket-Key");
    const std::string *version = req.header("Sec-WebSocket-Version");

    //buf里的头部信息必须满足WebSocket握手要求
    if(!upgrade || !connection_hdr || !key || !version ||
            !IEquals(*upgrade, "websocket") ||
            !HeaderContains(*connection_hdr, "Upgrade") || *version != "13"){
        c->Send(std::string("HTTP/1.1 400 Bad Request\r\n\r\n"));
        c->Shutdown();
        return false;
    }

    std::string accept = ComputeAcceptKey(*key);
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: "+
        accept + "\r\n\r\n";
    c->Send(response);
    //握手成功，切换到 WebSocket 模式。
    //浏览器收到响应后会自行校验 accept key，如果不匹配浏览器会断开连接。
    sess->state = Session::State::kOpen;
    sess->last_heartbeat= std::chrono::steady_clock::now();
    if(sess->user_id.empty()){
        sess->user_id = "web-"+ std::to_string(c->fd());
    }

    //给客户端发 {"type":"ready"}，告诉它可以开始发命令了
    SendJsonMessage(c,"ready");
    if (buf->readable_bytes() > 0)//握手请求里可能夹带了 WebSocket 帧（HTTP 和帧数据在同一个 TCP 包里），要立刻处理
    {
        HandleFrames(c, sess, buf);
    }
    return true;
}
void WebSocketServer::HandleFrames(const ConnectionPtr&c, Session *sess,Buffer* buf){
    while (true)
    {
        bool fin =false;
        uint8_t opcode = 0;
        std::string payload;
        if(!ParseFrame(buf,&fin,&opcode,&payload)) break;
        if(!fin){
            LOG_WARN << "Fragmented frames not supported fd=" << c->fd();
            SendClose(c, 1003, "fragment not supported");
            return;
        }
        switch(opcode){
            case 0x1: // 为text  
                HandleTextFrame(c, sess, payload);//则解析 JSON 命令
                break;
            case 0x8: // close  
                HandleCloseFrame(c, sess, payload);//断开连接
                return;
            case 0x9: // ping  
                HandlePingFrame(c, payload);//回pong
                break;
            case 0xA: // pong 
                sess->last_heartbeat = std::chrono::steady_clock::now();//更新心跳时间
                break;
            default:
                LOG_WARN << "Unsupported WebSocket opcode=" << static_cast<int>(opcode)
                         << " fd=" << c->fd();
                SendClose(c, 1003, "unsupported opcode");
                return;       
        }
    }  
}

bool WebSocketServer::ParseFrame(Buffer *buf, bool* fin, uint8_t* opcode, std::string*payload){
    if(buf->readable_bytes()<2) return false;
    const uint8_t *data = reinterpret_cast<const uint8_t*>(buf->peek());
    size_t len =buf->readable_bytes();
    size_t pos =0;
    // 第1字节：FIN + opcode FIN = 这是不是完整消息的最后一帧1=是
    //opcode = 这帧的类型：0x1是文本，0x8是关闭，0x9是ping
    uint8_t b1 = data[pos++];
    /* 第2字节：MASK(数据有没有被加密（客户端→服务器必须加密，服务器→客户端不加密）) 
    + payload长度*/
    uint8_t b2 = data[pos++];
    bool masked = (b2 & 0x80) !=0;
    /*低7位 = 数据长度，但只有7位最大127，所以用特殊值 126/127 表示"长度在后面"*/
    uint64_t payload_len = (b2 & 0x7F);// 低7位是长度
    if(payload_len == 126)//后面两字节才是真长度
    {
        if (len < pos + 2)
            return false;
        payload_len = (static_cast<uint64_t>(data[pos]) << 8) |
                        static_cast<uint64_t>(data[pos + 1]);
        pos += 2;
    }else if (payload_len == 127)// 127 → 后面8字节才是真长度
    {
        if (len < pos + 8)
            return false;
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
        {
            payload_len = (payload_len << 8) | data[pos + i];
        }
        pos += 8;
    }
    uint8_t mask_key[4] = {0, 0, 0, 0};
    if (masked) // 有 mask → 读4字节 key
    {
        if (len < pos + 4)
            return false;
        std::memcpy(mask_key, data + pos, 4);
        pos += 4;
    }
    if (len < pos + payload_len)// 数据不够 → 等下次
        return false;
    payload->assign(reinterpret_cast<const char *>(data + pos),
                    static_cast<size_t>(payload_len)); // 拷贝 payload
    if (masked)
    {
        for (size_t i = 0; i < payload->size(); ++i)
        {
            (*payload)[i] ^= mask_key[i % 4];// XOR 解密
        }
    }
    buf->retrieve(pos + static_cast<size_t>(payload_len)); // 消费掉这帧
    *fin = (b1 & 0x80) != 0;// 取 FIN 位
    *opcode = b1 & 0x0F;// 取 opcode
    return true;
}
void WebSocketServer::HandleTextFrame(const ConnectionPtr&c, Session *sess, std::string_view payload){
    sess->last_heartbeat = std::chrono::steady_clock::now();
    WsCommand cmd;
    if(!ParseCommand(payload,&cmd)){
        SendJsonMessage(c, "error", "\"message\":\"bad payload\"");
        return;
    }
    if(cmd.action == "login"){
        if(cmd.user_id.empty()){
            SendJsonMessage(c,"error", "\"message\":\"missing user_id\"");
            return;
        }
        sess->user_id = cmd.user_id;
        SendJsonMessage(c, "login_ack",
                        "\"user_id\":\"" + EscapeJson(sess->user_id) + "\"");      
    }else if(cmd.action == "subscribe"){
        UpdateSubscriptions(c,sess,cmd.lights);
        SendJsonMessage(c,"subscribe_ack",
                            "\"count\":" + std::to_string(sess->subscribed.size()));
    }
    else if (cmd.action == "ping")
    {
        SendJsonMessage(c, "pong");
    }
    else
    {
        SendJsonMessage(c, "error", "\"message\":\"unknown action\"");
    }
}
void WebSocketServer::HandlePingFrame(const ConnectionPtr&c, std::string_view payload){
    SendPong(c,payload);
}
void WebSocketServer::HandleCloseFrame(const ConnectionPtr &c, Session *sess,
                                        std::string_view /*payload*/)
{
    sess->state = Session::State::kClosing;
    RemoveConnection(c);
    SendClose(c, 1000, "bye");
    c->Shutdown();
}

void WebSocketServer::SendText(const ConnectionPtr &c,
                                std::string_view payload)
{
    std::string frame = BuildFrame(/*opcode=*/0x1, payload);
    c->Send(frame);
}
void WebSocketServer::SendJsonMessage(const ConnectionPtr &c,
                                          const std::string &type,
                                          const std::string &body_kv)
{
    std::string payload = "{\"type\":\"" + type + "\"";
    if (!body_kv.empty())
    {
        payload.append(",").append(body_kv);
    }
    payload.append("}");
    SendText(c, payload);
}

void WebSocketServer::SendClose(const ConnectionPtr &c, uint16_t code,
                                std::string_view reason)
{
    std::string payload;
    payload.reserve(2 + reason.size());
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason.data(), reason.size());
    std::string frame = BuildFrame(/*opcode=*/0x8, payload);
    c->Send(frame);
}

void WebSocketServer::SendPong(const ConnectionPtr &c,
                                std::string_view payload)
{
    std::string frame = BuildFrame(/*opcode=*/0xA, payload);
    c->Send(frame);
}

void WebSocketServer::UpdateSubscriptions(const ConnectionPtr &c,
                                            Session *sess,
                                            const std::vector<uint32_t> &new_ids)
{
    std::vector<uint32_t> normalized = new_ids;
    normalized.erase(std::remove(normalized.begin(), normalized.end(), 0),
                        normalized.end());
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()),
                        normalized.end());
    {
        std::lock_guard<std::mutex> lk(subs_mu_);
        for (uint32_t id : sess->subscribed)
        {
            auto it = subs_.find(id);
            if (it == subs_.end())
                continue;
            auto &vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                        [&](const std::weak_ptr<TcpConnection> &wp)
                                        {
                                            auto sp = wp.lock();
                                            return !sp || sp.get() == c.get();
                                        }),
                        vec.end());
            if (vec.empty())
                subs_.erase(it);
        }
        for (uint32_t id : normalized)
        {
            subs_[id].push_back(c);
        }
    }
    sess->subscribed = std::move(normalized);
}

//连接断开时清理
void WebSocketServer::RemoveConnection(const ConnectionPtr &c)
{
    Session *sess = c->get_context<Session>();
    if (!sess)
        return;
    std::lock_guard<std::mutex> lk(subs_mu_);
    for (uint32_t id : sess->subscribed)//// 遍历这个连接订阅的所有灯
    {
        auto it = subs_.find(id);
        if (it == subs_.end())
            continue;
        auto &vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                    [&](const std::weak_ptr<TcpConnection> &wp)
                                    {
                                        auto sp = wp.lock();
                                        return !sp || sp.get() == c.get();//一种是找到目标了get返回shared_ptr 里的裸指针地址，另一种是发现垃圾顺手清理。
                                    }),
                    vec.end());
        if (vec.empty())
            subs_.erase(it);
    }
    sess->subscribed.clear();
}

void WebSocketServer::PublishTo(uint32_t id, std::string_view payload)
{
    std::vector<std::shared_ptr<TcpConnection>> targets;
    //锁内收集
    {
        std::lock_guard<std::mutex> lk(subs_mu_);
        auto it = subs_.find(id);
        if (it == subs_.end())
            return;
        auto &vec = it->second;
        for (auto iter = vec.begin(); iter != vec.end();)
        {
            auto sp = iter->lock();
            if (!sp || !sp->IsConnected())
            {
                iter = vec.erase(iter);
                continue;
            }
            targets.push_back(sp);
            ++iter;
        }
        if (vec.empty())
            subs_.erase(it);
    }
    if (targets.empty())
        return;
    //锁外发送
    std::string frame = BuildFrame(/*opcode=*/0x1, payload);
    for (auto &c : targets)
    {
        c->Send(frame);
    }
}

std::string WebSocketServer::BuildFrame(uint8_t opcode,
                                        std::string_view payload, bool fin)
{
    std::string frame;
    size_t payload_len = payload.size();
    frame.reserve(2 + payload_len + 8);
    uint8_t first = opcode | (fin ? 0x80 : 0x00);
    frame.push_back(static_cast<char>(first));
    if (payload_len <= 125)
    {
        frame.push_back(static_cast<char>(payload_len));
    }
    else if (payload_len <= 0xFFFF)
    {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload_len & 0xFF));
    }
    else
    {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
        {
            frame.push_back(static_cast<char>((payload_len >> (i * 8)) & 0xFF));
        }
    }
    frame.append(payload.data(), payload.size());
    return frame;
}

}//netx