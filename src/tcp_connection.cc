#include "netx/tcp_connection.h"
#include "netx/channel.h"
#include "netx/event_loop.h"
#include "netx/socket.h"
#include "netx/logging.h"

#include <sys/socket.h>
#include <unistd.h>
#include <sys/uio.h>
namespace netx{
TcpConnection::TcpConnection(EventLoop *loop, int fd)
: loop_(loop), socket_(new Socket(fd)), channel_(new Channel(loop, fd)),
      input_(), output_(), state_(State::kConnected), context_(){
    channel_->set_read_callback([this]() { this->HandleRead(); });
    channel_->set_write_callback([this]() { this->HandleWrite(); });
    channel_->set_close_callback([this]() { this->HandleClose(); });
    channel_->set_error_callback([this]() { this->HandleError(); });
    channel_->enable_reading();
}
TcpConnection::~TcpConnection(){}
int TcpConnection::fd() const{return socket_->fd();}
bool TcpConnection::IsConnected() const { return state_ == State::kConnected; }
void TcpConnection::set_context(std::any ctx) { context_ = std::move(ctx); }
void TcpConnection::Send(const std::string &s) {
  if (loop_->IsInLoopThread()) {
    SendInLoop(s.data(), s.size());
  } else {
    auto self = shared_from_this();
    std::string copy = s;
    loop_->QueueInLoop([self, data = std::move(copy)]() mutable {
      self->SendInLoop(data.data(), data.size());
    });
  }
}

void TcpConnection::Send(std::string &&s) {
  if (loop_->IsInLoopThread()) {
    SendInLoop(s.data(), s.size());
  } else {
    auto self = shared_from_this();
    std::string payload = std::move(s);
    loop_->QueueInLoop([self, data = std::move(payload)]() mutable {
      self->SendInLoop(data.data(), data.size());
    });
  }
}

void TcpConnection::Send(const char *data, size_t len) {
  if (loop_->IsInLoopThread()) {
    SendInLoop(data, len);
  } else {
    auto self = shared_from_this();
    std::string copy(data, len);
    loop_->QueueInLoop([self, payload = std::move(copy)]() mutable {
      self->SendInLoop(payload.data(), payload.size());
    });
  }
}

void TcpConnection::SendVec(const char *data1, size_t len1, const char *data2,
                            size_t len2) {
  if (loop_->IsInLoopThread()) {
    SendVecInLoop(data1, len1, data2, len2);
  } else {
    auto self = shared_from_this();
    std::string header(data1, len1);
    std::string body(data2, len2);
    loop_->QueueInLoop(
        [self, h = std::move(header), b = std::move(body)]() mutable {
          self->SendVecInLoop(h.data(), h.size(), b.data(), b.size());
        });
  }
}

void TcpConnection::Shutdown() { ::shutdown(fd(), SHUT_WR); }

void TcpConnection::Tie(const std::shared_ptr<TcpConnection> &self) {
  channel_->tie(self);
}

// 处理可读事件：读取数据并回调上层
void TcpConnection::HandleRead() {
  int saved = 0;
  ssize_t n = input_.read_fd(fd(), &saved);
  LOG_DEBUG << "conn fd=" << fd() << " read n=" << n << " errno=" << saved;
  if (n > 0) {
    auto self = shared_from_this();
    // 优先使用轻量级函数指针回调，避免经过 std::function
    if (raw_msg_fn_) {
      raw_msg_fn_(raw_msg_ctx_, self, &input_);
    } else if (message_cb_) {
      message_cb_(self, &input_);
    }
  } else if (n == 0) {
    HandleClose();
  } else {
    if (saved != EAGAIN && saved != EWOULDBLOCK) {
      HandleError();
    }
  }
}

// 处理可写事件：发送输出缓冲区数据（水平触发模式）
void TcpConnection::HandleWrite() {
  if (!writing_)
    return;
  
  // Level Triggered Mode: Write once. If incomplete, EPOLLOUT will trigger again.
  const size_t to_write = output_.readable_bytes();
  ssize_t n = ::write(fd(), output_.peek(), to_write);
  LOG_DEBUG << "conn fd=" << fd() << " write try=" << to_write << " n=" << n;
  if (n > 0) {
    output_.retrieve(static_cast<size_t>(n));
    if (output_.readable_bytes() == 0) {
      writing_ = false;
      channel_->disable_writing();
    }
  } else {
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            HandleError();
        }
    }
  }
}

// 处理连接关闭：清理资源并回调上层
void TcpConnection::HandleClose() {
  if (state_ == State::kDisconnected) return;
  state_ = State::kDisconnected;
  channel_->remove();
  writing_ = false;
  auto self = shared_from_this();
  if (raw_close_fn_) {
    raw_close_fn_(raw_close_ctx_, self);
  } else if (close_cb_) {
    close_cb_(self);
  }
}

// 处理错误：直接关闭连接
void TcpConnection::HandleError() { HandleClose(); }

// 在事件循环中发送数据：优先直接写，未完成则入缓冲区
void TcpConnection::SendInLoop(const char *data, size_t len) {
  const size_t out_len = output_.readable_bytes();
  if (!writing_ && out_len == 0) {
    // 快路径：无待发送数据，直接写入
    ssize_t n = ::write(fd(), data, len);
    if (n < 0)
      n = 0;
    if (static_cast<size_t>(n) < len) {
      output_.append(data + n, len - n);
      writing_ = true;
      channel_->enable_writing();
    }
  } else {
    // 聚合写：将已有待发送数据与本次数据一起 writev，减少内存拷贝
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char *>(output_.peek());
    iov[0].iov_len = out_len;
    iov[1].iov_base = const_cast<char *>(data);
    iov[1].iov_len = len;
    ssize_t n = ::writev(fd(), iov, (out_len > 0 ? 2 : 1));
    if (n < 0)
      n = 0;
    size_t written = static_cast<size_t>(n);
    if (written < out_len) {
      // 仅写出部分旧数据，新数据全部入缓冲
      output_.retrieve(written);//去掉已写的
      output_.append(data, len);//新数据全部 append 到 output_
      writing_ = true;
      channel_->enable_writing();
    } else {
      // 旧数据写完，消耗新数据的剩余部分
      size_t consumed_new = written - out_len;
      if (consumed_new < len) {
        output_.append(data + consumed_new, len - consumed_new);
        writing_ = true;
        channel_->enable_writing();
      } else {
        // 全部写完，若无待发送则关闭写关注
        if (output_.readable_bytes() == 0) {
          writing_ = false;
          channel_->disable_writing();
        }
      }
    }
  }
}

// 在事件循环中发送两段数据：优先 writev 直接写
void TcpConnection::SendVecInLoop(const char *data1, size_t len1,
                                  const char *data2, size_t len2) {
  const size_t out_len = output_.readable_bytes();
  if (!writing_ && out_len == 0) {
    // 直接 writev 两段
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char *>(data1);
    iov[0].iov_len = len1;
    iov[1].iov_base = const_cast<char *>(data2);
    iov[1].iov_len = len2;
    ssize_t n = ::writev(fd(), iov, (len2 > 0 ? 2 : 1));
    if (n < 0)
      n = 0;
    size_t written = static_cast<size_t>(n);
    if (written < len1) {
      // 只写出部分 header
      output_.append(data1 + written, len1 - written);
      output_.append(data2, len2);
      writing_ = true;
      channel_->enable_writing();
    } else {
      // header 全部写完，处理 body 余量
      size_t consumed_body = written - len1;
      if (consumed_body < len2) {
        output_.append(data2 + consumed_body, len2 - consumed_body);
        writing_ = true;
        channel_->enable_writing();
      } else {
        if (output_.readable_bytes() == 0) {
          writing_ = false;
          channel_->disable_writing();
        }
      }
    }
  } else {
    // 已有待发送数据：聚合 3 段 writev（output_, data1, data2）
    struct iovec iov[3];
    iov[0].iov_base = const_cast<char *>(output_.peek());
    iov[0].iov_len = out_len;
    iov[1].iov_base = const_cast<char *>(data1);
    iov[1].iov_len = len1;
    iov[2].iov_base = const_cast<char *>(data2);
    iov[2].iov_len = len2;
    int iovcnt = 1 + (len1 > 0 ? 1 : 0) + (len2 > 0 ? 1 : 0);
    ssize_t n = ::writev(fd(), iov, iovcnt);
    if (n < 0)
      n = 0;
    size_t written = static_cast<size_t>(n);
    if (written < out_len) {
      // 旧数据未写完：新数据全部入缓冲
      output_.retrieve(written);
      output_.append(data1, len1);
      output_.append(data2, len2);
      writing_ = true;
      channel_->enable_writing();
    } else {
      // 旧数据写完，消耗新数据
      size_t remain = written - out_len;
      if (remain < len1) {
        // header 只写了部分
        output_.append(data1 + remain, len1 - remain);
        output_.append(data2, len2);
        writing_ = true;
        channel_->enable_writing();
      } else {
        size_t consumed_body = remain - len1;
        if (consumed_body < len2) {
          output_.append(data2 + consumed_body, len2 - consumed_body);
          writing_ = true;
          channel_->enable_writing();
        } else {
          if (output_.readable_bytes() == 0) {
            writing_ = false;
            channel_->disable_writing();
          }
        }
      }
    }
  }
}
}