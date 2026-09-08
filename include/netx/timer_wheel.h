#ifndef NETX_TIMER_WHEEL_H_
#define NETX_TIMER_WHEEL_H_

#include "netx/timer.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace netx {

class EventLoop;

// 时间轮定时器系统
// 时间复杂度：添加 O(1)，删除 O(1)，推进 O(1)
// 适用场景：大量短连接定时器（如 HTTP 服务器的连接超时）
class TimerWheel {
public:
    // 定时器 ID 类型
    using TimerId = uint64_t;

    // loop: 所属 EventLoop
    explicit TimerWheel(EventLoop* loop);

    // 析构函数
    ~TimerWheel() = default;

    // 添加定时器
    // delay_ms: 延迟时间（毫秒）
    // cb: 回调函数
    // repeat: 是否重复
    // 返回：定时器 ID
    TimerId AddTimer(uint64_t delay_ms, TimerCallback cb, bool repeat = false);

    // 添加重复定时器
    // interval_ms: 间隔时间（毫秒）
    // cb: 回调函数
    // 返回：定时器 ID
    TimerId AddRepeatTimer(uint64_t interval_ms, TimerCallback cb);

    // 删除定时器
    // timer_id: 定时器 ID
    void RemoveTimer(TimerId timer_id);

    // 推进时间轮，处理到期的定时器
    // now: 当前时间戳（毫秒）
    void Tick(uint64_t now);

    // 获取最近的超时时间（毫秒）
    // 返回：0 表示有定时器已到期，>0 表示最近超时时间，-1 表示无定时器
    int64_t GetNextTimeout() const;

    // 获取定时器数量
    size_t Size() const;

    // 是否为空
    bool Empty() const;

private:
    // 槽数量（60 个槽，每个槽代表 1 秒）
    static const int kSlotNum = 60;
    // 每个槽的精度（1000 毫秒）
    static const int kSlotMs = 1000;

    // 计算定时器应该放入哪个槽
    int CalculateSlot(uint64_t expire) const;

    // 执行所有到期的定时器
    void ProcessExpiredTimers(uint64_t now);

    // 所属 EventLoop
    EventLoop* loop_;
    // 时间轮槽：每个槽是一个定时器链表
    std::vector<std::list<TimerPtr>> slots_;
    // 当前槽索引
    int current_slot_;
    // 当前时间戳（毫秒）
    uint64_t current_time_;
    // 定时器映射表：ID -> 定时器，用于快速查找和删除
    std::unordered_map<TimerId, TimerPtr> timer_map_;
    // 下一个定时器 ID
    TimerId next_timer_id_;
    // 互斥锁（用于线程安全）
    mutable std::mutex mu_;
};

} // namespace netx

#endif // NETX_TIMER_WHEEL_H_
