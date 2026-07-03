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
    class Buffer
    {
    public:
        explicit Buffer(size_t initial = 16*1024);

        Buffer(const Buffer &)=delete;
        Buffer &operator=(const Buffer &) = delete;
        ~Buffer();

        size_t readable_bytes() const {return writer_index_-reader_index_;}
        size_t writeable_bytes() const {
            size_t cap =pool_ptr_?pool_capacity_:heap_buf_.size();
            return pool_capacity_-writer_index_;
        }
        size_t prependable_bytes() const {return reader_index_;}
        const char*peek() const {return begin()+reader_index_;}//指向第一个未读字节
        char *begin_write(){return begin()+writer_index_;}
        const char *begin_write() const {return begin()+writer_index_;}

    private:
        char *begin(){return pool_ptr_?pool_ptr_:heap_buf_.data();}
        const char *begin()const{return pool_ptr_?pool_ptr_:heap_buf_.data();}
        //get more space(may go to move/larger)
        void make_space(size_t len);
        std::vector<char> heap_buf_;
        char * pool_ptr_=nullptr;
        size_t pool_capacity_=0;
        size_t reader_index_ =0;
        size_t writer_index_=0;
    };
}

#endif