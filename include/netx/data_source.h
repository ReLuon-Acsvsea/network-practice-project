// 数据源接口定义
//
// 功能：
// - 定义统一的数据源接口
// - 支持多种数据源实现（模拟、HTTP、Redis 等）
// - 支持订阅/发布模式

#ifndef NETX_DATA_SOURCE_H_
#define NETX_DATA_SOURCE_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "netx/traffic_light.h"

namespace netx {

// 数据源接口
class DataSource {
public:
    // 回调函数类型
    using UpdateCallback = std::function<void(const LightUpdate&)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    virtual ~DataSource() = default;

    // 初始化数据源
    virtual bool Init() = 0;

    // 启动数据源
    virtual void Start() = 0;

    // 停止数据源
    virtual void Stop() = 0;

    // 获取所有路口配置
    virtual std::vector<Intersection> GetIntersections() const = 0;

    // 获取单个路口配置
    virtual Intersection GetIntersection(const std::string& id) const = 0;

    // 获取红绿灯实时状态
    virtual TrafficLight GetLightStatus(const std::string& light_id) const = 0;

    // 订阅状态变化
    virtual void Subscribe(UpdateCallback cb) = 0;

    // 设置错误回调
    virtual void SetErrorCallback(ErrorCallback cb) = 0;

    // 检查数据源是否正在运行
    virtual bool IsRunning() const = 0;
};

// 数据源工厂
class DataSourceFactory {
public:
    // 创建数据源
    // type: 数据源类型（"simulator", "http", "redis", "kafka"）
    // config: 配置参数
    static std::unique_ptr<DataSource> Create(const std::string& type,
                                               const std::string& config = "");
};

} // namespace netx

#endif // NETX_DATA_SOURCE_H_
