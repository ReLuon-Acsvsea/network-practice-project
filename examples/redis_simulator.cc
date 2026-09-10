// Redis 模拟器
//
// 功能：生成红绿灯数据，通过 RedisWriter 写入 Redis
// 运行后，服务器可以用 RedisDataSource 从 Redis 读取
//
// 用法：./redis_simulator

#include <chrono>
#include <iostream>
#include <thread>

#include "netx/logging.h"
#include "netx/redis_writer.h"
#include "netx/simulator_data_source.h"

using netx::Intersection;
using netx::LightColor;
using netx::RedisConfig;
using netx::RedisWriter;
using netx::SimulatorDataSource;

int main() {
    LOG_INFO << "Redis 模拟器启动";

    // 创建模拟器
    SimulatorDataSource simulator;
    if (!simulator.InitDefault()) {
        LOG_ERROR << "模拟器初始化失败";
        return 1;
    }

    // 创建 RedisWriter（pipeline 批量写入）
    RedisWriter writer;
    RedisConfig config;
    if (!writer.Init(config)) {
        LOG_ERROR << "RedisWriter 初始化失败";
        return 1;
    }

    // 订阅模拟器更新
    writer.SubscribeTo(&simulator);

    // 启动模拟器
    simulator.Start();

    // 打印状态
    std::cout << "开始写入数据到 Redis（按 Ctrl+C 停止）..." << std::endl;
    std::cout << "使用 pipeline 批量写入，50条一批或1秒刷新一次" << std::endl;

    while (true) {
        // 每5秒打印一次当前状态
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
