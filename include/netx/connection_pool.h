// 连接池模板类
//
// 功能：
// - 管理可复用的连接对象（数据库、Redis、HTTP 等）
// - 线程安全的获取/归还机制
// - 自动创建和销毁连接
// - 支持最大连接数限制
// - 支持空闲连接超时回收
//
// 使用示例：
//   // 1. 定义连接工厂
//   auto factory = []() {
//       auto conn = std::make_shared<MySQLConnection>();
//       conn->Connect("127.0.0.1", 3306, "user", "pass", "db");
//       return conn;
//   };
//
//   // 2. 创建连接池
//   ConnectionPool<MySQLConnection> pool;
//   pool.Init(factory, 10);  // 最大 10 个连接
//
//   // 3. 使用连接
//   auto conn = pool.Get();
//   conn->Query("SELECT * FROM users");
//   pool.Put(conn);  // 归还连接

#ifndef NETX_CONNECTION_POOL_H_
#define NETX_CONNECTION_POOL_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace netx {

// 连接池配置
struct PoolConfig {
    int max_size = 10;                          // 最大连接数
    int min_size = 2;                           // 最小空闲连接数
    int max_idle_ms = 60000;                    // 空闲连接超时时间（毫秒）
    int get_timeout_ms = 5000;                  // 获取连接超时时间（毫秒）
};

// 连接接口：使用连接池的连接必须实现这个接口
class Poolable {
public:
    virtual ~Poolable() = default;
    virtual bool IsAlive() const = 0;           // 连接是否有效
    virtual void Close() = 0;                   // 关闭连接
};

// 连接池统计信息
struct PoolStats {
    int active_count = 0;       // 使用中的连接数
    int idle_count = 0;         // 空闲连接数
    int total_count = 0;        // 总连接数
    int wait_count = 0;         // 等待获取连接的线程数
    int create_count = 0;       // 创建的连接总数
    int destroy_count = 0;      // 销毁的连接总数
};

// 连接池模板类
// T 必须实现 Poolable 接口
template <typename T>
class ConnectionPool {
public:
    // 连接类型
    using ConnectionPtr = std::shared_ptr<T>;
    // 连接工厂函数类型
    using FactoryFunc = std::function<ConnectionPtr()>;

    ConnectionPool() = default;
    ~ConnectionPool() { Close(); }

    // 禁止拷贝和移动
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // 初始化连接池
    // factory: 创建连接的工厂函数
    // config: 连接池配置
    void Init(FactoryFunc factory, const PoolConfig& config = PoolConfig{}) {
        {
            std::lock_guard<std::mutex> lock(mu_);

            if (initialized_) {
                return;
            }

            factory_ = std::move(factory);
            config_ = config;

            // 预创建最小连接数
            for (int i = 0; i < config_.min_size; ++i) {
                auto conn = CreateConnection();
                if (conn) {
                    idle_.push_back(conn);
                }
            }

            initialized_ = true;
        }

        // 启动后台清理线程（在锁外启动，避免死锁）
        stop_cleanup_ = false;
        cleanup_thread_ = std::thread(&ConnectionPool<T>::CleanupWorkerFunc, this);
    }

    // 获取连接
    // 超时返回 nullptr
    ConnectionPtr Get() {
        std::unique_lock<std::mutex> lock(mu_);

        if (!initialized_) {
            return nullptr;
        }

        // 1. 有空闲连接？直接返回
        if (!idle_.empty()) {
            auto conn = idle_.back();
            idle_.pop_back();
            active_++;
            return conn;
        }

        // 2. 没有空闲，但还没到上限？创建新连接
        if (TotalCount() < config_.max_size) {
            auto conn = CreateConnection();
            if (conn) {
                active_++;
                return conn;
            }
        }

        // 3. 到上限了，等待其他线程归还
        wait_count_++;
        auto status = cv_.wait_for(lock,
            std::chrono::milliseconds(config_.get_timeout_ms),
            [this]() { return !idle_.empty() || !initialized_; });
        wait_count_--;

        if (!status || !initialized_) {
            return nullptr;  // 超时或池已关闭
        }

        if (!idle_.empty()) {
            auto conn = idle_.back();
            idle_.pop_back();
            active_++;
            return conn;
        }

        return nullptr;
    }

    // 归还连接
    void Put(ConnectionPtr conn) {
        if (!conn) return;

        std::lock_guard<std::mutex> lock(mu_);

        if (!initialized_) {
            return;
        }

        active_--;
        // 记录归还时间，用于空闲超时判断
        idle_times_[conn.get()] = std::chrono::steady_clock::now();
        idle_.push_back(std::move(conn));
        cv_.notify_one();  // 唤醒等待的线程
    }

    // 关闭连接池
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mu_);

            initialized_ = false;
            idle_.clear();
            active_ = 0;

            // 唤醒所有等待的线程
            cv_.notify_all();
        }

        // 停止清理线程
        stop_cleanup_ = true;
        cleanup_cv_.notify_one();
        if (cleanup_thread_.joinable()) {
            cleanup_thread_.join();
        }
    }

    // 获取统计信息
    PoolStats GetStats() const {
        std::lock_guard<std::mutex> lock(mu_);
        return {
            active_,
            static_cast<int>(idle_.size()),
            TotalCount(),
            wait_count_,
            create_count_,
            destroy_count_
        };
    }

    // 获取配置
    PoolConfig GetConfig() const {
        std::lock_guard<std::mutex> lock(mu_);
        return config_;
    }

    // 检查连接池是否已初始化
    bool IsInitialized() const {
        std::lock_guard<std::mutex> lock(mu_);
        return initialized_;
    }

private:
    // 创建连接
    ConnectionPtr CreateConnection() {
        if (!factory_) {
            return nullptr;
        }

        try {
            auto conn = factory_();
            if (conn) {
                create_count_++;
                idle_times_[conn.get()] = std::chrono::steady_clock::now();
            }
            return conn;
        } catch (...) {
            return nullptr;
        }
    }

    // 获取总连接数
    int TotalCount() const {
        return active_ + static_cast<int>(idle_.size());
    }

    // 清理空闲连接（保留最小连接数）
    void CleanupIdle() {
        auto now = std::chrono::steady_clock::now();
        auto it = idle_.begin();

        while (it != idle_.end() && TotalCount() > config_.min_size) {
            auto idle_it = idle_times_.find(it->get());
            if (idle_it != idle_times_.end()) {
                auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - idle_it->second).count();
                if (idle_ms > config_.max_idle_ms) {
                    // 超时，销毁连接
                    (*it)->Close();
                    idle_times_.erase(idle_it);
                    it = idle_.erase(it);
                    destroy_count_++;
                    continue;
                }
            }
            ++it;
        }
    }

    // 后台清理线程函数
    void CleanupWorkerFunc() {
        // 清理间隔：取超时时间的 1/4，最少 1 秒，最多 30 秒
        int interval_ms = config_.max_idle_ms / 4;
        if (interval_ms < 1000) interval_ms = 1000;
        if (interval_ms > 30000) interval_ms = 30000;

        while (!stop_cleanup_.load()) {
            std::unique_lock<std::mutex> lock(mu_);
            cleanup_cv_.wait_for(lock,
                std::chrono::milliseconds(interval_ms),
                [this]() { return stop_cleanup_.load(); });

            if (stop_cleanup_.load()) {
                break;
            }

            // 执行清理
            CleanupIdle();
        }
    }

    // 成员变量
    mutable std::mutex mu_;
    std::condition_variable cv_;                    // 等待连接归还
    std::condition_variable cleanup_cv_;            // 唤醒清理线程

    FactoryFunc factory_;
    PoolConfig config_;
    std::vector<ConnectionPtr> idle_;  // 空闲连接列表
    std::unordered_map<void*, std::chrono::steady_clock::time_point> idle_times_;  // 空闲开始时间

    int active_ = 0;           // 使用中的连接数
    int wait_count_ = 0;       // 等待的线程数
    int create_count_ = 0;     // 创建的连接总数
    int destroy_count_ = 0;    // 销毁的连接总数

    bool initialized_ = false;
    std::atomic<bool> stop_cleanup_{false};         // 停止清理线程
    std::thread cleanup_thread_;                    // 后台清理线程
};

// RAII 连接包装器
// 自动获取和归还连接
template <typename T>
class ConnectionGuard {
public:
    using ConnectionPtr = std::shared_ptr<T>;

    ConnectionGuard(ConnectionPool<T>& pool)
        : pool_(pool), conn_(pool.Get()) {}

    ~ConnectionGuard() {
        if (conn_) {
            pool_.Put(std::move(conn_));
        }
    }

    // 禁止拷贝
    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;

    // 允许移动
    ConnectionGuard(ConnectionGuard&& other) noexcept
        : pool_(other.pool_), conn_(std::move(other.conn_)) {}

    ConnectionGuard& operator=(ConnectionGuard&& other) noexcept {
        if (this != &other) {
            if (conn_) {
                pool_.Put(std::move(conn_));
            }
            pool_ = other.pool_;
            conn_ = std::move(other.conn_);
        }
        return *this;
    }

    // 获取连接指针
    ConnectionPtr Get() const { return conn_; }

    // 重载箭头操作符
    T* operator->() const { return conn_.get(); }

    // 重载解引用操作符
    T& operator*() const { return *conn_; }

    // 检查连接是否有效
    explicit operator bool() const { return conn_ != nullptr; }

private:
    ConnectionPool<T>& pool_;
    ConnectionPtr conn_;
};

} // namespace netx

#endif // NETX_CONNECTION_POOL_H_
