// Redis 模拟器
//
// 功能：生成红绿灯数据，定时写入 Redis
// 运行后，服务器可以用 RedisDataSource 从 Redis 读取
//
// 用法：./redis_simulator

#include <chrono>
#include <iostream>
#include <thread>

#include "netx/logging.h"
#include "netx/redis_connection.h"
#include "netx/simulator_data_source.h"

using netx::Intersection;
using netx::LightColor;
using netx::RedisConfig;
using netx::RedisConnection;
using netx::SimulatorDataSource;

// 写入单个灯到 Redis
void WriteLightToRedis(RedisConnection* conn, const std::string& light_id,
                       const std::string& intersection_id,
                       const std::string& direction,
                       LightColor color, int countdown) {
    std::string key = "traffic:light:" + light_id;
    conn->HSet(key, "light_id", light_id);
    conn->HSet(key, "intersection_id", intersection_id);
    conn->HSet(key, "direction", direction);
    conn->HSet(key, "color", std::to_string(static_cast<int>(color)));
    conn->HSet(key, "countdown", std::to_string(countdown));
}

// 写入路口配置到 Redis
void WriteIntersectionToRedis(RedisConnection* conn,
                              const Intersection& intersection) {
    std::string key = "traffic:intersection:" + intersection.id;
    conn->HSet(key, "id", intersection.id);
    conn->HSet(key, "name", intersection.name);
}

int main() {
    LOG_INFO << "Redis 模拟器启动";

    // 连接 Redis
    RedisConnection conn;
    RedisConfig config;
    if (!conn.Connect(config)) {
        LOG_ERROR << "连接 Redis 失败";
        return 1;
    }

    // 创建模拟器
    SimulatorDataSource simulator;
    if (!simulator.InitDefault()) {
        LOG_ERROR << "模拟器初始化失败";
        return 1;
    }

    // 写入路口配置
    for (const auto& intersection : simulator.GetIntersections()) {
        WriteIntersectionToRedis(&conn, intersection);
    }
    LOG_INFO << "路口配置已写入 Redis";

    // 启动模拟器
    simulator.Start();

    // 定时写入 Redis
    std::cout << "开始写入数据到 Redis（按 Ctrl+C 停止）..." << std::endl;
    while (true) {
        for (const auto& intersection : simulator.GetIntersections()) {
            for (const auto& light : intersection.lights) {
                WriteLightToRedis(&conn, light.id, light.intersection_id,
                                  light.direction, light.color,
                                  light.countdown);
            }
        }

        // 打印当前状态（每5秒打印一次）
        static int tick = 0;
        if (++tick % 5 == 0) {
            std::cout << "\n--- 第 " << tick << " 秒 ---" << std::endl;
            for (const auto& intersection : simulator.GetIntersections()) {
                for (const auto& light : intersection.lights) {
                    const char* color = "unknown";
                    switch (light.color) {
                        case LightColor::kRed: color = "红"; break;
                        case LightColor::kYellow: color = "黄"; break;
                        case LightColor::kGreen: color = "绿"; break;
                    }
                    std::cout << light.id << " " << light.direction
                              << "方向: " << color
                              << " " << light.countdown << "秒" << std::endl;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
