// Redis 连接包装器
//
// 功能：
// - 封装 hiredis，提供简单接口
// - 实现 Poolable 接口，支持连接池
// - 支持连接/断开/查询

#ifndef NETX_REDIS_CONNECTION_H_
#define NETX_REDIS_CONNECTION_H_

#include <hiredis/hiredis.h>

#include <memory>
#include <mutex>
#include <string>

#include "netx/connection_pool.h"
#include "netx/logging.h"

namespace netx {

// Redis 连接配置
struct RedisConfig {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int connect_timeout_ms = 3000;
    int command_timeout_ms = 1000;
};

// Redis 连接类
class RedisConnection : public Poolable {
public:
    RedisConnection() = default;
    ~RedisConnection() override { Close(); }

    // 禁止拷贝
    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    // 连接到 Redis
    bool Connect(const RedisConfig& config) {
        std::lock_guard<std::mutex> lock(mu_);

        if (ctx_) {
            return true;  // 已连接
        }

        config_ = config;

        // 设置连接超时
        struct timeval tv;
        tv.tv_sec = config.connect_timeout_ms / 1000;
        tv.tv_usec = (config.connect_timeout_ms % 1000) * 1000;

        ctx_ = redisConnectWithTimeout(config.host.c_str(), config.port, tv);
        if (!ctx_ || ctx_->err) {
            if (ctx_) {
                LOG_ERROR << "Redis connect failed: " << ctx_->errstr;
                redisFree(ctx_);
                ctx_ = nullptr;
            }
            return false;
        }

        // 认证
        if (!config.password.empty()) {
            redisReply* reply = static_cast<redisReply*>(
                redisCommand(ctx_, "AUTH %s", config.password.c_str()));
            if (!reply || reply->type == REDIS_REPLY_ERROR) {
                LOG_ERROR << "Redis auth failed";
                if (reply) freeReplyObject(reply);
                redisFree(ctx_);
                ctx_ = nullptr;
                return false;
            }
            freeReplyObject(reply);
        }

        // 选择数据库
        if (config.db != 0) {
            redisReply* reply = static_cast<redisReply*>(
                redisCommand(ctx_, "SELECT %d", config.db));
            if (!reply || reply->type == REDIS_REPLY_ERROR) {
                LOG_ERROR << "Redis select db failed";
                if (reply) freeReplyObject(reply);
                redisFree(ctx_);
                ctx_ = nullptr;
                return false;
            }
            freeReplyObject(reply);
        }

        LOG_INFO << "Redis connected: " << config.host << ":" << config.port;
        return true;
    }

    // Poolable 接口
    bool IsAlive() const override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ctx_) return false;

        // PING 检测
        redisReply* reply = static_cast<redisReply*>(
            redisCommand(ctx_, "PING"));
        if (!reply) return false;

        bool ok = (reply->type == REDIS_REPLY_STATUS &&
                   std::string(reply->str) == "PONG");
        freeReplyObject(reply);
        return ok;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (ctx_) {
            redisFree(ctx_);
            ctx_ = nullptr;
        }
    }

    // 执行命令（返回结果字符串）
    // 成功返回 true，结果存在 result 中（result 可以为 nullptr）
    bool Command(std::string* result, const char* fmt, ...) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ctx_) return false;

        va_list ap;
        va_start(ap, fmt);
        redisReply* reply = static_cast<redisReply*>(
            redisvCommand(ctx_, fmt, ap));
        va_end(ap);

        if (!reply) {
            return false;
        }

        bool ok = true;
        if (result) {
            if (reply->type == REDIS_REPLY_STRING) {
                *result = reply->str;
            } else if (reply->type == REDIS_REPLY_INTEGER) {
                *result = std::to_string(reply->integer);
            } else if (reply->type == REDIS_REPLY_STATUS) {
                *result = reply->str ? reply->str : "";
            } else if (reply->type == REDIS_REPLY_NIL) {
                result->clear();
            } else {
                ok = false;
            }
        }

        freeReplyObject(reply);
        return ok;
    }

    // SET 命令
    bool Set(const std::string& key, const std::string& value) {
        std::string result;
        return Command(&result, "SET %s %s", key.c_str(), value.c_str());
    }

    // GET 命令
    bool Get(const std::string& key, std::string* value) {
        return Command(value, "GET %s", key.c_str());
    }

    // HSET 命令
    bool HSet(const std::string& key, const std::string& field,
              const std::string& value) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ctx_) return false;

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(ctx_, "HSET %s %s %s",
                         key.c_str(), field.c_str(), value.c_str()));
        if (!reply) return false;

        bool ok = (reply->type == REDIS_REPLY_INTEGER ||
                   reply->type == REDIS_REPLY_STATUS);
        freeReplyObject(reply);
        return ok;
    }

    // HGET 命令
    bool HGet(const std::string& key, const std::string& field,
              std::string* value) {
        return Command(value, "HGET %s %s", key.c_str(), field.c_str());
    }

    // HGETALL 命令
    bool HGetAll(const std::string& key,
                 std::unordered_map<std::string, std::string>* result) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ctx_) return false;

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(ctx_, "HGETALL %s", key.c_str()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY) {
            if (reply) freeReplyObject(reply);
            return false;
        }

        result->clear();
        for (size_t i = 0; i + 1 < reply->elements; i += 2) {
            std::string field = reply->element[i]->str;
            std::string value = reply->element[i + 1]->str;
            (*result)[field] = value;
        }

        freeReplyObject(reply);
        return true;
    }

    // HSET 多字段版本（一次命令设置多个字段）
    bool HMSet(const std::string& key,
               const std::unordered_map<std::string, std::string>& fields) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ctx_ || fields.empty()) return false;

        // 构建 HSET key field1 value1 field2 value2 ...
        std::string cmd = "HSET " + key;
        for (const auto& [field, value] : fields) {
            cmd += " " + field + " " + value;
        }

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(ctx_, cmd.c_str()));
        if (!reply) return false;

        bool ok = (reply->type == REDIS_REPLY_INTEGER ||
                   reply->type == REDIS_REPLY_STATUS);
        freeReplyObject(reply);
        return ok;
    }

    // Pipeline 批量 HSet（一次往返完成多组写入）
    // 返回成功写入的数量
    int PipelineHSet(const std::vector<std::pair<std::string,
                      std::unordered_map<std::string, std::string>>>& batch) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ctx_ || batch.empty()) return 0;

        // 发送阶段：把所有命令追加到管道
        for (const auto& [key, fields] : batch) {
            std::string cmd = "HSET " + key;
            for (const auto& [field, value] : fields) {
                cmd += " " + field + " " + value;
            }
            redisAppendCommand(ctx_, cmd.c_str());
        }

        // 接收阶段：逐个取回复
        int ok_count = 0;
        for (size_t i = 0; i < batch.size(); ++i) {
            redisReply* reply = nullptr;
            redisGetReply(ctx_, reinterpret_cast<void**>(&reply));
            if (!reply) break;

            if (reply->type == REDIS_REPLY_INTEGER ||
                reply->type == REDIS_REPLY_STATUS) {
                ++ok_count;
            }
            freeReplyObject(reply);
        }

        return ok_count;
    }

    // KEYS 命令（返回所有匹配的 key）
    bool Keys(const std::string& pattern, std::vector<std::string>* result) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ctx_) return false;

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(ctx_, "KEYS %s", pattern.c_str()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY) {
            if (reply) freeReplyObject(reply);
            return false;
        }

        result->clear();
        for (size_t i = 0; i < reply->elements; ++i) {
            if (reply->element[i]->str) {
                result->push_back(reply->element[i]->str);
            }
        }

        freeReplyObject(reply);
        return true;
    }

    // 获取连接上下文
    redisContext* ctx() const { return ctx_; }

private:
    mutable std::mutex mu_;
    redisContext* ctx_ = nullptr;
    RedisConfig config_;
};

}  // namespace netx

#endif  // NETX_REDIS_CONNECTION_H_
