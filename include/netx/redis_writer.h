// Redis 写入器
//
// 功能：
// - 订阅数据源更新
// - 将红绿灯状态写入 Redis
// - 供 RedisDataSource 读取

#ifndef NETX_REDIS_WRITER_H_
#define NETX_REDIS_WRITER_H_

#include <memory>
#include <mutex>
#include <string>

#include "netx/connection_pool.h"
#include "netx/data_source.h"
#include "netx/logging.h"
#include "netx/redis_connection.h"
#include "netx/traffic_light.h"

namespace netx {

// Redis 写入器
// 订阅数据源更新，写入 Redis
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
    // 收到更新，写入 Redis
    void OnLightUpdate(const LightUpdate& update) {
        auto conn = pool_.Get();
        if (!conn) return;

        // key: traffic:light:LIGHT-001
        std::string key = "traffic:light:" + update.light_id;

        conn->HSet(key, "light_id", update.light_id);
        conn->HSet(key, "intersection_id", update.intersection_id);
        conn->HSet(key, "direction", update.direction);
        conn->HSet(key, "color",
                   std::to_string(static_cast<int>(update.color)));
        conn->HSet(key, "countdown",
                   std::to_string(update.countdown));
        conn->HSet(key, "timestamp",
                   std::to_string(update.timestamp));

        pool_.Put(conn);
    }

    ConnectionPool<RedisConnection> pool_;
};

}  // namespace netx

#endif  // NETX_REDIS_WRITER_H_
