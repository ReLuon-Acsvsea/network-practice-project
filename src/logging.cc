#include "netx/logging.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <sys/syscall.h>
#include <unistd.h>

namespace netx {

namespace {

// 全局日志级别
std::atomic<LogLevel> g_global_level{LogLevel::kInfo};

// 日志配置（只在 InitLogging 时设置）
LoggingConfig g_config{};

// 日志级别枚举转字符串
inline const char* LevelName(LogLevel l) {
  switch (l) {
    case LogLevel::kTrace: return "TRACE";
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO";
    case LogLevel::kWarn:  return "WARN";
    case LogLevel::kError: return "ERROR";
  }
  return "INFO";
}

// 每线程缓存的时间：YYYY-MM-DD HH:MM:SS
struct CachedTime {
  std::time_t last_sec{0};
  char formatted[20]{};  // "YYYY-MM-DD HH:MM:SS"
};

thread_local CachedTime t_cached_time;

// 获取当前时间字符串和毫秒
inline void FormatCurrentTime(char* buf, std::size_t buf_size, int* out_ms) {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto seconds = system_clock::to_time_t(now);
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  *out_ms = static_cast<int>(ms.count());

  if (seconds != t_cached_time.last_sec) {
    t_cached_time.last_sec = seconds;
    std::tm tm_time;
    localtime_r(&seconds, &tm_time);
    std::snprintf(t_cached_time.formatted,
                  sizeof(t_cached_time.formatted),
                  "%04d-%02d-%02d %02d:%02d:%02d",
                  tm_time.tm_year + 1900,
                  tm_time.tm_mon + 1,
                  tm_time.tm_mday,
                  tm_time.tm_hour,
                  tm_time.tm_min,
                  tm_time.tm_sec);
  }

  std::snprintf(buf, buf_size, "%s.%03d", t_cached_time.formatted, *out_ms);
}

// 每线程缓存 TID 及其字符串形式
struct CachedTid {
  pid_t tid{0};
  char buf[32]{};
  int len{0};
};

thread_local CachedTid t_cached_tid;

inline const char* GetTidString(int* len) {
  if (t_cached_tid.tid == 0) {
    pid_t tid = static_cast<pid_t>(::syscall(SYS_gettid));
    t_cached_tid.tid = tid;
    t_cached_tid.len =
        std::snprintf(t_cached_tid.buf, sizeof(t_cached_tid.buf), "tid=%d", tid);
  }
  *len = t_cached_tid.len;
  return t_cached_tid.buf;
}

// 简单阻塞队列（多生产者单消费者）
class BlockingQueue {
 public:
  BlockingQueue() = default;

  void push(std::string&& s) {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      queue_.push_back(std::move(s));
    }
    cv_.notify_one();
  }

  // 返回 false 表示队列已停止且为空
  bool pop(std::string& out) {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this] { return stopped_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      stopped_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  bool stopped_{false};
};

// 有界 MPMC 队列（Dmitry Vyukov 算法的简化版）
class MpmcQueue {
 public:
  explicit MpmcQueue(std::size_t capacity)
      : buffer_mask_(RoundUpToPowerOf2(capacity) - 1),
        buffer_(new Cell[buffer_mask_ + 1]) {
    for (std::size_t i = 0; i != buffer_mask_ + 1; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
    enqueue_pos_.store(0, std::memory_order_relaxed);
    dequeue_pos_.store(0, std::memory_order_relaxed);
  }

  bool push(std::string&& data) {
    Cell* cell;
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &buffer_[pos & buffer_mask_];
      std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) -
                      static_cast<intptr_t>(pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(
                pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        // 队列满
        return false;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
    cell->data = std::move(data);
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool pop(std::string& data) {
    Cell* cell;
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      cell = &buffer_[pos & buffer_mask_];
      std::size_t seq = cell->sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) -
                      static_cast<intptr_t>(pos + 1);
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(
                pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        // 队列空
        return false;
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
    data = std::move(cell->data);
    cell->sequence.store(pos + buffer_mask_ + 1,
                         std::memory_order_release);
    return true;
  }

 private:
  struct Cell {
    std::atomic<std::size_t> sequence;
    std::string data;
  };

  static std::size_t RoundUpToPowerOf2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  const std::size_t buffer_mask_;
  std::unique_ptr<Cell[]> buffer_;
  std::atomic<std::size_t> enqueue_pos_;
  std::atomic<std::size_t> dequeue_pos_;
};

// 后台日志线程与队列管理
class LogBackend {
 public:
  static LogBackend& Instance() {
    static LogBackend backend;
    return backend;
  }

  void Configure(const LoggingConfig& cfg) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (running_) return;  // 运行中不允许重新配置
    config_ = cfg;
  }

  void Start() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (running_ || !config_.async) return;
    running_ = true;
    openOutput();
    if (config_.queue_type == LogQueueType::kBlocking) {
      blocking_queue_.reset(new BlockingQueue());
    } else {
      mpmc_queue_.reset(new MpmcQueue(config_.queue_capacity));
    }
    worker_ = std::thread(&LogBackend::ThreadFunc, this);
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (!running_) return;
      running_ = false;
      if (blocking_queue_) {
        blocking_queue_->stop();
      }
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    flushAndClose();
  }

  // 前端调用：推送一条已经格式化好的日志行（不带换行）
  void Append(std::string&& line) {
    if (!config_.async) {
      WriteSync(line);
      return;
    }
    if (!running_) {
      WriteSync(line);
      return;
    }
    // 尝试入队，MPMC 队列满时直接丢弃（避免阻塞业务线程）
    bool ok = false;
    if (config_.queue_type == LogQueueType::kBlocking) {
      blocking_queue_->push(std::move(line));
      ok = true;
    } else {
      ok = mpmc_queue_->push(std::move(line));
    }
    if (!ok) {
      // 极端情况下队列满：回退到同步写，保证尽量不丢关键日志
      WriteSync(line);
    }
  }

 private:
  LogBackend() = default;
  ~LogBackend() { Stop(); }

  void ThreadFunc() {
    std::string line;
    while (true) {
      if (config_.queue_type == LogQueueType::kBlocking) {
        if (!blocking_queue_->pop(line)) {
          break;  // stopped 且队列空
        }
      } else {
        if (!mpmc_queue_->pop(line)) {
          // 空队列
          if (!running_) break;
          std::this_thread::sleep_for(
              std::chrono::milliseconds(config_.flush_interval_ms));
          FlushUnlocked();
          continue;
        }
      }
      WriteSync(line);
    }
    FlushUnlocked();
  }

  void openOutput() {
    if (config_.log_file && config_.log_file[0] != '\0') {
      file_ = std::fopen(config_.log_file, "ae");
    }
    if (!file_) {
      file_ = stderr;
    }
  }

  void flushAndClose() {
    std::lock_guard<std::mutex> lk(io_mutex_);
    if (file_) {
      std::fflush(file_);
      if (file_ != stderr) {
        std::fclose(file_);
      }
      file_ = nullptr;
    }
  }

  void WriteSync(const std::string& line) {
    std::lock_guard<std::mutex> lk(io_mutex_);
    if (!file_) {
      file_ = stderr;
    }
    std::fwrite(line.data(), 1, line.size(), file_);
    std::fputc('\n', file_);
  }

  void FlushUnlocked() {
    std::lock_guard<std::mutex> lk(io_mutex_);
    if (file_) {
      std::fflush(file_);
    }
  }

  LoggingConfig config_{};
  bool running_{false};
  std::mutex mutex_;

  std::unique_ptr<BlockingQueue> blocking_queue_;
  std::unique_ptr<MpmcQueue> mpmc_queue_;
  std::thread worker_;

  std::mutex io_mutex_;
  FILE* file_{nullptr};
};

}  // namespace

// ======================= detail::LogStream 实现 =========================

namespace detail {

namespace {

const char kDigits[] = "9876543210123456789";
const char* const kZero = kDigits + 9;

}  // namespace

LogStream::LogStream() : buffer_() {}

template <typename T>
void LogStream::formatInteger(T v) {
  char buf[32];
  char* p = buf + sizeof(buf);
  T value = v;
  bool negative = value < 0;
  if (negative) {
    value = -value;
  }
  do {
    int lsd = static_cast<int>(value % 10);
    value /= 10;
    *--p = kZero[lsd];
  } while (value != 0);
  if (negative) {
    *--p = '-';
  }
  buffer_.append(p, static_cast<std::size_t>(buf + sizeof(buf) - p));
}

LogStream& LogStream::operator<<(bool v) {
  buffer_.append(v ? "1" : "0", 1);
  return *this;
}

LogStream& LogStream::operator<<(short v) {
  formatInteger(v);
  return *this;
}

LogStream& LogStream::operator<<(unsigned short v) {
  formatInteger(v);
  return *this;
}

LogStream& LogStream::operator<<(int v) {
  formatInteger(v);
  return *this;
}

LogStream& LogStream::operator<<(unsigned int v) {
  formatInteger(v);
  return *this;
}

LogStream& LogStream::operator<<(long v) {
  formatInteger(v);
  return *this;
}

LogStream& LogStream::operator<<(unsigned long v) {
  formatInteger(v);
  return *this;
}

LogStream& LogStream::operator<<(long long v) {
  formatInteger(v);
  return *this;
}

LogStream& LogStream::operator<<(unsigned long long v) {
  formatInteger(v);
  return *this;
}

LogStream& LogStream::operator<<(float v) {
  char buf[32];
  int len = std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
  buffer_.append(buf, static_cast<std::size_t>(len));
  return *this;
}

LogStream& LogStream::operator<<(double v) {
  char buf[32];
  int len = std::snprintf(buf, sizeof(buf), "%.6g", v);
  buffer_.append(buf, static_cast<std::size_t>(len));
  return *this;
}

LogStream& LogStream::operator<<(char v) {
  buffer_.append(&v, 1);
  return *this;
}

LogStream& LogStream::operator<<(const char* s) {
  if (s) {
    buffer_.append(s, std::strlen(s));
  } else {
    buffer_.append("(null)", 6);
  }
  return *this;
}

LogStream& LogStream::operator<<(const unsigned char* s) {
  return operator<<(reinterpret_cast<const char*>(s));
}

LogStream& LogStream::operator<<(const std::string& s) {
  buffer_.append(s.data(), s.size());
  return *this;
}

// 显式实例化模板
template void LogStream::formatInteger<int>(int);
template void LogStream::formatInteger<unsigned int>(unsigned int);
template void LogStream::formatInteger<long>(long);
template void LogStream::formatInteger<unsigned long>(unsigned long);
template void LogStream::formatInteger<long long>(long long);
template void LogStream::formatInteger<unsigned long long>(unsigned long long);

}  // namespace detail

// ======================= Logger 与全局接口实现 =========================

LogLevel Logger::global_level_ = LogLevel::kInfo;

Logger::Logger(const char* file, int line, LogLevel level)
    : file_(file), line_(line), level_(level) {}

Logger::~Logger() {
  // 构造前缀：时间戳 + 级别 + tid + 文件:行
  char time_buf[32];
  int ms = 0;
  FormatCurrentTime(time_buf, sizeof(time_buf), &ms);

  int tid_len = 0;
  const char* tid_str = GetTidString(&tid_len);

  std::string line;
  line.reserve(128 + stream_.size());

  line.append(time_buf);
  line.push_back(' ');
  line.push_back('[');
  line.append(LevelName(level_));
  line.push_back(']');
  line.push_back(' ');
  line.append(tid_str, static_cast<std::size_t>(tid_len));
  line.push_back(' ');
  line.append(file_);
  line.push_back(':');

  char buf[32];
  int len = std::snprintf(buf, sizeof(buf), "%d", line_);
  line.append(buf, static_cast<std::size_t>(len));
  line.push_back(' ');

  line.append(stream_.data(), static_cast<std::size_t>(stream_.size()));

  LogBackend::Instance().Append(std::move(line));
}

void InitLogging(const LoggingConfig& config) {
  g_config = config;
  // 覆盖 Logger 静态级别到原子变量
  Logger::set_level(Logger::level());
  if (g_config.async) {
    LogBackend::Instance().Configure(g_config);
    LogBackend::Instance().Start();
  }
}

void ShutdownLogging() {
  LogBackend::Instance().Stop();
}

}  // namespace netx