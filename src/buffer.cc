#include "netx/buffer.h"

#include <sys/uio.h>
#include <unistd.h>
#include <cstring>
#include <mutex>

// Optimization: Prefetch for write index
#if defined(__GNUC__) || defined(__clang__)
#define NETX_PREFETCH(addr) __builtin_prefetch(addr)
#define NETX_LIKELY(x) __builtin_expect(!!(x), 1)
#define NETX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NETX_PREFETCH(addr)
#define NETX_LIKELY(x) (x)
#define NETX_UNLIKELY(x) (x)
#endif

namespace {
// default pool parameters
static const size_t kDefaultPoolBlock = 16 * 1024;
static const size_t kDefaultPoolCount = 256;
} // namespace

namespace netx {

static const size_t kPrepend = 8;

// 简单的固定块缓冲池：返回原始内存块指针（kPrepend + block_size_）
// 用于减少小对象的内存分配开销
class BufferPool {
 public:
  // 构造函数：预分配指定数量的内存块
  BufferPool(size_t block_size, size_t pool_count)
      : block_size_(block_size) {
    for (size_t i = 0; i < pool_count; ++i) {
      pool_.push_back(new char[kPrepend + block_size_]);
    }
  }

  // 析构函数：释放所有预分配的内存块
  ~BufferPool() {
    for (auto p : pool_) delete[] p;
    pool_.clear();
  }

  size_t block_size() const { return block_size_; }

  // 获取一个内存块：从池中取出或新分配
  char* Acquire() {
    char* raw = nullptr;
    if (NETX_LIKELY(!pool_.empty())) {
      raw = pool_.back();
      pool_.pop_back();
    }
    if (NETX_UNLIKELY(!raw)) raw = new char[kPrepend + block_size_];
    return raw;
  }

  // 归还内存块到池中
  void Release(char* p) { if (p) pool_.push_back(p); }

 private:
  size_t block_size_;
  std::vector<char*> pool_;
};

// 每线程局部池：避免跨线程竞争
thread_local BufferPool g_tls_pool(kDefaultPoolBlock, kDefaultPoolCount);

// 构造函数：优先从线程局部池获取内存，超大容量则使用堆分配
Buffer::Buffer(size_t initial) : reader_index_(kPrepend), writer_index_(kPrepend) {
  if (NETX_LIKELY(initial <= g_tls_pool.block_size())) {
    pool_ptr_ = g_tls_pool.Acquire();
    pool_capacity_ = kPrepend + g_tls_pool.block_size();
  } else {
    heap_buf_.resize(kPrepend + initial);
  }
}

// 析构函数：归还池内存
Buffer::~Buffer() {
  if (pool_ptr_) {
    g_tls_pool.Release(pool_ptr_);
    pool_ptr_ = nullptr;
    pool_capacity_ = 0;
  }
}

// 消费指定长度的数据
void Buffer::retrieve(size_t len) {
  if (len < readable_bytes()) {
    reader_index_ += len;
  } else {
    retrieve_all();
  }
}

// 消费所有可读数据，重置读写索引
void Buffer::retrieve_all() { reader_index_ = writer_index_ = kPrepend; }

// 追加数据到缓冲区
void Buffer::append(const void* data, size_t len) {
  ensure_writable(len);
  std::memcpy(begin_write(), data, len);
  writer_index_ += len;
}

// 确保可写空间足够
void Buffer::ensure_writable(size_t len) {
  if (writable_bytes() < len) make_space(len);
}

// 扩展可写空间：优先移动数据，必要时扩容
void Buffer::make_space(size_t len) {
  size_t writable = writable_bytes();
  size_t prependable = prependable_bytes();
  // writable: 当前可写空间
  // prependable: 已读空间（reader_index_ 前面的部分，可以回收利用）
  // kPrepend: 预留的 8 字节（用于协议头，不能动）
  // 可用空间 = 可写空间 + 已读空间 - 预留空间
  if (writable + prependable - kPrepend < len) {
    // 整理后也不够，必须扩容
    if (pool_ptr_) { // 当前用的是线程局部池（16KB固定大小），空间不足，迁移到堆
      size_t readable = readable_bytes();

      // 第1步：堆上分配新空间 = 预留(8字节) + 已有数据 + 需要的新空间
      heap_buf_.resize(kPrepend + readable + len);

      // 第2步：把可读数据从池拷贝到堆（peek()指向池内存，heap_buf_.data()指向堆内存）
      std::memcpy(heap_buf_.data() + kPrepend, peek(), readable);

      // 第3步：重置读写指针，数据从 kPrepend 位置开始
      reader_index_ = kPrepend;
      writer_index_ = reader_index_ + readable;
      
      // 第4步：归还池内存，下次其他 Buffer 可以复用；pool_ptr_ 置空，后续用 heap_buf_
      g_tls_pool.Release(pool_ptr_);
      pool_ptr_ = nullptr;
      pool_capacity_ = 0;
    } else {
      // 当前用的是堆，直接扩容 vector
      heap_buf_.resize(writer_index_ + len);
    }
  } else {
    // 整理就够了：把可读数据挪到 kPrepend 位置，回收已读空间
    size_t readable = readable_bytes();
    // memmove 可处理重叠区域（源和目的可能重叠）
    std::memmove(begin() + kPrepend, peek(), readable);
    reader_index_ = kPrepend;
    writer_index_ = reader_index_ + readable;
  }
}

// 从文件描述符读取数据：使用 readv 优化，避免频繁扩容
ssize_t Buffer::read_fd(int fd, int* saved_errno) {
  char extrabuf[65536];
  struct iovec iov[2];
  const size_t writable = writable_bytes();
  iov[0].iov_base = begin_write();
  iov[0].iov_len = writable;
  iov[1].iov_base = extrabuf;
  iov[1].iov_len = sizeof(extrabuf);
  const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
  const ssize_t n = ::readv(fd, iov, iovcnt);
  if (n < 0) {
    *saved_errno = errno;
    return n;
  } else if (static_cast<size_t>(n) <= writable) {
    writer_index_ += n;
  } else {
    size_t cap = pool_ptr_ ? pool_capacity_ : heap_buf_.size();
    writer_index_ = cap;
    append(extrabuf, n - writable);
  }
  return n;
}

}  // namespace netx
