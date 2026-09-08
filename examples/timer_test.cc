#include "netx/event_loop.h"
#include "netx/logging.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace netx;

int main() {
    LOG_INFO << "Timer test starting...";

    EventLoop loop;

    // 测试 1：延迟定时器（使用秒级延迟，时间轮精度为 1 秒）
    LOG_INFO << "Test 1: RunAfter - delayed timer (1 second)";
    auto start = std::chrono::steady_clock::now();

    loop.RunAfter(1000, [&start]() {
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start).count();
        LOG_INFO << "Timer 1 fired! Elapsed: " << elapsed << "ms (expected ~1000ms)";
    });

    // 测试 2：另一个延迟定时器
    loop.RunAfter(2000, [&start]() {
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start).count();
        LOG_INFO << "Timer 2 fired! Elapsed: " << elapsed << "ms (expected ~2000ms)";
    });

    // 测试 3：重复定时器（每 1 秒执行一次）
    int repeat_count = 0;
    loop.RunEvery(1000, [&repeat_count, &start]() {
        repeat_count++;
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start).count();
        LOG_INFO << "Repeat timer fired! Count: " << repeat_count
                 << ", Elapsed: " << elapsed << "ms";

        // 3 次后停止
        if (repeat_count >= 3) {
            LOG_INFO << "Repeat timer completed!";
        }
    });

    // 测试 4：取消定时器
    auto timer_id = loop.RunAfter(5000, []() {
        LOG_INFO << "This should NOT be printed!";
    });
    loop.CancelTimer(timer_id);
    LOG_INFO << "Timer 4 cancelled (should not fire)";

    // 运行事件循环 5 秒
    LOG_INFO << "Running event loop for 5 seconds...";
    std::thread loop_thread([&loop]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        loop.Quit();
    });

    loop.Loop();
    loop_thread.join();

    LOG_INFO << "Timer test completed!";
    return 0;
}
