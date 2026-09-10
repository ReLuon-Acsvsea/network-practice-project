// 高并发红绿灯推送服务器 V2（基于数据源接口）
//
// 功能概要：
// - 使用 DataSource 接口获取红绿灯数据
// - 支持多种数据源（模拟、HTTP、Redis 等）
// - 客户端通过 LOGIN / SUBSCRIBE / PING 协议与服务器交互
// - 服务器推送红绿灯状态更新
//
// 编译：make traffic_push_server_v2
// 运行：./traffic_push_server_v2 [--port 9000] [--ws-port 9100] [--http-port 9200]

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
#include "netx/traffic_light.h"
#include "netx/data_source.h"
#include "netx/redis_data_source.h"
#include "netx/simulator_data_source.h"
#include "netx/websocket_server.h"

using netx::Buffer;
using netx::EventLoop;
using netx::InetAddress;
using netx::HttpServer;
using netx::TcpConnection;
using netx::TcpServer;
using netx::LightUpdate;
using netx::LightColor;
using netx::DataSource;
using netx::RedisDataSource;
using netx::SimulatorDataSource;
using netx::Intersection;
using netx::TrafficLight;
using netx::WebSocketServer;

namespace {

// 协议消息类型
enum class MsgType : uint16_t {
  kLogin = 1,
  kSubscribe = 2,
  kUnsubscribe = 3,
  kPing = 4,
  kPong = 5,
  kLightUpdate = 100,
};

// 会话状态：挂在 TcpConnection::context() 中
struct Session {
  std::string user_id;
  std::vector<std::string> subscribed_lights;
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
std::string BuildSubscribe(const std::vector<std::string>& lights) {
  uint16_t cnt = static_cast<uint16_t>(lights.size());
  uint16_t cnt_n = htons(cnt);

  // 计算总长度
  size_t total_body_len = sizeof(cnt_n);
  for (const auto& light : lights) {
    total_body_len += sizeof(uint16_t) + light.size();
  }

  std::string body;
  body.resize(total_body_len);
  char* p = body.data();

  // 写入数量
  std::memcpy(p, &cnt_n, sizeof(cnt_n));
  p += sizeof(cnt_n);

  // 写入每个灯ID
  for (const auto& light : lights) {
    uint16_t light_len = static_cast<uint16_t>(light.size());
    uint16_t light_len_n = htons(light_len);
    std::memcpy(p, &light_len_n, sizeof(light_len_n));
    p += sizeof(light_len_n);
    std::memcpy(p, light.data(), light.size());
    p += light.size();
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

//字符串查找替换
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
  <title>红绿灯推送系统 V2</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <link rel="stylesheet" href="/style.css" />
</head>
<body>
  <div class="container">
    <h1>红绿灯实时推送系统 V2</h1>
    <h2>基于数据源接口的实时红绿灯推送</h2>
    <p class="status" id="status">正在连接...</p>
    <div class="controls">
      <label>订阅红绿灯ID（用逗号分隔）</label>
      <div class="control-row">
        <input id="light-input" value="L-1,L-2,L-3,L-4" />
        <button id="subscribe-btn">更新订阅</button>
      </div>
    </div>
    <div id="lights" class="lights"></div>
  </div>
  <script>
    window.__WS_PORT__ = {{WS_PORT}};
  </script>
  <script src="/app.js"></script>
</body>
</html>)";

constexpr char kFallbackAppJs[] = R"(const statusEl=document.getElementById('status');
const lightsEl=document.getElementById('lights');
const inputEl=document.getElementById('light-input');
const btnEl=document.getElementById('subscribe-btn');
const wsPort=window.__WS_PORT__||(window.location.port||(window.location.protocol==='https:'?443:80));
const wsScheme=window.location.protocol==='https:'?'wss':'ws';
const wsUrl=`${wsScheme}://${window.location.hostname}:${wsPort}/ws`;
let ws=null;let reconnectTimer=null;
const lights=new Map();let currentIds=new Set();
const idToName=new Map();let nextId=1;
const getName=(id)=>{if(!idToName.has(id)){idToName.set(id,nextId++);}
return idToName.get(id);};
const setStatus=(text)=>{statusEl.textContent=text;};
const colorName=(color)=>{switch(color){case 0:return'红';case 1:return'黄';case 2:return'绿';default:return'未知';}};
const render=()=>{if(lights.size===0){lightsEl.innerHTML='';lightsEl.style.display='none';return;}
lightsEl.style.display='grid';lightsEl.innerHTML='';
Array.from(lights.entries()).sort((a,b)=>{const na=parseInt(a[0].replace(/\D/g,''))||0;const nb=parseInt(b[0].replace(/\D/g,''))||0;return na-nb;}).forEach(([id,info])=>{
const colorClass=info.color===0?'red':info.color===1?'yellow':'green';
const card=document.createElement('div');
card.className=`light-card ${colorClass}`;
card.innerHTML=`<div class="light-id">${id}</div>
<div class="light-direction">${info.direction}</div>
<div class="light-color">${colorName(info.color)}</div>
<div class="light-countdown">倒计时: ${info.countdown}秒</div>`;
lightsEl.appendChild(card);
});};
const send=(payload)=>{if(ws&&ws.readyState===WebSocket.OPEN){ws.send(JSON.stringify(payload));}};
const sendSubscribe=()=>{const ids=(inputEl.value||'').split(',').map((id)=>id.trim()).filter(Boolean);
currentIds=new Set(ids);lights.forEach((_,id)=>{if(!currentIds.has(id)){lights.delete(id);}});
render();send({action:'subscribe',lights:ids});};
const connect=()=>{setStatus(`连接中 ${wsUrl}`);ws=new WebSocket(wsUrl);
ws.onopen=()=>{setStatus('连接成功');send({action:'login',user_id:`web-${Date.now()}`});sendSubscribe();};
ws.onmessage=(evt)=>{try{const msg=JSON.parse(evt.data);
if(msg.type==='light_update'){if(currentIds.size>0&&!currentIds.has(msg.light_id)){return;}
lights.set(msg.light_id,{direction:msg.direction,color:msg.color,countdown:msg.countdown});render();}
else if(msg.type==='subscribe_ack'){setStatus(`已订阅 ${msg.count||0} 个红绿灯`);}
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
.lights {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 12px;
}
.light-card {
  border-radius: 8px;
  padding: 16px;
  background: #fff;
  box-shadow: 0 2px 6px rgba(0,0,0,0.08);
}
.light-card.red { border-left: 4px solid #ef5350; }
.light-card.yellow { border-left: 4px solid #ffc107; }
.light-card.green { border-left: 4px solid #66bb6a; }
.light-id { font-weight: bold; margin-bottom: 8px; }
.light-direction { font-size: 14px; color: #607d8b; margin-bottom: 4px; }
.light-color { font-size: 20px; margin-bottom: 4px; }
.light-color.red { color: #ef5350; }
.light-color.yellow { color: #ffc107; }
.light-color.green { color: #66bb6a; }
.light-countdown { font-size: 14px; color: #607d8b; }
.status { margin: 8px 0; color: #607d8b; }
)";

constexpr char kEmptyFavicon[] = "";

// SUBSCRIBE 管理：light_id -> 订阅该灯的连接（弱引用）
class SubscriptionManager {
 public:
  using ConnectionPtr = std::shared_ptr<TcpConnection>;

  // 更新某连接的订阅集合：new_ids 为完整新集合
  void UpdateSubscriptions(const ConnectionPtr& conn,
                           const std::vector<std::string>& new_ids) {
    std::lock_guard<std::mutex> lock(mu_);
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
    for (const auto& id : new_ids) {
      subs_[id].push_back(conn);
    }
  }

  // 连接关闭时清理其所有订阅
  void RemoveConnection(const ConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mu_);
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

  // 遍历某个灯ID的所有订阅连接（锁内复制，锁外发送，减少持锁时间）
  template <typename F>
  void ForEachSubscriber(const std::string& light_id, F&& f) {
    std::vector<ConnectionPtr> alive;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = subs_.find(light_id);
      if (it == subs_.end()) return;
      auto& vec = it->second;
      for (auto iter = vec.begin(); iter != vec.end();) {
        auto sp = iter->lock();
        if (!sp || !sp->IsConnected()) {
          iter = vec.erase(iter);
          continue;
        }
        alive.push_back(std::move(sp));
        ++iter;
      }
      if (vec.empty()) {
        subs_.erase(it);
      }
    }
    // 锁外执行发送，不阻塞其他线程
    for (auto& c : alive) {
      f(c);
    }
  }

  size_t LightCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return subs_.size();
  }

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::vector<std::weak_ptr<TcpConnection>>> subs_;
};

// 将 LightUpdate 转成发给 WebSocket 客户端的 JSON 文本
std::string BuildLightUpdateJson(const LightUpdate& update) {
  std::string payload = "{\"type\":\"light_update\",\"light_id\":\"";
  payload.append(update.light_id);
  payload.append("\",\"intersection_id\":\"");
  payload.append(update.intersection_id);
  payload.append("\",\"direction\":\"");
  payload.append(update.direction);
  payload.append("\",\"color\":");
  payload.append(std::to_string(static_cast<int>(update.color)));
  payload.append(",\"countdown\":");
  payload.append(std::to_string(update.countdown));
  payload.append(",\"timestamp\":");
  payload.append(std::to_string(update.timestamp));
  payload.append("}");
  return payload;
}

// 主服务器对象
class TrafficPushServerV2 {
 public:
  TrafficPushServerV2(EventLoop* base_loop, const InetAddress& listen_addr,
                      int io_threads, int http_port, int ws_port,
                      std::string web_root,
                      std::unique_ptr<DataSource> data_source)
      : base_loop_(base_loop),
        server_(base_loop, listen_addr, io_threads, /*reuse_port=*/true),
        http_port_(http_port),
        ws_port_(ws_port),
        web_root_(std::move(web_root)),
        data_source_(std::move(data_source)) {
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
    LOG_INFO << "TrafficPushServerV2 starting... http_port=" << http_port_
             << " ws_port=" << ws_port_;

    if (!data_source_) {
      LOG_ERROR << "No data source provided";
      return;
    }

    // 订阅数据源更新
    data_source_->Subscribe([this](const LightUpdate& update) {
      OnLightUpdate(update);
    });

    // 启动数据源
    data_source_->Start();

    if (http_server_) {
      http_server_->Start();
    }
    if (ws_server_) {
      ws_server_->Start();
    }
    server_.Start();
  }

  void Stop() {
    if (data_source_) {
      data_source_->Stop();
    }
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

    std::vector<std::string> light_ids;
    const char* p = body + sizeof(uint16_t);
    size_t remaining = len - sizeof(uint16_t);

    for (uint16_t i = 0; i < cnt; ++i) {
      if (remaining < sizeof(uint16_t)) break;
      uint16_t id_len_n = 0;
      std::memcpy(&id_len_n, p, sizeof(id_len_n));
      uint16_t id_len = ntohs(id_len_n);
      p += sizeof(uint16_t);
      remaining -= sizeof(uint16_t);

      if (remaining < id_len) break;
      light_ids.emplace_back(p, id_len);
      p += id_len;
      remaining -= id_len;
    }

    subs_.UpdateSubscriptions(conn, light_ids);
    Session* sess = conn->get_context<Session>();
    LOG_INFO << "SUBSCRIBE fd=" << conn->fd() << " user="
             << (sess ? sess->user_id : "") << " count=" << light_ids.size();
  }

  void HandlePing(const TcpServer::ConnectionPtr& conn) {
    Session* sess = conn->get_context<Session>();
    if (sess) {
      sess->last_heartbeat = std::chrono::steady_clock::now();
    }
    static const std::string kPongPacket = MakePacket(MsgType::kPong, nullptr, 0);
    conn->Send(kPongPacket);
  }

  void OnLightUpdate(const LightUpdate& u) {
    // 构造 JSON 推送给 WebSocket 客户端
    if (ws_server_) {
      std::string json = BuildLightUpdateJson(u);
      // 推送给订阅了这个灯的 WebSocket 客户端
      ws_server_->PublishTo(u.light_id, json);
    }

    // 推送给 TCP 客户端
    // 构造二进制包
    std::string light_id = u.light_id;
    uint16_t id_len = static_cast<uint16_t>(light_id.size());
    uint16_t id_len_n = htons(id_len);

    char body[sizeof(uint16_t) + 32 + 1 + sizeof(uint32_t)];  // 灯ID + 状态 + 倒计时
    size_t offset = 0;

    // 写入灯ID长度
    std::memcpy(body + offset, &id_len_n, sizeof(id_len_n));
    offset += sizeof(id_len_n);

    // 写入灯ID
    std::memcpy(body + offset, light_id.data(), id_len);
    offset += id_len;

    // 写入状态（1字节）
    body[offset] = static_cast<char>(u.color);
    offset += 1;

    // 写入倒计时（4字节，网络字节序）
    uint32_t countdown_n = htonl(static_cast<uint32_t>(u.countdown));
    std::memcpy(body + offset, &countdown_n, sizeof(countdown_n));
    offset += sizeof(countdown_n);

    std::string packet = MakePacket(MsgType::kLightUpdate, body, offset);

    // 推送给订阅了这个灯的 TCP 客户端
    subs_.ForEachSubscriber(u.light_id, [&packet](const std::shared_ptr<TcpConnection>& c) {
      c->Send(packet);
    });
  }

  EventLoop* base_loop_;
  TcpServer server_;
  SubscriptionManager subs_;
  int http_port_ = 0;
  int ws_port_ = 0;
  std::string web_root_;
  std::unique_ptr<HttpServer> http_server_;
  std::unique_ptr<WebSocketServer> ws_server_;
  std::unique_ptr<DataSource> data_source_;
};

struct ServerOptions {
  int port = 9000;
  int io_threads = 1;
  int http_port = 9200;
  int ws_port = 9100;
  std::string web_root = "netx/web";
};

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog
            << " [--port <port>] [--io-threads <n>]\n"
               "           [--http-port <port>] [--ws-port <port>]\n"
               "           [--web-root <dir>]\n"
               "\nDefault: port=9000, io-threads=1, http-port=9200, ws-port=9100\n";
}

bool ParseArgs(int argc, char* argv[], ServerOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&](int& dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = std::stoi(argv[++i]);
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

  LOG_INFO << "TrafficPushServerV2 config: port=" << opts.port
           << " io_threads=" << opts.io_threads
           << " http_port=" << opts.http_port
           << " ws_port=" << opts.ws_port
           << " web_root=" << opts.web_root;

  // 创建数据源（从 Redis 读取，内部用连接池）
  auto data_source = std::make_unique<RedisDataSource>();
  if (!data_source->InitDefault()) {
    LOG_ERROR << "Failed to initialize Redis data source";
    return 1;
  }

  EventLoop loop(/*single_thread_mode=*/opts.io_threads == 1);
  InetAddress addr(static_cast<uint16_t>(opts.port));
  TrafficPushServerV2 server(&loop, addr, opts.io_threads,
                             opts.http_port, opts.ws_port, opts.web_root,
                             std::move(data_source));
  server.Start();
  loop.Loop();
  return 0;
}
