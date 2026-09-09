#ifndef NETX_BUFFER_H_
#define NETX_BUFFER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

namespace netx
{

    // Buffer 是一个用于网络读写的可变大小缓冲区，支持追加、读取与零拷贝视图
    class Buffer
    {
    public:
        // initial：初始分配的容量（字节）
        explicit Buffer(size_t initial = 16 * 1024);
        // non-copyable
        Buffer(const Buffer &) = delete;
        Buffer &operator=(const Buffer &) = delete;
        // 析构 Buffer，释放底层内存（包括可能的内存池块）
        ~Buffer();

        // 当前可读字节数（[reader_index_, writer_index_)）
        size_t readable_bytes() const { return writer_index_ - reader_index_; }
        // 当前可写空间大小（[writer_index_, capacity)）
        size_t writable_bytes() const
        {
            size_t cap = pool_ptr_ ? pool_capacity_ : heap_buf_.size();
            return cap - writer_index_;
        }
        // 预留可前置区域大小（[0, reader_index_)）
        size_t prependable_bytes() const { return reader_index_; }

        // 读取指针：指向第一个未读字节
        const char *peek() const { return begin() + reader_index_; }
        // 写入指针（可写地址）
        char *begin_write() { return begin() + writer_index_; }
        // 写入指针（const 版本）
        const char *begin_write() const { return begin() + writer_index_; }

        // 从缓冲区读取 len 字节（前移 reader_index_）
        void retrieve(size_t len);
        // 清空缓冲区（reader_index_/writer_index_ 复位）
        void retrieve_all();
        // 将一段内存数据追加到缓冲区尾部
        void append(const void *data, size_t len);
        // 将字符串视图内容追加到缓冲区尾部
        void append(std::string_view sv) { append(sv.data(), sv.size()); }

        // 确保至少还可以写入 len 字节，不足则扩容或整理空间
        void ensure_writable(size_t len);

        // 从 fd 读取数据到缓冲区尾部，返回实际读取字节数，错误码写入 saved_errno
        ssize_t read_fd(int fd, int *saved_errno);

        // Pool helper: Buffer will try to acquire a pooled backing store when
        // constructed. The pool uses fixed-size blocks; if more space is needed
        // the Buffer will transparently migrate to a heap-backed buffer.

    private:
        // 返回缓冲区起始地址（优先使用线程局部内存池，否则使用堆内存）
        char *begin() { return pool_ptr_ ? pool_ptr_ : heap_buf_.data(); }
        // 返回缓冲区起始地址（const 版本）
        const char *begin() const { return pool_ptr_ ? pool_ptr_ : heap_buf_.data(); }
        // 为即将写入的 len 字节腾出空间（可能触发移动或扩容）
        void make_space(size_t len);

        // 堆上分配的缓冲区，当没有使用内存池或空间不足时使用
        std::vector<char> heap_buf_;
        // If non-null, buffer is backed by a pooled slab (fixed size) managed per-thread.
        // 指向线程局部内存池中分配的固定大小缓冲区，为 nullptr 时表示使用 heap_buf_
        char *pool_ptr_ = nullptr;
        // 内存池缓冲区的总容量
        size_t pool_capacity_ = 0;
        // 当前读指针索引
        size_t reader_index_ = 0;
        // 当前写指针索引
        size_t writer_index_ = 0;
    };

} // namespace netx

#endif // NETX_BUFFER_H_
