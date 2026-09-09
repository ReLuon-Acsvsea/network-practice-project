// 模拟数据源实现
//
// 功能：
// - 基于配置文件生成模拟的红绿灯数据
// - 支持多路口、多方向、多类型红绿灯
// - 按照时序方案自动更新状态
// - 支持订阅/发布模式

#ifndef NETX_SIMULATOR_DATA_SOURCE_H_
#define NETX_SIMULATOR_DATA_SOURCE_H_

#include "netx/data_source.h"
#include "netx/logging.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace netx {

// 模拟数据源配置
struct SimulatorConfig {
    // 路口列表
    std::vector<Intersection> intersections;

    // 更新间隔（毫秒）
    int update_interval_ms = 1000;
};

// 模拟数据源实现
class SimulatorDataSource : public DataSource {
public:
    SimulatorDataSource() = default;
    ~SimulatorDataSource() override { Stop(); }

    // 从配置初始化
    bool InitFromConfig(const SimulatorConfig& config);

    // 从 JSON 文件初始化
    bool InitFromFile(const std::string& file_path);

    // 初始化默认配置（用于测试）
    bool InitDefault();

    // DataSource 接口实现
    bool Init() override;
    void Start() override;
    void Stop() override;

    std::vector<Intersection> GetIntersections() const override;
    Intersection GetIntersection(const std::string& id) const override;
    TrafficLight GetLightStatus(const std::string& light_id) const override;

    void Subscribe(UpdateCallback cb) override;
    void SetErrorCallback(ErrorCallback cb) override;
    bool IsRunning() const override;

private:
    // 更新所有路口状态
    void UpdateAllIntersections();

    // 更新单个路口
    void UpdateIntersection(Intersection& intersection);

    // 计算当前颜色和倒计时
    void CalculateLightState(const LightSchedule& schedule,
                             uint64_t start_time,
                             uint64_t current_time,
                             LightColor* color,
                             int* countdown,
                             int phase_offset_ms = 0);

    // 通知订阅者
    void NotifySubscribers(const LightUpdate& update);

    // 获取当前时间戳（毫秒）
    uint64_t NowMs() const;

    // 成员变量
    SimulatorConfig config_;
    std::vector<Intersection> intersections_;

    // 回调函数
    std::vector<UpdateCallback> subscribers_;
    ErrorCallback error_callback_;

    // 线程相关
    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex mu_;

    // 各路口的启动时间
    std::unordered_map<std::string, uint64_t> start_times_;
};

} // namespace netx

#endif // NETX_SIMULATOR_DATA_SOURCE_H_
