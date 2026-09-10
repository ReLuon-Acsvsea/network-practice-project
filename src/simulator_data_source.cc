// 模拟数据源实现

#include "netx/simulator_data_source.h"
#include "netx/logging.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace netx {

bool SimulatorDataSource::InitFromConfig(const SimulatorConfig& config) {
    std::lock_guard<std::mutex> lock(mu_);

    config_ = config;
    intersections_ = config.intersections;

    // 初始化各路口的启动时间，均匀错开让每个路口处于不同相位
    uint64_t now = NowMs();
    int n = static_cast<int>(intersections_.size());
    for (int i = 0; i < n; ++i) {
        int cycle_ms = intersections_[i].schedule.CycleDuration() * 1000;
        int offset_ms = (n > 1) ? (cycle_ms * i / n) : 0;
        start_times_[intersections_[i].id] = now - offset_ms;
    }

    LOG_INFO << "SimulatorDataSource initialized with "
             << intersections_.size() << " intersections";
    return true;
}

bool SimulatorDataSource::InitFromFile(const std::string& file_path) {
    // TODO: 从 JSON 文件加载配置
    // 目前使用默认配置
    LOG_WARN << "InitFromFile not implemented, using default config";
    return InitDefault();
}

bool SimulatorDataSource::InitDefault() {
    SimulatorConfig config;

    // 生成50个路口，每个路口4个方向
    const int kNumIntersections = 50;
    const std::vector<std::string> directions = {"东", "南", "西", "北"};

    for (int i = 1; i <= kNumIntersections; ++i) {
        Intersection intersection;
        intersection.id = "INT-" + std::to_string(i);
        intersection.name = "路口-" + std::to_string(i);
        intersection.latitude = 31.23 + i * 0.001;
        intersection.longitude = 121.47 + i * 0.001;

        // 4个方向的红绿灯
        int light_base = (i - 1) * 4 + 1;
        for (int d = 0; d < 4; ++d) {
            TrafficLight light;
            light.id = "L-" + std::to_string(light_base + d);
            light.intersection_id = intersection.id;
            light.direction = directions[d];
            light.type = LightType::kVehicle;
            // 东西方向绿灯在前，南北方向红灯在前
            if (d < 2) {
                light.color = LightColor::kGreen;
                light.countdown = 30;
            } else {
                light.color = LightColor::kRed;
                light.countdown = 33;
            }
            light.timestamp = 0;
            intersection.lights.push_back(light);
        }

        // 时序方案：绿灯30秒 -> 黄灯3秒 -> 红灯27秒
        intersection.schedule.id = "SCH-" + std::to_string(i);
        intersection.schedule.phases = {
            {LightColor::kGreen, 30},
            {LightColor::kYellow, 3},
            {LightColor::kRed, 27}
        };

        config.intersections.push_back(intersection);
    }

    config.update_interval_ms = 1000;  // 每秒更新一次

    return InitFromConfig(config);
}

bool SimulatorDataSource::Init() {
    if (intersections_.empty()) {
        return InitDefault();
    }
    return true;
}

void SimulatorDataSource::Start() {
    if (running_.load()) {
        return;
    }

    running_ = true;
    worker_ = std::thread([this]() {
        LOG_INFO << "SimulatorDataSource started";

        while (running_.load()) {
            try {
                UpdateAllIntersections();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.update_interval_ms));
            } catch (const std::exception& e) {
                if (error_callback_) {
                    error_callback_(e.what());
                }
                LOG_ERROR << "SimulatorDataSource error: " << e.what();
            }
        }

        LOG_INFO << "SimulatorDataSource stopped";
    });
}

void SimulatorDataSource::Stop() {
    if (!running_.load()) {
        return;
    }

    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::vector<Intersection> SimulatorDataSource::GetIntersections() const {
    std::lock_guard<std::mutex> lock(mu_);
    return intersections_;
}

Intersection SimulatorDataSource::GetIntersection(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);

    for (const auto& intersection : intersections_) {
        if (intersection.id == id) {
            return intersection;
        }
    }

    return Intersection{};
}

TrafficLight SimulatorDataSource::GetLightStatus(const std::string& light_id) const {
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

void SimulatorDataSource::Subscribe(UpdateCallback cb) {
    std::lock_guard<std::mutex> lock(mu_);
    subscribers_.push_back(std::move(cb));
}

void SimulatorDataSource::SetErrorCallback(ErrorCallback cb) {
    error_callback_ = std::move(cb);
}

bool SimulatorDataSource::IsRunning() const {
    return running_.load();
}

void SimulatorDataSource::UpdateAllIntersections() {
    std::lock_guard<std::mutex> lock(mu_);

    for (auto& intersection : intersections_) {
        UpdateIntersection(intersection);
    }
}

void SimulatorDataSource::UpdateIntersection(Intersection& intersection) {
    uint64_t now = NowMs();
    uint64_t start_time = start_times_[intersection.id];
    int cycle_duration_ms = intersection.schedule.CycleDuration() * 1000;  // 转换为毫秒

    // 为每个灯计算状态（根据方向设置不同的相位偏移）
    for (auto& light : intersection.lights) {
        // 根据方向设置相位偏移（毫秒）
        int phase_offset_ms = 0;
        if (light.direction == "东" || light.direction == "西") {
            phase_offset_ms = 0;  // 东西方向：绿灯在前
        } else if (light.direction == "南" || light.direction == "北") {
            phase_offset_ms = cycle_duration_ms / 2;  // 南北方向：红灯在前（相位差 180 度）
        }

        // 计算当前颜色和倒计时
        LightColor color;
        int countdown;
        CalculateLightState(intersection.schedule, start_time, now, &color, &countdown, phase_offset_ms);

        light.color = color;
        light.countdown = countdown;
        light.timestamp = now;

        // 通知订阅者
        LightUpdate update;
        update.light_id = light.id;
        update.intersection_id = light.intersection_id;
        update.direction = light.direction;
        update.color = light.color;
        update.countdown = light.countdown;
        update.timestamp = light.timestamp;

        NotifySubscribers(update);
    }
}

void SimulatorDataSource::CalculateLightState(const LightSchedule& schedule,
                                               uint64_t start_time,
                                               uint64_t current_time,
                                               LightColor* color,
                                               int* countdown,
                                               int phase_offset_ms) {
    // 计算已经过去的时间（毫秒）
    uint64_t elapsed_ms = current_time - start_time;

    // 计算总周期时长（毫秒）
    int cycle_duration_ms = schedule.CycleDuration() * 1000;
    if (cycle_duration_ms == 0) {
        *color = LightColor::kRed;
        *countdown = 0;
        return;
    }

    // 计算当前在周期中的位置（加上相位偏移，毫秒）
    int phase_position_ms = (elapsed_ms + phase_offset_ms) % cycle_duration_ms;

    // 找到当前应该是什么颜色
    int accumulated_ms = 0;
    for (const auto& phase : schedule.phases) {
        int duration_ms = phase.duration * 1000;  // 转换为毫秒
        if (phase_position_ms < accumulated_ms + duration_ms) {
            // 当前在这个阶段
            *color = phase.color;
            *countdown = (accumulated_ms + duration_ms - phase_position_ms) / 1000;  // 转换为秒
            return;
        }
        accumulated_ms += duration_ms;
    }

    // 默认返回红色
    *color = LightColor::kRed;
    *countdown = 0;
}

void SimulatorDataSource::NotifySubscribers(const LightUpdate& update) {
    // 注意：调用时已经持有锁
    for (const auto& cb : subscribers_) {
        try {
            cb(update);
        } catch (const std::exception& e) {
            LOG_ERROR << "Subscriber callback error: " << e.what();
        }
    }
}

uint64_t SimulatorDataSource::NowMs() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

} // namespace netx
