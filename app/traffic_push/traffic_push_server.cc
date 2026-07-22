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
#include "netx/websocket_server.h"

using netx::Buffer;
using netx::EventLoop;
using netx::InetAddress;
using netx::HttpServer;
using netx::TcpConnection;
using netx::TcpServer;
using netx::LightUpdate;
using netx::WebSocketServer;

namespace {
// 协议消息类型
enum class MsgType : uint16_t {
  kLogin = 1,
  kSubscribe = 2,
  kPing = 3,
  kPong = 4,
  kLightUpdate = 100,
};

// 会话状态：挂在 TcpConnection::context() 中
struct Session {
  std::string user_id;
  std::vector<uint32_t> subscribed_light_ids;
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
//读整个文件内容到 string 里，文件不存在就返回空
std::string ReadFileIfExists(const std::filesystem::path &path){
    std::ifstream fin(path,std::ios::binary);
    if(!fin.is_open()) return {};
    std::ostringstream oss;
    oss<< fin.rdbuf();
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
  <title>高并发红绿灯推送-程序员老廖</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <link rel="stylesheet" href="/style.css" />
</head>
<body>
  <div class="container">
    <h1>高并发消息推送架构(以红绿灯信息推送为例)</h1>
    <h2>基于 epoll 的实时红绿灯推送</h2>
    <p class="status" id="status">正在连接...</p>
    <div class="controls">
      <label>订阅红绿灯 ID（用逗号分隔）</label>
      <div class="control-row">
        <input id="light-input" value="1,2,3" />
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
const setStatus=(text)=>{statusEl.textContent=text;};
const render=()=>{if(lights.size===0){lightsEl.innerHTML='';lightsEl.style.display='none';return;}
lightsEl.style.display='grid';lightsEl.innerHTML='';
Array.from(lights.entries()).sort((a,b)=>a[0]-b[0]).forEach(([id,info])=>{
const sec=Math.max(0,Math.round(info.remain_ms/1000));
const card=document.createElement('div');
card.className=`light-card ${info.state}`;
card.innerHTML=`<div class="light-id">#${id}</div>
<div class="light-state">${info.state.toUpperCase()}</div>
<div class="light-remain">${sec} s</div>`;
lightsEl.appendChild(card);
});};
const send=(payload)=>{if(ws&&ws.readyState===WebSocket.OPEN){ws.send(JSON.stringify(payload));}};
const sendSubscribe=()=>{const ids=(inputEl.value||'').split(',').map((id)=>id.trim()).filter(Boolean).map((id)=>parseInt(id,10)).filter((num)=>!Number.isNaN(num));
currentIds=new Set(ids);lights.forEach((_,id)=>{if(!currentIds.has(id)){lights.delete(id);}});render();send({action:'subscribe',lights:ids});};
const connect=()=>{setStatus(`连接中 ${wsUrl}`);ws=new WebSocket(wsUrl);
ws.onopen=()=>{setStatus('连接成功');send({action:'login',user_id:`web-${Date.now()}`});sendSubscribe();};
ws.onmessage=(evt)=>{try{const msg=JSON.parse(evt.data);
if(msg.type==='light_update'){if(currentIds.size>0&&!currentIds.has(msg.light_id)){return;}
lights.set(msg.light_id,{state:msg.state,remain_ms:msg.remain_ms});render();}
else if(msg.type==='subscribe_ack'){setStatus(`已订阅 ${msg.count||0} 个路口`);}
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
.light-card.yellow { border-left: 4px solid #fdd835; }
.light-card.green { border-left: 4px solid #66bb6a; }
.light-id { font-weight: bold; margin-bottom: 8px; }
.light-state { font-size: 20px; margin-bottom: 4px; }
.status { margin: 8px 0; color: #607d8b; }
)";
constexpr char kEmptyFavicon[] = "";

class SubscriptionManager{
 public:
  using ConnectionPtr = std::shared_ptr<TcpConnection>;
  void UpdateSubscriptions(const ConnectionPtr & conn,
                           const std::vector<uint32_t>& new_ids){
    Session *sess = conn->get_context<Session>();
    if(!sess) return;
    for(uint32_t id:sess->subscribed_light_ids){
      auto it =subs_.find(id);
      if(it==subs_.end()) continue;
      auto & vec = it->second;
      vec.erase(std::remove_if(vec.begin(),vec.end(),
                                [&](const std::weak_ptr<TcpConnection> & wp){
                                  auto sp= wp.lock();
                                  return !sp || sp.get()==conn.get();
                                }),
                    vec.end());
      if(vec.empty()){
        subs_.erase(it);
      }
    }
    sess->subscribed_light_ids = new_ids;
    for(uint32_t id : sess->subscribed_light_ids){
      subs_[id].push_back(conn);
    }
  }
  void RemoveConnection(const ConnectionPtr &conn){
    UpdateSubscriptions(conn, {});
  }
  template <typename F>
  void ForEachSubscriber(uint32_t ligit_id, F&&f){
    auto it = subs_.find(ligit_id);
    if(it==subs_.end()) return ;
    auto & vec = it->second;
    for(auto iter=vec.begin();iter!=vec.end();){
      auto sp =iter->lock();
      if(!sp || !sp->IsConnected()){
        iter =vec.erase(iter);
        continue;
      }
      f(sp);
      ++iter;//避免erase带来的迭代器问题，放在后面
    }
    if(vec.empty()){
      subs_.erase(it);
    }
  }

  size_t LightCount() const {return subs_.size();}
 private:
  std::unordered_map<uint32_t,std::vector<std::weak_ptr<TcpConnection>>> subs_;
};

// 将 LightUpdate 转成发给 WebSocket 客户端的 JSON 文本
std::string BuildLightUpdateJson(const LightUpdate& update) {
  const char *state = "red";
  if(update.state == 1){
    state = "green";
  }else if(update.state==2){
    state ="yellow";
  }
  std::string payload = "{\"type\":\"light_update\",\"light_id\":";
  payload.append(std::to_string(update.light_id));
  payload.append(",\"state\":\"");
  payload.append(state);
  payload.append("\",\"remain_ms\":");
  payload.append(std::to_string(update.remain_ms));
  payload.append("}");
  return payload;
}
//红绿灯状态模拟器：独立线程生成状态变化通过 EventLoop::QueueInLoop 投递
class LightSimulator {
  public:
    using UpdateCallback = std::function<void(const LightUpdate&)>;
  private:

    struct LightState {
      uint8_t state =0; //0=红 1=绿 2=黄
      uint32_t remain_ms =0;
    };
    void Run(){
      if(lights_.empty()) return ;
      using Clock = std::chrono::steady_clock;
      const auto interval = std::chrono::milliseconds(tick_ms_);
      while(running_.load()){
        auto start =Clock::now();
        std::vector<LightUpdate> updates;
        GenerateBatchUpdates(updates);
        if(!updates.empty()){
          loop_->QueueInLoop([this,ups=std::move(updates)]() mutable {
            for(const auto& u:ups){
              cb_(u);
            }
          });
        }
        auto next = start +interval;
        std::this_thread::sleep_until(next);
        
      }
    }
    void GenerateBatchUpdates(std::vector<LightUpdate>& out){
      const uint32_t n =static_cast<uint32_t>(lights_.size());
      if(n==0) return;
      out.clear();
      const uint32_t count = std::min(updates_per_tick_,n);
      out.reserve(count);
      for( uint32_t i=0;i<count;++i){
        uint32_t idx = next_light_;
        next_light_ =(next_light_+1)%n;
        auto& st=lights_[idx];
        if(st.remain_ms <= per_light_interval_ms_){
          st.state = static_cast<uint8_t>((st.state+1)%3);
          st.remain_ms = RandomDurationForState(st.state);
        }else{
          st.remain_ms -=per_light_interval_ms_;
        }
        LightUpdate u;
        u.light_id = idx+1;
        u.state = st.state;
        u.remain_ms = st.remain_ms;
        out.push_back(u);
      }
    }
    EventLoop *loop_;
    std::vector<LightState>lights_;
    int tick_ms_ =100;
    uint32_t updates_per_tick_ =100;//每批更新数量
    UpdateCallback cb_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    uint32_t next_light_ =0;
    uint32_t per_light_interval_ms_=0;
    std::mt19937 rng_{std::random_device{}()};
    void InitializeLights(){
      for(auto & st : lights_){
        st.state=static_cast<uint8_t>(rng_() %3);
        st.remain_ms = RandomDurationForState(st.state);
      }
    }
    uint32_t RandomDurationForState(uint8_t state){
      switch(state){
        case 0:
          return RandomRange(25000,45000);
        case 1:
          return RandomRange(15000,30000);
        case 2:
        default:
          return RandomRange(3000,5000);
      }
    }
    uint32_t RandomRange(uint32_t min, uint32_t max){
      std::uniform_int_distribution<uint32_t> dist(min,max);
      return dist(rng_);
    }

};


}