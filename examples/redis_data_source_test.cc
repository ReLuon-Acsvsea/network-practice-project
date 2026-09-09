// Redis 数据源测试
//
// 测试功能：
// 1. Redis 连接池
// 2. 写入红绿灯配置到 Redis
// 3. 从 Redis 读取状态
// 4. 订阅更新
//
// 前提：Redis 服务运行在 127.0.0.1:6379

#include <chrono>
#include <iostream>
#include <thread>

#include "netx/logging.h"
#include "netx/redis_data_source.h"

using netx::Intersection;
using netx::LightColor;
using netx::LightType;
using netx::RedisDataSource;
using netx::TrafficLight;

// 打印红绿灯状态
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
    LOG_INFO << "Redis 数据源测试开始";

    // 创建 Redis 数据源
    RedisDataSource data_source;

    // 初始化
    if (!data_source.InitDefault()) {
        LOG_ERROR << "初始化失败，请确保 Redis 运行在 127.0.0.1:6379";
        return 1;
    }

    // 写入测试数据到 Redis
    std::cout << "\n===== 写入测试数据到 Redis =====" << std::endl;

    // 先用 RedisConnection 直接测试
    {
        netx::RedisConnection test_conn;
        netx::RedisConfig test_config;
        if (test_conn.Connect(test_config)) {
            std::cout << "直接连接测试: OK" << std::endl;
            std::string result;
            if (test_conn.Command(&result, "SET test:key hello")) {
                std::cout << "SET 测试: " << result << std::endl;
            }
            if (test_conn.Get("test:key", &result)) {
                std::cout << "GET 测试: " << result << std::endl;
            }
        } else {
            std::cout << "直接连接测试: FAILED" << std::endl;
        }
    }

    Intersection intersection;
    intersection.id = "INT-001";
    intersection.name = "人民路-中山路路口";
    intersection.latitude = 31.2304;
    intersection.longitude = 121.4737;
    intersection.lights = {
        {"LIGHT-001", "INT-001", "东", LightType::kVehicle,
         LightColor::kRed, 27, 0},
        {"LIGHT-002", "INT-001", "南", LightType::kVehicle,
         LightColor::kGreen, 30, 0},
        {"LIGHT-003", "INT-001", "西", LightType::kVehicle,
         LightColor::kRed, 27, 0},
        {"LIGHT-004", "INT-001", "北", LightType::kVehicle,
         LightColor::kGreen, 30, 0}};

    if (data_source.WriteConfig(intersection)) {
        std::cout << "写入成功" << std::endl;
    } else {
        std::cout << "写入失败" << std::endl;
    }

    // 启动数据源
    data_source.Start();
    LOG_INFO << "数据源已启动";

    // 等待第一次更新
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // 打印状态
    std::cout << "\n===== 从 Redis 读取的状态 =====" << std::endl;
    for (const auto& intersection : data_source.GetIntersections()) {
        std::cout << "\n路口: " << intersection.name << std::endl;
        for (const auto& light : intersection.lights) {
            PrintLight(light);
        }
    }

    // 停止数据源
    data_source.Stop();
    LOG_INFO << "Redis 数据源测试完成";

    return 0;
}
