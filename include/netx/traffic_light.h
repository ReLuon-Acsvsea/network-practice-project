// 红绿灯数据结构定义
//
// 功能：
// - 定义红绿灯的基本信息、状态、变化规律
// - 支持多路口、多方向、多类型红绿灯
// - 支持配置驱动的时序方案

#ifndef NETX_TRAFFIC_LIGHT_H_
#define NETX_TRAFFIC_LIGHT_H_

#include <cstdint>
#include <string>
#include <vector>

namespace netx {

// 红绿灯颜色
enum class LightColor : uint8_t {
    kRed = 0,
    kYellow = 1,
    kGreen = 2
};

// 颜色转字符串
inline const char* LightColorToString(LightColor color) {
    switch (color) {
        case LightColor::kRed: return "red";
        case LightColor::kYellow: return "yellow";
        case LightColor::kGreen: return "green";
        default: return "unknown";
    }
}

// 字符串转颜色
inline LightColor StringToLightColor(const std::string& str) {
    if (str == "red") return LightColor::kRed;
    if (str == "yellow") return LightColor::kYellow;
    if (str == "green") return LightColor::kGreen;
    return LightColor::kRed;  // 默认红色
}

// 红绿灯类型
enum class LightType : uint8_t {
    kVehicle = 0,    // 机动车灯
    kPedestrian = 1, // 行人灯
    kBicycle = 2     // 自行车灯
};

// 红绿灯类型转字符串
inline const char* LightTypeToString(LightType type) {
    switch (type) {
        case LightType::kVehicle: return "vehicle";
        case LightType::kPedestrian: return "pedestrian";
        case LightType::kBicycle: return "bicycle";
        default: return "unknown";
    }
}

// 红绿灯基本信息
struct TrafficLight {
    std::string id;              // 灯的唯一标识（如 "LIGHT-001"）
    std::string intersection_id; // 所属路口ID
    std::string direction;       // 方向（东、南、西、北）
    LightType type;              // 类型（机动车、行人、自行车）

    // 当前状态
    LightColor color = LightColor::kRed;  // 当前颜色
    int countdown = 0;                     // 倒计时秒数
    uint64_t timestamp = 0;                // 状态更新时间戳（毫秒）
};

// 红绿灯阶段（一个颜色持续的时间）
struct LightPhase {
    LightColor color;            // 颜色
    int duration;                // 持续时间（秒）
};

// 红绿灯时序方案
struct LightSchedule {
    std::string id;              // 时序方案ID
    std::vector<LightPhase> phases;  // 阶段序列

    // 计算总周期时长
    int CycleDuration() const {
        int total = 0;
        for (const auto& phase : phases) {
            total += phase.duration;
        }
        return total;
    }
};

// 路口配置
struct Intersection {
    std::string id;              // 路口ID（如 "INT-001"）
    std::string name;            // 路口名称（如 "人民路-中山路路口"）
    double latitude = 0.0;       // 纬度
    double longitude = 0.0;      // 经度

    // 各方向的红绿灯
    std::vector<TrafficLight> lights;

    // 时序方案
    LightSchedule schedule;

    // 协调关系（多个路口之间的相位差）
    std::string coordination_group;  // 协调组
    int phase_offset = 0;            // 相位偏移（秒）
};

// 红绿灯状态更新 V1（兼容旧版本）
struct LightUpdateV1 {
    uint32_t light_id = 0;
    uint8_t state = 0;      // 0=红,1=黄,2=绿
    uint32_t remain_ms = 0; // 当前状态预计剩余毫秒数
};

// 红绿灯状态更新 V2（新版本）
struct LightUpdate {
    std::string light_id;        // 灯的ID
    std::string intersection_id; // 路口ID
    std::string direction;       // 方向
    LightColor color;            // 颜色
    int countdown;               // 倒计时
    uint64_t timestamp;          // 时间戳
};

// 兼容旧版本的别名
using LightUpdateCompat = LightUpdateV1;

} // namespace netx

#endif // NETX_TRAFFIC_LIGHT_H_


