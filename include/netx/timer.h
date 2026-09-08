#ifndef NETX_TIMER_H_
#define NETX_TIMER_H_

#include <cstdint>
#include <functional>
#include <memory>

namespace netx {

// 定时器回调类型
using TimerCallback = std::function<void()>;

// 定时器类：封装单个定时器的状态和行为
class Timer {
public:
    // id: 定时器唯一标识
    // expire: 过期时间戳（毫秒）
    // cb: 回调函数
    // repeat: 是否重复
    // interval: 重复间隔（毫秒），仅当 repeat=true 时有效
    Timer(uint64_t id, uint64_t expire, TimerCallback cb,
          bool repeat = false, uint64_t interval = 0);

    // 析构函数
    ~Timer() = default;

    // 执行回调函数
    void Run() const;

    // 获取定时器 ID
    uint64_t id() const { return id_; }

    // 获取过期时间戳（毫秒）
    uint64_t expire() const { return expire_; }

    // 设置新的过期时间
    void set_expire(uint64_t expire) { expire_ = expire; }

    // 是否为重复定时器
    bool is_repeat() const { return repeat_; }

    // 获取重复间隔（毫秒）
    uint64_t interval() const { return interval_; }

    // 重置定时器（用于重复定时器）
    void Restart(uint64_t now);

private:
    // 定时器唯一标识
    uint64_t id_;
    // 过期时间戳（毫秒）
    uint64_t expire_;
    // 回调函数
    TimerCallback callback_;
    // 是否重复执行
    bool repeat_;
    // 重复间隔（毫秒）
    uint64_t interval_;
};

// 定时器智能指针类型
using TimerPtr = std::shared_ptr<Timer>;

} // namespace netx

#endif // NETX_TIMER_H_
