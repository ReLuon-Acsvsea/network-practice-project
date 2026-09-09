// Redis 数据管道测试
//
// 测试流程：
// SimulatorDataSource → RedisWriter → Redis → RedisDataSource
//
// 验证：
// 1. 模拟器生成数据
// 2. RedisWriter 写入 Redis
// 3. RedisDataSource 从 Redis 读取
// 4. 数据一致性

#include <chrono>
#include <iostream>
#include <thread>

#include "netx/logging.h"
#include "netx/redis_data_source.h"
#include "netx/redis_writer.h"
#include "netx/simulator_data_source.h"

using netx::LightColor;
using netx::RedisDataSource;
using netx::RedisWriter;
using netx::SimulatorDataSource;
using netx::TrafficLight;

// 打印红绿灯
void PrintLight(const TrafficLight& light) {
    const char* color = "unknown";
    switch (light.color) {
        case LightColor::kRed: color = "红"; break;
        case LightColor::kYellow: color = "黄"; break;
        case LightColor::kGreen: color = "绿"; break;
    }
    std::cout << "  [" << light.id << "] "
              << light.direction << "方向: "
              << color << "灯, "
              << "倒计时 " << light.countdown << " 秒" << std::endl;
}

int main() {
    LOG_INFO << "Redis 数据管道测试开始";

    // 1. 创建模拟器（数据源）
    SimulatorDataSource simulator;
    if (!simulator.InitDefault()) {
        LOG_ERROR << "模拟器初始化失败";
        return 1;
    }

    // 2. 创建 RedisWriter（写入 Redis）
    RedisWriter writer;
    netx::RedisConfig redis_config;
    if (!writer.Init(redis_config)) {
        LOG_ERROR << "RedisWriter 初始化失败";
        return 1;
    }

    // 3. 订阅模拟器更新
    writer.SubscribeTo(&simulator);

    // 4. 启动模拟器
    simulator.Start();
    LOG_INFO << "模拟器已启动";

    // 等待几秒，让模拟器生成数据并写入 Redis
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 5. 创建 RedisDataSource（从 Redis 读取）
    RedisDataSource redis_source;
    if (!redis_source.Init(redis_config)) {
        LOG_ERROR << "RedisDataSource 初始化失败";
        return 1;
    }

    // 启动 RedisDataSource
    redis_source.Start();
    LOG_INFO << "RedisDataSource 已启动";

    // 等待第一次更新
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // 6. 打印两边的数据
    std::cout << "\n===== 模拟器数据 =====" << std::endl;
    for (const auto& intersection : simulator.GetIntersections()) {
        std::cout << "路口: " << intersection.name << std::endl;
        for (const auto& light : intersection.lights) {
            PrintLight(light);
        }
    }

    std::cout << "\n===== Redis 数据 =====" << std::endl;
    for (const auto& intersection : redis_source.GetIntersections()) {
        std::cout << "路口: " << intersection.name << std::endl;
        for (const auto& light : intersection.lights) {
            PrintLight(light);
        }
    }

    // 7. 再等几秒，验证数据同步
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "\n===== 2秒后 =====" << std::endl;
    auto sim_light = simulator.GetLightStatus("LIGHT-001");
    auto redis_light = redis_source.GetLightStatus("LIGHT-001");
    std::cout << "模拟器 LIGHT-001: 倒计时 " << sim_light.countdown << " 秒"
              << std::endl;
    std::cout << "Redis   LIGHT-001: 倒计时 " << redis_light.countdown << " 秒"
              << std::endl;

    // 停止
    redis_source.Stop();
    simulator.Stop();
    LOG_INFO << "Redis 数据管道测试完成";

    return 0;
}
