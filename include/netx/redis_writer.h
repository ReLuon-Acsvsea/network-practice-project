// Redis 写入器
//
// 功能：
// - 订阅数据源更新
// - 将红绿灯状态写入 Redis
// - 供 RedisDataSource 读取
// - 使用 pipeline 批量写入，减少网络往返

#ifndef NETX_REDIS_WRITER_H_
#define NETX_REDIS_WRITER_H_

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "netx/connection_pool.h"
#include "netx/data_source.h"
#include "netx/logging.h"
#include "netx/redis_connection.h"
#include "netx/traffic_light.h"

namespace netx {

// Redis 写入器
// 订阅数据源更新，用 pipeline 批量写入 Redis
class RedisWriter {
public:
    RedisWriter() = default;
    ~RedisWriter() = default;

    // 初始化（连接 Redis）
    bool Init(const RedisConfig& config) {
        PoolConfig pool_config;
        pool_config.max_size = 3;
        pool_config.min_size = 1;

        auto factory = [config]() {
            auto conn = std::make_shared<RedisConnection>();
            if (!conn->Connect(config)) {
                return std::shared_ptr<RedisConnection>();
            }
            return conn;
        };

        pool_.Init(factory, pool_config);

        // 测试连接
        auto conn = pool_.Get();
        if (!conn) {
            LOG_ERROR << "RedisWriter: Failed to connect to Redis";
            return false;
        }
        pool_.Put(conn);

        LOG_INFO << "RedisWriter initialized";
        return true;
    }

    // 订阅数据源
    void SubscribeTo(DataSource* source) {
        if (!source) return;

        source->Subscribe([this](const LightUpdate& update) {
            OnLightUpdate(update);
        });

        LOG_INFO << "RedisWriter subscribed to data source";
    }

private:
    // 收到更新，收集到缓冲区
    void OnLightUpdate(const LightUpdate& update) {
        std::lock_guard<std::mutex> lock(mu_);

        std::string key = "traffic:light:" + update.light_id;
        std::unordered_map<std::string, std::string> fields = {
            {"light_id",       update.light_id},
            {"intersection_id", update.intersection_id},
            {"direction",      update.direction},
            {"color",          std::to_string(static_cast<int>(update.color))},
            {"countdown",      std::to_string(update.countdown)},
            {"timestamp",      std::to_string(update.timestamp)},
        };

        pending_batch_.emplace_back(std::move(key), std::move(fields));

        // 攒够一批 或 距上次刷新超过1秒 → 立即发送
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_flush_time_).count();

        if (static_cast<int>(pending_batch_.size()) >= batch_size_ ||
            elapsed >= flush_interval_ms_) {
            FlushBatch();
        }
    }

    // 用 pipeline 批量发送
    void FlushBatch() {
        if (pending_batch_.empty()) return;

        auto conn = pool_.Get();
        if (!conn) {
            LOG_ERROR << "RedisWriter: No available connection";
            pending_batch_.clear();
            return;
        }

        int ok = conn->PipelineHSet(pending_batch_);
        if (ok < static_cast<int>(pending_batch_.size())) {
            LOG_ERROR << "RedisWriter: Pipeline partial failure: "
                      << ok << "/" << pending_batch_.size();
        }

        pool_.Put(conn);
        pending_batch_.clear();
        last_flush_time_ = std::chrono::steady_clock::now();
    }

    mutable std::mutex mu_;
    ConnectionPool<RedisConnection> pool_;
    std::vector<std::pair<std::string,
        std::unordered_map<std::string, std::string>>> pending_batch_;
    int batch_size_ = 50;  // 每批最多 50 组
    int flush_interval_ms_ = 1000;  // 最多等1秒
    std::chrono::steady_clock::time_point last_flush_time_ =
        std::chrono::steady_clock::now();
};

}  // namespace netx

#endif  // NETX_REDIS_WRITER_H_
