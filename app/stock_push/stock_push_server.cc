// 股票推送服务器 Demo（基于 netx / epoll）
//
// 功能概要：
// - 单机多连接长连接服务器
// - 客户端通过 LOGIN / SUBSCRIBE / PING 协议与服务器交互
// - 服务器内部模拟股票行情数据，并向订阅该股票代码的客户端推送 STOCK_UPDATE
//
// 说明：
// - 为了实现简单、聚焦网络与并发，本文件将主要逻辑集中在一个编译单元中。
// - 多 IO 线程支持可以后续扩展；当前默认使用 1 个 IO 线程，便于避免额外锁开销。

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>
#include <functional>

#include "netx/buffer.h"
#include "netx/event_loop.h"
#include "netx/http_server.h"
#include "netx/inet_address.h"
#include "netx/logging.h"
#include "netx/tcp_connection.h"
#include "netx/tcp_server.h"
#include "netx/websocket_server.h"

using netx::Buffer;
using netx::EventLoop;
using netx::InetAddress;
using netx::HttpServer;
using netx::TcpConnection;
using netx::TcpServer;
using netx::WebSocketServer;

namespace {

// 协议消息类型
enum class MsgType : uint16_t {
  kLogin = 1,
  kSubscribe = 2,
  kPing = 3,
  kPong = 4,
  kStockUpdate = 100,
};

// 行情更新数据
struct StockUpdate {
  std::string code;        // 股票代码
  double price;            // 当前价格
  double change_pct;       // 涨跌幅 (%)
  uint64_t volume;         // 成交量
  uint64_t timestamp;      // 时间戳 (毫秒)
};

// 会话状态：挂在 TcpConnection::context() 中
struct Session {
  std::string user_id;
  std::vector<std::string> subscribed_codes;
  std::chrono::steady_clock::time_point last_heartbeat;
};

// 简单的工具：打包一条协议消息
std::string MakePacket(MsgType type, const void* body, size_t body_len) {
  const uint32_t total_len = static_cast<uint32_t>(sizeof(uint32_t) +
                                                   sizeof(uint16_t) + body_len);
  std::string out;
  out.resize(total_len);
  char* p = out.data();

  uint32_t len_n = htonl(total_len);
  std::memcpy(p, &len_n, sizeof(len_n));

  uint16_t type_n = htons(static_cast<uint16_t>(type));
  std::memcpy(p + sizeof(uint32_t), &type_n, sizeof(type_n));

  if (body_len > 0 && body) {
    std::memcpy(p + sizeof(uint32_t) + sizeof(uint16_t), body, body_len);
  }
  return out;
}

// 构造 LOGIN 消息
std::string BuildLogin(const std::string& user) {
  uint16_t len = static_cast<uint16_t>(user.size());
  uint16_t len_n = htons(len);
  std::string body;
  body.resize(sizeof(len_n) + user.size());
  std::memcpy(body.data(), &len_n, sizeof(len_n));
  std::memcpy(body.data() + sizeof(len_n), user.data(), user.size());
  return MakePacket(MsgType::kLogin, body.data(), body.size());
}

// 构造 SUBSCRIBE 消息
std::string BuildSubscribe(const std::vector<std::string>& codes) {
  uint16_t cnt = static_cast<uint16_t>(codes.size());
  uint16_t cnt_n = htons(cnt);
  
  // 计算总长度
  size_t total_body_len = sizeof(cnt_n);
  for (const auto& code : codes) {
    total_body_len += sizeof(uint16_t) + code.size();
  }
  
  std::string body;
  body.resize(total_body_len);
  char* p = body.data();
  
  // 写入数量
  std::memcpy(p, &cnt_n, sizeof(cnt_n));
  p += sizeof(cnt_n);
  
  // 写入每个股票代码
  for (const auto& code : codes) {
    uint16_t code_len = static_cast<uint16_t>(code.size());
    uint16_t code_len_n = htons(code_len);
    std::memcpy(p, &code_len_n, sizeof(code_len_n));
    p += sizeof(code_len_n);
    std::memcpy(p, code.data(), code.size());
    p += code.size();
  }
  
  return MakePacket(MsgType::kSubscribe, body.data(), body.size());
}

// 构造 PING 消息
std::string BuildPing() {
  return MakePacket(MsgType::kPing, nullptr, 0);
}

std::string ReadFileIfExists(const std::filesystem::path& path) {
  std::ifstream fin(path, std::ios::binary);
  if (!fin.is_open()) return {};
  std::ostringstream oss;
  oss << fin.rdbuf();
  return oss.str();
}

//字符串查找替换。就是把 {{WS_PORT}} 换成 "9100"
std::string ReplaceAllTokens(std::string text, std::string_view from,
                             std::string_view to) {
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

constexpr char kFallbackIndexHtml[] = R"(<!DOCTYPE html>
<html lang="zh">
<head>
  <meta charset="utf-8" />
  <title>股票行情推送系统</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <link rel="stylesheet" href="/style.css" />
</head>
<body>
  <div class="container">
    <h1>股票行情实时推送系统</h1>
    <h2>基于 epoll 的实时股票行情推送</h2>
    <p class="status" id="status">正在连接...</p>
    <div class="controls">
      <label>订阅股票代码（用逗号分隔）</label>
      <div class="control-row">
        <input id="stock-input" value="STOCK1,STOCK2,STOCK3" />
        <button id="subscribe-btn">更新订阅</button>
      </div>
    </div>
    <div id="stocks" class="stocks"></div>
  </div>
  <script>
    window.__WS_PORT__ = {{WS_PORT}};
  </script>
  <script src="/app.js"></script>
</body>
</html>)";

constexpr char kFallbackAppJs[] = R"(const statusEl=document.getElementById('status');
const stocksEl=document.getElementById('stocks');
const inputEl=document.getElementById('stock-input');
const btnEl=document.getElementById('subscribe-btn');
const wsPort=window.__WS_PORT__||(window.location.port||(window.location.protocol==='https:'?443:80));
const wsScheme=window.location.protocol==='https:'?'wss':'ws';
const wsUrl=`${wsScheme}://${window.location.hostname}:${wsPort}/ws`;
let ws=null;let reconnectTimer=null;
const stocks=new Map();let currentCodes=new Set();
const codeToId=new Map();let nextId=1;
const getId=(code)=>{if(!codeToId.has(code)){codeToId.set(code,nextId++);}
return codeToId.get(code);};
const setStatus=(text)=>{statusEl.textContent=text;};
const render=()=>{if(stocks.size===0){stocksEl.innerHTML='';stocksEl.style.display='none';return;}
stocksEl.style.display='grid';stocksEl.innerHTML='';
Array.from(stocks.entries()).sort((a,b)=>a[0].localeCompare(b[0])).forEach(([code,info])=>{
const changeClass=info.change_pct>=0?'up':'down';
const card=document.createElement('div');
card.className=`stock-card ${changeClass}`;
card.innerHTML=`<div class="stock-code">${code}</div>
<div class="stock-price">${info.price.toFixed(2)}</div>
<div class="stock-change">${info.change_pct>=0?'+':''}${info.change_pct.toFixed(2)}%</div>
<div class="stock-volume">成交量: ${info.volume.toLocaleString()}</div>`;
stocksEl.appendChild(card);
});};
const send=(payload)=>{if(ws&&ws.readyState===WebSocket.OPEN){ws.send(JSON.stringify(payload));}};
const sendSubscribe=()=>{const codes=(inputEl.value||'').split(',').map((code)=>code.trim()).filter(Boolean);
currentCodes=new Set(codes);stocks.forEach((_,code)=>{if(!currentCodes.has(code)){stocks.delete(code);}});
const ids=codes.map(code=>getId(code));render();send({action:'subscribe',lights:ids});};
const connect=()=>{setStatus(`连接中 ${wsUrl}`);ws=new WebSocket(wsUrl);
ws.onopen=()=>{setStatus('连接成功');send({action:'login',user_id:`web-${Date.now()}`});sendSubscribe();};
ws.onmessage=(evt)=>{try{const msg=JSON.parse(evt.data);
if(msg.type==='stock_update'){if(currentCodes.size>0&&!currentCodes.has(msg.code)){return;}
stocks.set(msg.code,{price:msg.price,change_pct:msg.change_pct,volume:msg.volume});render();}
else if(msg.type==='subscribe_ack'){setStatus(`已订阅 ${msg.count||0} 只股票`);}
else if(msg.type==='ready'){setStatus('连接成功，等待推送');}
else if(msg.type==='error'){setStatus(`错误：${msg.message||'未知'}`);}}
catch(err){console.error('bad message',err);}};
ws.onclose=()=>{setStatus('连接断开，3 秒后自动重试');
if(!reconnectTimer){reconnectTimer=setTimeout(()=>{reconnectTimer=null;connect();},3000);}};};
btnEl.addEventListener('click',sendSubscribe);connect();)";

constexpr char kFallbackStyleCss[] = R"(
body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  margin: 0;
  padding: 0;
  background: #f5f7fb;
  color: #1f2a44;
}
.container {
  max-width: 960px;
  margin: 0 auto;
  padding: 24px;
}
.controls {
  margin-bottom: 16px;
}
.control-row {
  display: flex;
  gap: 8px;
  margin-top: 8px;
}
input {
  flex: 1;
  padding: 8px 12px;
  font-size: 16px;
}
button {
  padding: 8px 16px;
  font-size: 16px;
  cursor: pointer;
}
.stocks {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 12px;
}
.stock-card {
  border-radius: 8px;
  padding: 16px;
  background: #fff;
  box-shadow: 0 2px 6px rgba(0,0,0,0.08);
}
.stock-card.up { border-left: 4px solid #ef5350; }
.stock-card.down { border-left: 4px solid #66bb6a; }
.stock-code { font-weight: bold; margin-bottom: 8px; }
.stock-price { font-size: 20px; margin-bottom: 4px; }
.stock-change { font-size: 14px; margin-bottom: 4px; }
.stock-change.up { color: #ef5350; }
.stock-change.down { color: #66bb6a; }
.stock-volume { font-size: 12px; color: #607d8b; }
.status { margin: 8px 0; color: #607d8b; }
)";
constexpr char kEmptyFavicon[] = "";

// SUBSCRIBE 管理：stock_code -> 订阅该股票的连接（弱引用）
class SubscriptionManager {
 public:
  using ConnectionPtr = std::shared_ptr<TcpConnection>;

  // 更新某连接的订阅集合：new_codes 为完整新集合
  void UpdateSubscriptions(const ConnectionPtr& conn,
                           const std::vector<std::string>& new_codes) {
    // 从旧订阅中移除该连接
    for (auto it = subs_.begin(); it != subs_.end();) {
      auto& vec = it->second;
      vec.erase(std::remove_if(vec.begin(), vec.end(),
                               [&](const std::weak_ptr<TcpConnection>& wp) {
                                 auto sp = wp.lock();
                                 return !sp || sp.get() == conn.get();
                               }),
                vec.end());
      if (vec.empty()) {
        it = subs_.erase(it);
      } else {
        ++it;
      }
    }

    // 记录新订阅并加入映射
    for (const auto& code : new_codes) {
      subs_[code].push_back(conn);
    }
  }

  // 连接关闭时清理其所有订阅
  void RemoveConnection(const ConnectionPtr& conn) {
    for (auto it = subs_.begin(); it != subs_.end();) {
      auto& vec = it->second;
      vec.erase(std::remove_if(vec.begin(), vec.end(),
                               [&](const std::weak_ptr<TcpConnection>& wp) {
                                 auto sp = wp.lock();
                                 return !sp || sp.get() == conn.get();
                               }),
                vec.end());
      if (vec.empty()) {
        it = subs_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // 遍历某个股票代码的所有订阅连接；回调在调用者所在线程执行
  template <typename F>
  void ForEachSubscriber(const std::string& code, F&& f) {
    auto it = subs_.find(code);
    if (it == subs_.end()) return;
    auto& vec = it->second;
    for (auto iter = vec.begin(); iter != vec.end();) {
      auto sp = iter->lock();
      if (!sp || !sp->IsConnected()) {
        iter = vec.erase(iter);
        continue;
      }
      f(sp);
      ++iter;
    }
    if (vec.empty()) {
      subs_.erase(it);
    }
  }

  size_t StockCount() const { return subs_.size(); }

 private:
  std::unordered_map<std::string, std::vector<std::weak_ptr<TcpConnection>>> subs_;
};

// 将 StockUpdate 转成发给 WebSocket 客户端的 JSON 文本
std::string BuildStockUpdateJson(const StockUpdate& update) {
  std::string payload = "{\"type\":\"stock_update\",\"code\":\"";
  payload.append(update.code);
  payload.append("\",\"price\":");
  payload.append(std::to_string(update.price));
  payload.append(",\"change_pct\":");
  payload.append(std::to_string(update.change_pct));
  payload.append(",\"volume\":");
  payload.append(std::to_string(update.volume));
  payload.append("}");
  return payload;
}

// 股票行情模拟器：独立线程生成行情数据，通过 EventLoop::QueueInLoop 投递
class StockSimulator {
 public:
  using UpdateCallback = std::function<void(const StockUpdate&)>;

  StockSimulator(EventLoop* loop, uint32_t stock_count, int tick_ms,
                 uint32_t updates_per_tick, UpdateCallback cb)
      : loop_(loop),
        stocks_(stock_count),
        tick_ms_(tick_ms),
        updates_per_tick_(updates_per_tick == 0 ? 1u : updates_per_tick),
        cb_(std::move(cb)) {
    InitializeStocks();
  }

  ~StockSimulator() { Stop(); }

  void Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    worker_ = std::thread([this]() { Run(); });
  }

  void Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  struct StockState {
    double price = 100.0;         // 初始价格
    double change_pct = 0.0;      // 涨跌幅
    uint64_t volume = 0;          // 成交量
    std::string code;             // 股票代码
  };

  void InitializeStocks() {
    for (size_t i = 0; i < stocks_.size(); ++i) {
      stocks_[i].code = "STOCK" + std::to_string(i + 1);
      stocks_[i].price = 10.0 + (rng_() % 1000);  // 10-1010 随机初始价格
      stocks_[i].change_pct = 0.0;
      stocks_[i].volume = rng_() % 1000000;
    }
  }

  void Run() {
    if (stocks_.empty()) return;
    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::milliseconds(tick_ms_);
    while (running_.load()) {
      auto start = Clock::now();

      std::vector<StockUpdate> updates;
      GenerateBatchUpdates(updates);

      if (!updates.empty()) {
        loop_->QueueInLoop([this, ups = std::move(updates)]() mutable {
          for (const auto& u : ups) {
            cb_(u);
          }
        });
      }

      auto next = start + interval;
      std::this_thread::sleep_until(next);
    }
  }

  void GenerateBatchUpdates(std::vector<StockUpdate>& out) {
    const uint32_t n = static_cast<uint32_t>(stocks_.size());
    if (n == 0) return;
    out.clear();
    const uint32_t count = std::min(updates_per_tick_, n);
    out.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
      uint32_t idx = next_stock_;
      next_stock_ = (next_stock_ + 1) % n;
      auto& st = stocks_[idx];

      // 模拟价格波动 (-2% ~ +2%)
      double change = (static_cast<double>(rng_() % 400) - 200.0) / 100.0;
      st.price *= (1.0 + change / 100.0);
      st.change_pct = change;
      st.volume += rng_() % 10000;

      StockUpdate u;
      u.code = st.code;
      u.price = st.price;
      u.change_pct = st.change_pct;
      u.volume = st.volume;
      u.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      out.push_back(u);
    }
  }

  EventLoop* loop_;
  std::vector<StockState> stocks_;
  int tick_ms_ = 100;
  uint32_t updates_per_tick_ = 10;
  UpdateCallback cb_;

  std::atomic<bool> running_{false};
  std::thread worker_;
  uint32_t next_stock_ = 0;
  std::mt19937 rng_{std::random_device{}()};
};

// 主服务器对象
class StockPushServer {
 public:
  StockPushServer(EventLoop* base_loop, const InetAddress& listen_addr,
                  int io_threads, uint32_t stock_count, int tick_ms,
                  uint32_t updates_per_tick, int http_port, int ws_port,
                  std::string web_root)
      : base_loop_(base_loop),
        server_(base_loop, listen_addr, io_threads, /*reuse_port=*/false),
        simulator_(base_loop, stock_count, tick_ms, updates_per_tick,
                   [this](const StockUpdate& u) { OnStockUpdate(u); }),
        http_port_(http_port),
        ws_port_(ws_port),
        web_root_(std::move(web_root)) {
    server_.SetConnectionCallback(
        [this](const TcpServer::ConnectionPtr& c) { OnConnection(c); });
    server_.SetMessageCallback(
        [this](const TcpServer::ConnectionPtr& c, Buffer* b) {
          OnMessage(c, b);
        });

    if (http_port_ > 0) {
      InetAddress http_addr(static_cast<uint16_t>(http_port_));
      http_server_ =
          std::make_unique<HttpServer>(base_loop_, http_addr, 1, false);
      ConfigureStaticHttp();
    }
    if (ws_port_ > 0) {
      InetAddress ws_addr(static_cast<uint16_t>(ws_port_));
      ws_server_ =
          std::make_unique<WebSocketServer>(base_loop_, ws_addr, 1);
    }
  }

  void Start() {
    LOG_INFO << "StockPushServer starting... http_port=" << http_port_
             << " ws_port=" << ws_port_;
    if (http_server_) {
      http_server_->Start();
    }
    if (ws_server_) {
      ws_server_->Start();
    }
    simulator_.Start();
    server_.Start();
  }

 private:
  void ConfigureStaticHttp() {
    if (!http_server_) return;
    std::string ws_port_str = std::to_string(ws_port_);

    auto index = LoadWebAsset("index.html", kFallbackIndexHtml);
    index = ReplaceAllTokens(std::move(index), "{{WS_PORT}}", ws_port_str);
    http_server_->GetStatic("/", index, "text/html");

    auto app_js = LoadWebAsset("app.js", kFallbackAppJs);
    app_js = ReplaceAllTokens(std::move(app_js), "{{WS_PORT}}", ws_port_str);
    http_server_->GetStatic("/app.js", app_js, "application/javascript");

    auto style_css = LoadWebAsset("style.css", kFallbackStyleCss);
    http_server_->GetStatic("/style.css", style_css, "text/css");

    http_server_->GetStatic("/favicon.ico", LoadWebAsset("favicon.ico", kEmptyFavicon),
                            "image/x-icon");
  }

  std::string LoadWebAsset(const std::string& name,
                           const char* fallback) const {
    if (!web_root_.empty()) {
      std::filesystem::path candidate = std::filesystem::path(web_root_) / name;
      std::error_code ec;
      if (std::filesystem::exists(candidate, ec)) {
        auto content = ReadFileIfExists(candidate);
        if (!content.empty()) {
          return content;
        }
      }
    }
    return std::string(fallback);
  }

  void OnConnection(const TcpServer::ConnectionPtr& conn) {
    if (conn->IsConnected()) {
      Session sess;
      sess.last_heartbeat = std::chrono::steady_clock::now();
      conn->set_context(std::move(sess));
      LOG_INFO << "new connection fd=" << conn->fd();
    } else {
      LOG_INFO << "connection closed fd=" << conn->fd();
      subs_.RemoveConnection(conn);
    }
  }

  void OnMessage(const TcpServer::ConnectionPtr& conn, Buffer* buf) {
    while (buf->readable_bytes() >= sizeof(uint32_t) + sizeof(uint16_t)) {
      const char* data = buf->peek();
      uint32_t total_len_n = 0;
      std::memcpy(&total_len_n, data, sizeof(total_len_n));
      uint32_t total_len = ntohl(total_len_n);
      if (total_len < sizeof(uint32_t) + sizeof(uint16_t)) {
        LOG_WARN << "invalid packet length=" << total_len << " fd=" << conn->fd();
        conn->Shutdown();
        return;
      }
      if (buf->readable_bytes() < total_len) {
        break;
      }
      uint16_t type_n = 0;
      std::memcpy(&type_n, data + sizeof(uint32_t), sizeof(type_n));
      uint16_t type_v = ntohs(type_n);
      MsgType type = static_cast<MsgType>(type_v);

      const char* body = data + sizeof(uint32_t) + sizeof(uint16_t);
      size_t body_len = total_len - sizeof(uint32_t) - sizeof(uint16_t);

      switch (type) {
        case MsgType::kLogin:
          HandleLogin(conn, body, body_len);
          break;
        case MsgType::kSubscribe:
          HandleSubscribe(conn, body, body_len);
          break;
        case MsgType::kPing:
          HandlePing(conn);
          break;
        default:
          LOG_WARN << "unknown msg type=" << type_v << " fd=" << conn->fd();
          break;
      }

      buf->retrieve(total_len);
    }
  }

  void HandleLogin(const TcpServer::ConnectionPtr& conn, const char* body,
                   size_t len) {
    if (len < sizeof(uint16_t)) {
      LOG_WARN << "LOGIN body too short fd=" << conn->fd();
      return;
    }
    uint16_t id_len_n = 0;
    std::memcpy(&id_len_n, body, sizeof(id_len_n));
    uint16_t id_len = ntohs(id_len_n);
    if (len < sizeof(uint16_t) + id_len) {
      LOG_WARN << "LOGIN body invalid length fd=" << conn->fd();
      return;
    }
    std::string user_id(body + sizeof(uint16_t),
                        body + sizeof(uint16_t) + id_len);
    Session* sess = conn->get_context<Session>();
    if (!sess) {
      Session tmp;
      tmp.user_id = std::move(user_id);
      tmp.last_heartbeat = std::chrono::steady_clock::now();
      conn->set_context(std::move(tmp));
    } else {
      sess->user_id = std::move(user_id);
      sess->last_heartbeat = std::chrono::steady_clock::now();
    }
    LOG_INFO << "LOGIN fd=" << conn->fd() << " user=" << (sess ? sess->user_id : "");
  }

  void HandleSubscribe(const TcpServer::ConnectionPtr& conn, const char* body,
                       size_t len) {
    if (len < sizeof(uint16_t)) {
      LOG_WARN << "SUBSCRIBE body too short fd=" << conn->fd();
      return;
    }
    uint16_t cnt_n = 0;
    std::memcpy(&cnt_n, body, sizeof(cnt_n));
    uint16_t cnt = ntohs(cnt_n);

    std::vector<std::string> codes;
    const char* p = body + sizeof(uint16_t);
    size_t remaining = len - sizeof(uint16_t);

    for (uint16_t i = 0; i < cnt; ++i) {
      if (remaining < sizeof(uint16_t)) break;
      uint16_t code_len_n = 0;
      std::memcpy(&code_len_n, p, sizeof(code_len_n));
      uint16_t code_len = ntohs(code_len_n);
      p += sizeof(uint16_t);
      remaining -= sizeof(uint16_t);

      if (remaining < code_len) break;
      codes.emplace_back(p, code_len);
      p += code_len;
      remaining -= code_len;
    }

    subs_.UpdateSubscriptions(conn, codes);
    Session* sess = conn->get_context<Session>();
    LOG_INFO << "SUBSCRIBE fd=" << conn->fd() << " user="
             << (sess ? sess->user_id : "") << " count=" << codes.size();
  }

  void HandlePing(const TcpServer::ConnectionPtr& conn) {
    Session* sess = conn->get_context<Session>();
    if (sess) {
      sess->last_heartbeat = std::chrono::steady_clock::now();
    }
    static const std::string kPongPacket = MakePacket(MsgType::kPong, nullptr, 0);
    conn->Send(kPongPacket);
  }

  void OnStockUpdate(const StockUpdate& u) {
    // 构造二进制包
    std::string body;
    uint16_t code_len = static_cast<uint16_t>(u.code.size());
    uint16_t code_len_n = htons(code_len);
    body.append(reinterpret_cast<const char*>(&code_len_n), sizeof(code_len_n));
    body.append(u.code);

    double price = u.price;
    double change_pct = u.change_pct;
    uint64_t volume = u.volume;
    uint64_t timestamp = u.timestamp;

    body.append(reinterpret_cast<const char*>(&price), sizeof(price));
    body.append(reinterpret_cast<const char*>(&change_pct), sizeof(change_pct));
    body.append(reinterpret_cast<const char*>(&volume), sizeof(volume));
    body.append(reinterpret_cast<const char*>(&timestamp), sizeof(timestamp));

    std::string packet = MakePacket(MsgType::kStockUpdate, body.data(), body.size());

    // 推给 TCP 客户端
    subs_.ForEachSubscriber(u.code, [&packet](const std::shared_ptr<TcpConnection>& c) {
      c->Send(packet);
    });

    // 推给 WebSocket 客户端
    if (ws_server_) {
      uint32_t id = 0;
      auto it = code_to_id_.find(u.code);
      if (it != code_to_id_.end()) {
        id = it->second;
      } else {
        id = next_id_++;
        code_to_id_[u.code] = id;
      }
      std::string json = BuildStockUpdateJson(u);
      ws_server_->PublishTo(id, json);
    }
  }

  EventLoop* base_loop_;
  TcpServer server_;
  SubscriptionManager subs_;
  StockSimulator simulator_;
  int http_port_ = 0;
  int ws_port_ = 0;
  std::string web_root_;
  std::unique_ptr<HttpServer> http_server_;
  std::unique_ptr<WebSocketServer> ws_server_;
  std::unordered_map<std::string, uint32_t> code_to_id_;
  uint32_t next_id_ = 1;
};

struct ServerOptions {
  int port = 9000;
  int io_threads = 1;
  uint32_t stock_count = 100;
  int tick_ms = 100;
  uint32_t updates_per_tick = 10;
  int http_port = 9200;
  int ws_port = 9100;
  std::string web_root = "netx/web";
};

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog
            << " [--port <port>] [--io-threads <n>] [--stocks <count>]\n"
               "           [--tick-ms <ms>] [--updates-per-tick <n>]\n"
               "           [--http-port <port>] [--ws-port <port>]\n"
               "           [--web-root <dir>]\n"
               "\nDefault: port=9000, io-threads=1, stocks=100, tick-ms=100, "
               "updates-per-tick=10, http-port=9200, ws-port=9100\n";
}

bool ParseArgs(int argc, char* argv[], ServerOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](int& dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = std::stoi(argv[++i]);
      return true;
    };
    auto next_u32 = [&](uint32_t& dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = static_cast<uint32_t>(std::stoul(argv[++i]));
      return true;
    };
    auto next_str = [&](std::string& dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    if (arg == "--port") {
      if (!next(opts->port)) return false;
    } else if (arg == "--io-threads") {
      if (!next(opts->io_threads)) return false;
    } else if (arg == "--stocks") {
      if (!next_u32(opts->stock_count)) return false;
    } else if (arg == "--tick-ms") {
      if (!next(opts->tick_ms)) return false;
    } else if (arg == "--updates-per-tick") {
      if (!next_u32(opts->updates_per_tick)) return false;
    } else if (arg == "--http-port") {
      if (!next(opts->http_port)) return false;
    } else if (arg == "--ws-port") {
      if (!next(opts->ws_port)) return false;
    } else if (arg == "--web-root") {
      if (!next_str(opts->web_root)) return false;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return false;
    } else {
      std::cout << "Unknown arg: " << arg << "\n";
      PrintUsage(argv[0]);
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  ServerOptions opts;
  if (!ParseArgs(argc, argv, &opts)) {
    return 1;
  }

  LOG_INFO << "StockPushServer config: port=" << opts.port
           << " io_threads=" << opts.io_threads
           << " stocks=" << opts.stock_count
           << " tick_ms=" << opts.tick_ms
           << " updates_per_tick=" << opts.updates_per_tick
           << " http_port=" << opts.http_port
           << " ws_port=" << opts.ws_port
           << " web_root=" << opts.web_root;

  EventLoop loop(/*single_thread_mode=*/opts.io_threads == 1);
  InetAddress addr(static_cast<uint16_t>(opts.port));
  StockPushServer server(&loop, addr, opts.io_threads, opts.stock_count,
                           opts.tick_ms, opts.updates_per_tick,
                           opts.http_port, opts.ws_port, opts.web_root);
  server.Start();
  loop.Loop();
  return 0;
}
