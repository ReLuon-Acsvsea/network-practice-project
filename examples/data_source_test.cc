// 数据源测试
//
// 测试功能：
// 1. 模拟数据源的初始化
// 2. 红绿灯状态更新
// 3. 多路口支持
// 4. 方向相位偏移（东西绿灯时，南北红灯）

#include <chrono>
#include <iostream>
#include <thread>

#include "netx/logging.h"
#include "netx/simulator_data_source.h"

using netx::Intersection;
using netx::LightColor;
using netx::SimulatorDataSource;
using netx::TrafficLight;

// 打印红绿灯状态
void PrintLightStatus(const TrafficLight& light) {
    const char* color_str = "unknown";
    switch (light.color) {
        case LightColor::kRed:
            color_str = "红";
            break;
        case LightColor::kYellow:
            color_str = "黄";
            break;
        case LightColor::kGreen:
            color_str = "绿";
            break;
    }
    std::cout << "  [" << light.id << "] "
              << light.direction << "方向: "
              << color_str << "灯, "
              << "倒计时 " << light.countdown << " 秒"
              << std::endl;
}

// 打印路口信息
void PrintIntersection(const Intersection& intersection) {
    std::cout << "\n路口: " << intersection.name
              << " (ID: " << intersection.id << ")"
              << std::endl;
    std::cout << "红绿灯:" << std::endl;

    for (const auto& light : intersection.lights) {
        PrintLightStatus(light);
    }
}

int main() {
    LOG_INFO << "数据源测试开始";

    // 创建模拟数据源
    SimulatorDataSource data_source;

    // 初始化默认配置
    if (!data_source.InitDefault()) {
        LOG_ERROR << "初始化失败";
        return 1;
    }

    // 启动数据源
    data_source.Start();
    LOG_INFO << "数据源已启动";

    // 等待第一次更新完成（更新间隔1秒）
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // 打印状态 A
    std::cout << "\n===== 状态 A =====" << std::endl;
    for (const auto& intersection : data_source.GetIntersections()) {
        PrintIntersection(intersection);
    }

    // 再运行 5 秒
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // 打印状态 B
    std::cout << "\n===== 状态 B =====" << std::endl;
    for (const auto& intersection : data_source.GetIntersections()) {
        PrintIntersection(intersection);
    }

    // 停止数据源
    data_source.Stop();
    LOG_INFO << "数据源测试完成";

    return 0;
}
