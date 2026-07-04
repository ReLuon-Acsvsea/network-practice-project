#include "netx/buffer.h"
#include <cstring>
#include <sys/uio.h>
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

namespace netx{
static const size_t  kPrepend=8;
class BufferPool{
public:
BufferPool(size_t block_size, size_t pool_count)
:block_size_(block_size){
    for(size_t i=0;i<pool_count;++i){
        pool_.push_back(new char[kPrepend+block_size_]);
    }
}
~BufferPool(){
    for(auto p:pool_) delete[] p;
    pool_.clear();
}
size_t block_size() const {return block_size_;}

char * Acquire(){
    char* raw =nullptr;
    if(NETX_LIKELY(!pool_.empty())){
        raw = pool_.back();
        pool_.pop_back();
    }
    if (NETX_UNLIKELY(!raw)) raw = new char[kPrepend + block_size_];
    return raw;
}

void Release(char *p){
    if(p) pool_.push_back(p);
}

private:
size_t block_size_;
std::vector<char*> pool_;

};
thread_local BufferPool g_tls_pool(kDefaultPoolBlock,kDefaultPoolCount);
Buffer::Buffer(size_t initial)
:reader_index_(kPrepend),writer_index_(kPrepend){
    if(NETX_LIKELY(initial<=g_tls_pool.block_size())){
        pool_ptr_= g_tls_pool.Acquire();
        pool_capacity_=kPrepend+g_tls_pool.block_size();
    }else{
        heap_buf_.resize(kPrepend+initial);
    }
}
Buffer::~Buffer(){
    if(pool_ptr_){
        g_tls_pool.Release(pool_ptr_);
        pool_ptr_=nullptr;
        pool_capacity_=0;
    }
}
// 消费指定长度的数据
void Buffer::retrieve(size_t len){
    if(len<readable_bytes()){
        reader_index_+=len;
    }else{
        retrieve_all();
    }
}
// 消费所有可读数据，重置读写索引
void Buffer::retrieve_all(){
    reader_index_=writer_index_=kPrepend;
}

void Buffer::append(const void *data,size_t len){
    ensure_writable(len);
    std::memcpy(begin_write(),data,len);
    writer_index_+= len;
}

void Buffer::ensure_writable(size_t len){
    if(writeable_bytes()<len) make_space(len);
}

// 扩展可写空间：优先移动数据，必要时扩容
void Buffer::make_space(size_t len){
    size_t writeable = writeable_bytes();
    size_t prependable = prependable_bytes();//已读空间
    if(writeable+prependable-kPrepend < len){//必须扩容
        if(pool_ptr_)//当前使用池
        {
            size_t readable = readable_bytes();
            heap_buf_.resize(kPrepend + readable +len);
            //可读数据从池拷贝到堆
            std::memcpy(heap_buf_.data()+kPrepend,peek(),readable);
            reader_index_=kPrepend;
            writer_index_=reader_index_+readable;

            g_tls_pool.Release(pool_ptr_);
            pool_ptr_=nullptr;
            pool_capacity_=0;
        }else{//当前使用堆
            heap_buf_.resize(writer_index_+len);
        }
    }else{//整理空间
        size_t readable = readable_bytes();
        std::memmove(begin()+kPrepend,peek(),readable);
        reader_index_=kPrepend;
        writer_index_=reader_index_+readable;
    }
}

ssize_t Buffer::read_fd(int fd, int * saved_errno){
    char extrabuf[65536];
    struct iovec iov[2];
    const size_t writable = writeable_bytes();
    iov[0].iov_base= begin_write();
    iov[0].iov_len= writable;
    iov[1].iov_base=extrabuf;
    iov[1].iov_len=sizeof(extrabuf);
    const int iovcnt = (writable<sizeof(extrabuf))?2:1;
    const ssize_t n =::readv(fd, iov, iovcnt);
    if(n<0){
        *saved_errno = errno;
        return n;
    }else if(static_cast<size_t>(n)<=writable){
        writer_index_+=n;
    }else{
        size_t cap =pool_ptr_ ? pool_capacity_ : heap_buf_.size();
        writer_index_ =cap;
        append(extrabuf,n-writable);
    }
    return n;
}

}