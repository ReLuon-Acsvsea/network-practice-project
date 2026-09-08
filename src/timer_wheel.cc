#include "netx/timer_wheel.h"
#include "netx/logging.h"

#include <algorithm>
#include <chrono>

namespace netx {

namespace {
// 获取当前时间戳（毫秒）
uint64_t NowMs() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
}
} // namespace

TimerWheel::TimerWheel(EventLoop* loop)
    : loop_(loop),
      slots_(kSlotNum),
      current_slot_(0),
      current_time_(NowMs()),
      next_timer_id_(1) {
}

TimerWheel::TimerId TimerWheel::AddTimer(uint64_t delay_ms,
                                          TimerCallback cb,
                                          bool repeat) {
    if (delay_ms == 0) {
        delay_ms = 1;  // 最小延迟 1 毫秒
    }

    std::lock_guard<std::mutex> lock(mu_);

    // 生成定时器 ID
    TimerId timer_id = next_timer_id_++;

    // 计算过期时间
    uint64_t expire = current_time_ + delay_ms;

    // 创建定时器
    TimerPtr timer = std::make_shared<Timer>(timer_id, expire, std::move(cb),
                                              repeat, repeat ? delay_ms : 0);

    // 计算槽位
    int slot = CalculateSlot(expire);

    // 添加到时间轮
    slots_[slot].push_back(timer);

    // 添加到映射表
    timer_map_[timer_id] = timer;

    LOG_DEBUG << "AddTimer id=" << timer_id
              << " delay=" << delay_ms
              << " slot=" << slot
              << " expire=" << expire;

    return timer_id;
}

TimerWheel::TimerId TimerWheel::AddRepeatTimer(uint64_t interval_ms,
                                                TimerCallback cb) {
    return AddTimer(interval_ms, std::move(cb), true);
}

void TimerWheel::RemoveTimer(TimerId timer_id) {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = timer_map_.find(timer_id);
    if (it != timer_map_.end()) {
        TimerPtr timer = it->second;

        // 从时间轮槽中移除
        int slot = CalculateSlot(timer->expire());
        slots_[slot].remove(timer);

        // 从映射表中移除
        timer_map_.erase(it);
        LOG_DEBUG << "RemoveTimer id=" << timer_id;
    }
}

void TimerWheel::Tick(uint64_t now) {
    std::lock_guard<std::mutex> lock(mu_);

    // 计算需要推进的槽数
    uint64_t elapsed = now - current_time_;
    int slots_to_advance = static_cast<int>(elapsed / kSlotMs);

    // 推进时间轮
    for (int i = 0; i < slots_to_advance; ++i) {
        current_slot_ = (current_slot_ + 1) % kSlotNum;
        current_time_ += kSlotMs;

        // 处理当前槽中的定时器
        ProcessExpiredTimers(now);
    }

    // 更新当前时间
    current_time_ = now;

    // 处理当前槽中已到期的定时器（处理同一槽内的精确时间）
    ProcessExpiredTimers(now);
}

int64_t TimerWheel::GetNextTimeout() const {
    std::lock_guard<std::mutex> lock(mu_);

    if (timer_map_.empty()) {
        return -1;  // 无定时器
    }

    // 查找最近的过期时间
    uint64_t min_expire = UINT64_MAX;
    for (const auto& pair : timer_map_) {
        if (pair.second->expire() < min_expire) {
            min_expire = pair.second->expire();
        }
    }

    // 计算超时时间
    if (min_expire <= current_time_) {
        return 0;  // 已到期
    }

    // 返回精确的超时时间（毫秒），最小 1ms 避免忙等
    uint64_t timeout = min_expire - current_time_;
    return static_cast<int64_t>(std::max(uint64_t(1), timeout));
}


size_t TimerWheel::Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return timer_map_.size();
}

bool TimerWheel::Empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return timer_map_.empty();
}

int TimerWheel::CalculateSlot(uint64_t expire) const {
    // 计算相对于当前时间的槽位偏移
    uint64_t delta = expire - current_time_;
    int offset = static_cast<int>((delta / kSlotMs) % kSlotNum);
    return (current_slot_ + offset) % kSlotNum;
}

void TimerWheel::ProcessExpiredTimers(uint64_t now) {
    // 遍历所有槽，检查是否有到期的定时器
    for (int i = 0; i < kSlotNum; ++i) {
        int slot = (current_slot_ + i) % kSlotNum;
        std::list<TimerPtr>& timers = slots_[slot];

        // 遍历定时器，执行到期的
        auto it = timers.begin();
        while (it != timers.end()) {
            TimerPtr timer = *it;

            // 检查是否到期
            if (timer->expire() <= now) {
                // 执行回调
                timer->Run();

                // 从当前槽中移除
                it = timers.erase(it);

                // 如果是重复定时器，重新添加到新槽位
                if (timer->is_repeat()) {
                    timer->Restart(now);
                    int new_slot = CalculateSlot(timer->expire());
                    slots_[new_slot].push_back(timer);
                } else {
                    // 从映射表中移除
                    timer_map_.erase(timer->id());
                }
            } else {
                ++it;
            }
        }
    }
}

} // namespace netx
