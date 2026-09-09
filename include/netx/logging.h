#ifndef NETX_LOGGING_H_
#define NETX_LOGGING_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace netx {

// 日志级别
enum class LogLevel { kTrace = 0, kDebug, kInfo, kWarn, kError };

// 队列类型（是否启用 MPMC，无锁多生产者多消费者环形队列）
enum class LogQueueType { kBlocking = 0, kMPMC };

// 日志配置（可在程序启动阶段调用 InitLogging 配置）
struct LoggingConfig {
  bool async = true;                              // 是否异步日志（false 则前端直接写）
  LogQueueType queue_type = LogQueueType::kMPMC;  // 队列类型，MPMC 或简单阻塞队列
  std::size_t queue_capacity = 1024;              // 队列容量（MPMC 时要求为 2 的幂）
  const char* log_file = nullptr;                 // 日志文件路径，nullptr 或空串则输出到 stderr
  int flush_interval_ms = 200;                    // 后台线程周期性 flush 间隔
};

namespace detail {

// 小固定缓冲区，用于前端格式化日志内容，避免频繁分配
constexpr int kSmallLogBuffer = 4000;

template <int SIZE>
class FixedBuffer {
 public:
  FixedBuffer() : cur_(data_) {}

  void append(const char* buf, std::size_t len) {
    if (static_cast<std::size_t>(avail()) >= len) {
      std::memcpy(cur_, buf, len);
      cur_ += len;
    }
  }

  const char* data() const { return data_; }
  int length() const { return static_cast<int>(cur_ - data_); }
  int avail() const { return static_cast<int>(end() - cur_); }
  void reset() { cur_ = data_; }

 private:
  const char* end() const { return data_ + sizeof(data_); }

  char data_[SIZE];
  char* cur_;
};

// 轻量级 LogStream，支持常用类型的 operator<<，内部使用 FixedBuffer
class LogStream {
 public:
  LogStream();

  void reset() { buffer_.reset(); }
  const char* data() const { return buffer_.data(); }
  int size() const { return buffer_.length(); }

  // 常用类型重载（只保留最热路径）
  LogStream& operator<<(bool v);
  LogStream& operator<<(short v);
  LogStream& operator<<(unsigned short v);
  LogStream& operator<<(int v);
  LogStream& operator<<(unsigned int v);
  LogStream& operator<<(long v);
  LogStream& operator<<(unsigned long v);
  LogStream& operator<<(long long v);
  LogStream& operator<<(unsigned long long v);
  LogStream& operator<<(float v);
  LogStream& operator<<(double v);
  LogStream& operator<<(char v);
  LogStream& operator<<(const char* s);
  LogStream& operator<<(const unsigned char* s);
  LogStream& operator<<(const std::string& s);

  // 禁止拷贝，允许移动
  LogStream(const LogStream&) = delete;
  LogStream& operator=(const LogStream&) = delete;

 private:
  template <typename T>
  void formatInteger(T v);

  FixedBuffer<kSmallLogBuffer> buffer_;
};

}  // namespace detail

// Logger：RAII 日志前端，析构时将格式化好的日志行推送到后台
class Logger {
 public:
  Logger(const char* file, int line, LogLevel level);
  ~Logger();

  detail::LogStream& stream() { return stream_; }

  static void set_level(LogLevel lvl) { global_level_ = lvl; }
  static LogLevel level() { return global_level_; }

 private:
  const char* file_;
  int line_;
  LogLevel level_;
  detail::LogStream stream_;
  static LogLevel global_level_;
};

// 日志子系统初始化 / 关闭（建议在 main 中调用）
void InitLogging(const LoggingConfig& config);
void ShutdownLogging();

// 宏接口：按日志级别输出
#define LOG_TRACE                                                                  \
  if (netx::Logger::level() <= netx::LogLevel::kTrace)                             \
  netx::Logger(__FILE__, __LINE__, netx::LogLevel::kTrace).stream()

#define LOG_DEBUG                                                                  \
  if (netx::Logger::level() <= netx::LogLevel::kDebug)                             \
  netx::Logger(__FILE__, __LINE__, netx::LogLevel::kDebug).stream()

#define LOG_INFO                                                                   \
  if (netx::Logger::level() <= netx::LogLevel::kInfo)                              \
  netx::Logger(__FILE__, __LINE__, netx::LogLevel::kInfo).stream()

#define LOG_WARN                                                                   \
  if (netx::Logger::level() <= netx::LogLevel::kWarn)                              \
  netx::Logger(__FILE__, __LINE__, netx::LogLevel::kWarn).stream()

#define LOG_ERROR                                                                  \
  if (netx::Logger::level() <= netx::LogLevel::kError)                             \
  netx::Logger(__FILE__, __LINE__, netx::LogLevel::kError).stream()

}  // namespace netx

#endif  // NETX_LOGGING_H_
