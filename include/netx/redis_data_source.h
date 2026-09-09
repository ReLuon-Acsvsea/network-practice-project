// Redis 数据源
//
// 功能：
// - 从 Redis 读取红绿灯配置和状态
// - 使用连接池管理 Redis 连接
// - 定时从 Redis 拉取最新状态

#ifndef NETX_REDIS_DATA_SOURCE_H_
#define NETX_REDIS_DATA_SOURCE_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "netx/connection_pool.h"
#include "netx/data_source.h"
#include "netx/redis_connection.h"
#include "netx/traffic_light.h"

namespace netx {

// Redis 数据源
// 从 Redis 读取红绿灯数据
class RedisDataSource : public DataSource {
public:
    RedisDataSource() = default;
    ~RedisDataSource() override { Stop(); }

    // 初始化（使用默认 Redis 配置）
    bool InitDefault() {
        RedisConfig config;
        return Init(config);
    }

    // 初始化（指定 Redis 配置）
    bool Init(const RedisConfig& config) {
        // 创建连接池
        PoolConfig pool_config;
        pool_config.max_size = 5;
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
            LOG_ERROR << "Failed to connect to Redis";
            return false;
        }
        pool_.Put(conn);

        // 从 Redis 加载路口配置
        if (!LoadConfig()) {
            LOG_WARN << "No config in Redis, waiting for data...";
            // 不初始化默认配置，等待 RedisWriter 写入数据
        }

        return true;
    }

    // DataSource 接口
    bool Init() override {
        return InitDefault();
    }

    void Start() override {
        if (running_.load()) return;
        running_ = true;

        worker_ = std::thread([this]() {
            LOG_INFO << "RedisDataSource started";
            while (running_.load()) {
                UpdateFromRedis();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(update_interval_ms_));
            }
            LOG_INFO << "RedisDataSource stopped";
        });
    }

    void Stop() override {
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::vector<Intersection> GetIntersections() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return intersections_;
    }

    Intersection GetIntersection(const std::string& id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& intersection : intersections_) {
            if (intersection.id == id) {
                return intersection;
            }
        }
        return Intersection{};
    }

    TrafficLight GetLightStatus(const std::string& light_id) const override {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& intersection : intersections_) {
            for (const auto& light : intersection.lights) {
                if (light.id == light_id) {
                    return light;
                }
            }
        }
        return TrafficLight{};
    }

    void Subscribe(UpdateCallback cb) override {
        std::lock_guard<std::mutex> lock(mu_);
        subscribers_.push_back(std::move(cb));
    }

    void SetErrorCallback(ErrorCallback cb) override {
        error_callback_ = std::move(cb);
    }

    bool IsRunning() const override { return running_.load(); }

    // 写入红绿灯配置到 Redis（用于测试）
    bool WriteConfig(const Intersection& intersection) {
        auto conn = pool_.Get();
        if (!conn) return false;

        std::string prefix = "traffic:intersection:" + intersection.id;

        // 写入基本信息
        conn->HSet(prefix, "id", intersection.id);
        conn->HSet(prefix, "name", intersection.name);
        conn->HSet(prefix, "latitude", std::to_string(intersection.latitude));
        conn->HSet(prefix, "longitude", std::to_string(intersection.longitude));

        // 写入红绿灯列表
        std::string lights_key = prefix + ":lights";
        conn->Command(nullptr, "DEL %s", lights_key.c_str());
        for (size_t i = 0; i < intersection.lights.size(); ++i) {
            const auto& light = intersection.lights[i];
            std::string light_prefix = lights_key + ":" + std::to_string(i);
            conn->HSet(light_prefix, "id", light.id);
            conn->HSet(light_prefix, "direction", light.direction);
            conn->HSet(light_prefix, "color",
                       std::to_string(static_cast<int>(light.color)));
            conn->HSet(light_prefix, "countdown",
                       std::to_string(light.countdown));
            conn->Command(nullptr, "RPUSH %s %s",
                          lights_key.c_str(), light.id.c_str());
        }

        // 写入时序方案
        std::string schedule_key = prefix + ":schedule";
        conn->HSet(schedule_key, "id", intersection.schedule.id);
        for (size_t i = 0; i < intersection.schedule.phases.size(); ++i) {
            const auto& phase = intersection.schedule.phases[i];
            std::string phase_key = schedule_key + ":phase:" +
                                    std::to_string(i);
            conn->HSet(phase_key, "color",
                       std::to_string(static_cast<int>(phase.color)));
            conn->HSet(phase_key, "duration",
                       std::to_string(phase.duration));
            conn->Command(nullptr, "RPUSH %s:phases %s",
                          schedule_key.c_str(),
                          std::to_string(i).c_str());
        }

        pool_.Put(conn);
        return true;
    }

private:
    // 从 Redis 加载配置
    bool LoadConfig() {
        auto conn = pool_.Get();
        if (!conn) return false;

        std::lock_guard<std::mutex> lock(mu_);
        intersections_.clear();

        // 从 traffic:light:* 读取所有灯
        std::vector<std::string> light_keys;
        if (!conn->Keys("traffic:light:*", &light_keys)) {
            pool_.Put(conn);
            return false;
        }

        if (light_keys.empty()) {
            pool_.Put(conn);
            return false;
        }

        // 按 intersection_id 分组
        std::unordered_map<std::string, Intersection> intersection_map;

        for (const auto& light_key : light_keys) {
            // traffic:light:LIGHT-001
            std::string light_id = light_key.substr(14);  // 去掉 "traffic:light:"

            std::string intersection_id, direction, color_str, countdown_str;
            if (conn->HGet(light_key, "intersection_id", &intersection_id) &&
                conn->HGet(light_key, "direction", &direction) &&
                conn->HGet(light_key, "color", &color_str) &&
                conn->HGet(light_key, "countdown", &countdown_str)) {

                TrafficLight light;
                light.id = light_id;
                light.intersection_id = intersection_id;
                light.direction = direction;
                light.color = static_cast<LightColor>(std::stoi(color_str));
                light.countdown = std::stoi(countdown_str);
                light.timestamp = NowMs();

                intersection_map[intersection_id].lights.push_back(light);
                intersection_map[intersection_id].id = intersection_id;
            }
        }

        // 设置路口名称
        for (auto& [id, intersection] : intersection_map) {
            std::string name;
            std::string key = "traffic:intersection:" + id;
            if (conn->HGet(key, "name", &name)) {
                intersection.name = name;
            }
            intersections_.push_back(intersection);
        }

        pool_.Put(conn);
        LOG_INFO << "Loaded " << intersections_.size() << " intersections from Redis";
        return !intersections_.empty();
    }

    // 使用默认配置（模拟数据）
    void InitDefaultConfig() {
        std::lock_guard<std::mutex> lock(mu_);

        // 路口 1
        Intersection intersection1;
        intersection1.id = "INT-001";
        intersection1.name = "人民路-中山路路口";
        intersection1.latitude = 31.2304;
        intersection1.longitude = 121.4737;
        intersection1.lights = {
            {"LIGHT-001", "INT-001", "东", LightType::kVehicle,
             LightColor::kRed, 27, 0},
            {"LIGHT-002", "INT-001", "南", LightType::kVehicle,
             LightColor::kGreen, 30, 0},
            {"LIGHT-003", "INT-001", "西", LightType::kVehicle,
             LightColor::kRed, 27, 0},
            {"LIGHT-004", "INT-001", "北", LightType::kVehicle,
             LightColor::kGreen, 30, 0}};
        intersection1.schedule.id = "SCHEDULE-001";
        intersection1.schedule.phases = {
            {LightColor::kGreen, 30},
            {LightColor::kYellow, 3},
            {LightColor::kRed, 27}};

        intersections_.push_back(intersection1);
    }

    // 从 Redis 更新状态
    void UpdateFromRedis() {
        auto conn = pool_.Get();
        if (!conn) return;

        // 如果还没加载配置，尝试加载
        if (intersections_.empty()) {
            std::vector<std::string> light_keys;
            if (conn->Keys("traffic:light:*", &light_keys) &&
                !light_keys.empty()) {
                pool_.Put(conn);
                LoadConfig();
                return;
            }
        }

        std::lock_guard<std::mutex> lock(mu_);
        for (auto& intersection : intersections_) {
            for (auto& light : intersection.lights) {
                std::string color_str, countdown_str;
                // 读取格式：traffic:light:LIGHT-001
                std::string key = "traffic:light:" + light.id;

                if (conn->HGet(key, "color", &color_str) &&
                    conn->HGet(key, "countdown", &countdown_str) &&
                    !color_str.empty() && !countdown_str.empty()) {
                    try {
                        light.color = static_cast<LightColor>(
                            std::stoi(color_str));
                        light.countdown = std::stoi(countdown_str);
                        light.timestamp = NowMs();

                        // 通知订阅者
                        LightUpdate update;
                        update.light_id = light.id;
                        update.intersection_id = light.intersection_id;
                        update.direction = light.direction;
                        update.color = light.color;
                        update.countdown = light.countdown;
                        update.timestamp = light.timestamp;
                        NotifySubscribers(update);
                    } catch (const std::exception& e) {
                        LOG_ERROR << "Parse error: " << e.what();
                    }
                }
            }
        }

        pool_.Put(conn);
    }

    void NotifySubscribers(const LightUpdate& update) {
        for (auto& cb : subscribers_) {
            cb(update);
        }
    }

    uint64_t NowMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // 成员变量
    mutable std::mutex mu_;
    ConnectionPool<RedisConnection> pool_;
    std::vector<Intersection> intersections_;
    std::vector<UpdateCallback> subscribers_;
    ErrorCallback error_callback_;

    std::atomic<bool> running_{false};
    std::thread worker_;
    int update_interval_ms_ = 1000;
};

}  // namespace netx

#endif  // NETX_REDIS_DATA_SOURCE_H_
