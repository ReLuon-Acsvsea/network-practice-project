// 连接池测试
//
// 测试功能：
// 1. 连接池初始化
// 2. 获取/归还连接
// 3. 最大连接数限制
// 4. RAII 自动归还
// 5. 多线程并发

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "netx/connection_pool.h"
#include "netx/logging.h"

using netx::ConnectionGuard;
using netx::ConnectionPool;
using netx::PoolConfig;
using netx::Poolable;

// 模拟连接类
class MockConnection : public Poolable {
public:
    MockConnection(int id) : id_(id) {
        std::cout << "  创建连接 #" << id_ << std::endl;
    }

    ~MockConnection() {
        std::cout << "  销毁连接 #" << id_ << std::endl;
    }

    bool IsAlive() const override { return alive_; }
    void Close() override { alive_ = false; }
    int id() const { return id_; }

    // 模拟查询
    std::string Query(const std::string& sql) {
        return "Result for: " + sql + " (conn #" + std::to_string(id_) + ")";
    }

private:
    int id_;
    bool alive_ = true;
};

int main() {
    LOG_INFO << "连接池测试开始";

    // 创建连接池
    ConnectionPool<MockConnection> pool;

    // 配置：最大5个，最小2个
    PoolConfig config;
    config.max_size = 5;
    config.min_size = 2;

    // 工厂函数
    int next_id = 1;
    auto factory = [&next_id]() {
        return std::make_shared<MockConnection>(next_id++);
    };

    // 初始化
    std::cout << "\n===== 初始化 =====" << std::endl;
    pool.Init(factory, config);

    auto stats = pool.GetStats();
    std::cout << "总连接: " << stats.total_count
              << ", 空闲: " << stats.idle_count
              << ", 活跃: " << stats.active_count << std::endl;

    // 测试1：获取/归还
    std::cout << "\n===== 测试1：获取/归还 =====" << std::endl;
    auto conn1 = pool.Get();
    std::cout << "获取连接: #" << conn1->id() << std::endl;

    auto conn2 = pool.Get();
    std::cout << "获取连接: #" << conn2->id() << std::endl;

    stats = pool.GetStats();
    std::cout << "总连接: " << stats.total_count
              << ", 空闲: " << stats.idle_count
              << ", 活跃: " << stats.active_count << std::endl;

    pool.Put(conn1);
    std::cout << "归还连接: #" << conn1->id() << std::endl;

    stats = pool.GetStats();
    std::cout << "总连接: " << stats.total_count
              << ", 空闲: " << stats.idle_count
              << ", 活跃: " << stats.active_count << std::endl;

    // 测试2：RAII 自动归还
    std::cout << "\n===== 测试2：RAII 自动归还 =====" << std::endl;
    {
        ConnectionGuard<MockConnection> guard(pool);
        if (guard) {
            std::cout << "RAII 获取连接: #" << guard->id() << std::endl;
            std::cout << "查询结果: " << guard->Query("SELECT 1") << std::endl;
        }
        stats = pool.GetStats();
        std::cout << "RAII 作用域内 - 总连接: " << stats.total_count
                  << ", 空闲: " << stats.idle_count
                  << ", 活跃: " << stats.active_count << std::endl;
    }
    stats = pool.GetStats();
    std::cout << "RAII 作用域外 - 总连接: " << stats.total_count
              << ", 空闲: " << stats.idle_count
              << ", 活跃: " << stats.active_count << std::endl;

    // 测试3：多线程并发
    std::cout << "\n===== 测试3：多线程并发 =====" << std::endl;
    std::vector<std::thread> threads;
    for (int i = 0; i < 3; ++i) {
        threads.emplace_back([&pool, i]() {
            ConnectionGuard<MockConnection> guard(pool);
            if (guard) {
                std::cout << "线程 " << i << " 获取连接: #" << guard->id() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    stats = pool.GetStats();
    std::cout << "多线程后 - 总连接: " << stats.total_count
              << ", 空闲: " << stats.idle_count
              << ", 活跃: " << stats.active_count << std::endl;

    // 关闭连接池
    std::cout << "\n===== 关闭 =====" << std::endl;
    pool.Close();

    // 测试4：空闲超时回收
    std::cout << "\n===== 测试4：空闲超时回收 =====" << std::endl;
    {
        ConnectionPool<MockConnection> timeout_pool;
        PoolConfig timeout_config;
        timeout_config.max_size = 5;
        timeout_config.min_size = 1;
        timeout_config.max_idle_ms = 2000;  // 2秒超时

        int tid = 100;
        auto timeout_factory = [&tid]() {
            return std::make_shared<MockConnection>(tid++);
        };

        timeout_pool.Init(timeout_factory, timeout_config);
        // 等待清理线程启动
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stats = timeout_pool.GetStats();
        std::cout << "初始化 - 总连接: " << stats.total_count
                  << ", 空闲: " << stats.idle_count << std::endl;

        // 获取并归还3个连接
        auto c1 = timeout_pool.Get();
        auto c2 = timeout_pool.Get();
        auto c3 = timeout_pool.Get();
        timeout_pool.Put(c1);
        timeout_pool.Put(c2);
        timeout_pool.Put(c3);

        stats = timeout_pool.GetStats();
        std::cout << "归还后 - 总连接: " << stats.total_count
                  << ", 空闲: " << stats.idle_count << std::endl;

        // 等待超时清理
        std::cout << "等待 3 秒让空闲连接超时..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));

        stats = timeout_pool.GetStats();
        std::cout << "超时后 - 总连接: " << stats.total_count
                  << ", 空闲: " << stats.idle_count
                  << " (应保留 min_size=" << timeout_config.min_size << " 个)" << std::endl;

        timeout_pool.Close();
    }

    LOG_INFO << "连接池测试完成";
    return 0;
}
