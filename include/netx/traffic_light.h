#ifndef NETX_TRAFFIC_LIGHT_H_
#define NETX_TRAFFIC_LIGHT_H_

#include <cstdint>

namespace netx {

// 表示一条红绿灯状态更新
struct LightUpdate {
  uint32_t light_id = 0;
  uint8_t state = 0;      // 0=红,1=黄,2=绿
  uint32_t remain_ms = 0; // 当前状态预计剩余毫秒数
};

}  // namespace netx

#endif  // NETX_TRAFFIC_LIGHT_H_