#include "netx/timer.h"

namespace netx {

Timer::Timer(uint64_t id, uint64_t expire, TimerCallback cb,
             bool repeat, uint64_t interval)
    : id_(id),
      expire_(expire),
      callback_(std::move(cb)),
      repeat_(repeat),
      interval_(interval) {
}

void Timer::Run() const {
    if (callback_) {
        callback_();
    }
}

void Timer::Restart(uint64_t now) {
    if (repeat_) {
        // 设置下一次过期时间：当前时间 + 间隔
        expire_ = now + interval_;
    }
}

} // namespace netx
