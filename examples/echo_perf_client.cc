#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

struct Options {
  std::string host = "127.0.0.1";
  int port = 8081;
  int connections = 100;
  int duration = 10;
  int payload_size = 1024;
  int threads = 1;
};

void PrintUsage(const char* prog) {
  std::cerr << "Usage: " << prog
            << " [--host <host>] [--port <port>] [--connections <n>]\n"
               "             [--duration <seconds>] [--size <bytes>] [--threads "
               "<n>]\n";
}

bool ParseArgs(int argc, char* argv[], Options* opts) {
  static struct option long_options[] = {
      {"host", required_argument, nullptr, 'H'},
      {"port", required_argument, nullptr, 'P'},
      {"connections", required_argument, nullptr, 'c'},
      {"duration", required_argument, nullptr, 'd'},
      {"size", required_argument, nullptr, 's'},
      {"threads", required_argument, nullptr, 't'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};

  while (true) {
    int option_index = 0;
    int c = getopt_long(argc, argv, "", long_options, &option_index);
    if (c == -1) break;
    switch (c) {
      case 'H':
        opts->host = optarg;
        break;
      case 'P':
        opts->port = std::stoi(optarg);
        break;
      case 'c':
        opts->connections = std::stoi(optarg);
        break;
      case 'd':
        opts->duration = std::stoi(optarg);
        break;
      case 's':
        opts->payload_size = std::stoi(optarg);
        break;
      case 't':
        opts->threads = std::stoi(optarg);
        break;
      case 'h':
      default:
        PrintUsage(argv[0]);
        return false;
    }
  }
  return true;
}

struct Endpoint {
  sockaddr_storage addr{};
  socklen_t addr_len = 0;
};

std::vector<Endpoint> ResolveEndpoints(const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_ADDRCONFIG;

  char port_str[16];
  std::snprintf(port_str, sizeof(port_str), "%d", port);

  addrinfo* result = nullptr;
  int ret = getaddrinfo(host.c_str(), port_str, &hints, &result);
  if (ret != 0) {
    throw std::runtime_error(std::string("getaddrinfo: ") + gai_strerror(ret));
  }

  std::vector<Endpoint> endpoints;
  for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    Endpoint ep;
    std::memcpy(&ep.addr, rp->ai_addr, rp->ai_addrlen);
    ep.addr_len = static_cast<socklen_t>(rp->ai_addrlen);
    endpoints.push_back(ep);
  }
  freeaddrinfo(result);

  if (endpoints.empty()) {
    throw std::runtime_error("resolve_host: empty endpoint list");
  }
  return endpoints;
}

int SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) return -1;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
  return 0;
}

int SetCloseOnExec(int fd) {
  int flags = fcntl(fd, F_GETFD, 0);
  if (flags == -1) return -1;
  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) return -1;
  return 0;
}

void SetTcpNoDelay(int fd) {
  int yes = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

struct ThreadStats {
  std::uint64_t requests = 0;
  long double total_latency_ms = 0.0;
  double max_latency_ms = 0.0;
  double min_latency_ms = std::numeric_limits<double>::infinity();
  std::uint64_t connect_failures = 0;
};

struct Connection {
  int fd = -1;
  bool alive = false;
  bool connected = false;
  bool awaiting_reply = false;
  std::size_t send_offset = 0;
  std::size_t recv_offset = 0;
  std::chrono::steady_clock::time_point last_send;
};

struct WorkerArgs {
  int worker_id = 0;
  int connection_count = 0;
  int duration_sec = 0;
  const std::vector<Endpoint>* endpoints = nullptr;
  const std::string* payload = nullptr;
};

void CloseConnection(Connection& conn, int epfd, int& active_connections,
                     int& inflight) {
  if (!conn.alive) return;
  epoll_ctl(epfd, EPOLL_CTL_DEL, conn.fd, nullptr);
  ::close(conn.fd);
  conn.alive = false;
  --active_connections;
  if (conn.awaiting_reply && inflight > 0) {
    --inflight;
  }
}

bool PrepareConnection(Connection& conn, const std::vector<Endpoint>& eps,
                       ThreadStats& stats) {
  for (const auto& ep : eps) {
    int fd = ::socket(ep.addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) continue;
    if (SetNonBlocking(fd) == -1 || SetCloseOnExec(fd) == -1) {
      ::close(fd);
      continue;
    }
    SetTcpNoDelay(fd);

    int ret =
        ::connect(fd, reinterpret_cast<const sockaddr*>(&ep.addr), ep.addr_len);
    if (ret == 0 || (ret == -1 && errno == EINPROGRESS)) {
      conn.fd = fd;
      conn.alive = true;
      conn.connected = (ret == 0);
      conn.awaiting_reply = false;
      conn.send_offset = 0;
      conn.recv_offset = 0;
      return true;
    }
    ::close(fd);
  }
  ++stats.connect_failures;
  return false;
}

bool TrySend(Connection& conn, const std::string& payload, bool allow_new_send,
             std::chrono::steady_clock::time_point now) {
  if (!conn.alive || conn.awaiting_reply || !conn.connected ||
      !allow_new_send) {
    return true;
  }

  const char* data = payload.data();
  const std::size_t total = payload.size();
  while (conn.send_offset < total) {
    ssize_t n = ::send(conn.fd, data + conn.send_offset, total - conn.send_offset,
                       MSG_NOSIGNAL);
    if (n > 0) {
      conn.send_offset += static_cast<std::size_t>(n);
    } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    } else {
      return false;
    }
  }

  conn.send_offset = 0;
  conn.recv_offset = 0;
  conn.awaiting_reply = true;
  conn.last_send = now;
  return true;
}

bool TryReceive(Connection& conn, const std::string& payload,
                ThreadStats& stats, int& inflight,
                std::chrono::steady_clock::time_point now) {
  if (!conn.alive || !conn.awaiting_reply || !conn.connected) return true;

  constexpr std::size_t kChunkSize = 64 * 1024;
  char buffer[kChunkSize];
  const std::size_t total = payload.size();

  while (conn.recv_offset < total) {
    std::size_t need = total - conn.recv_offset;
    std::size_t chunk = need < kChunkSize ? need : kChunkSize;
    ssize_t n = ::recv(conn.fd, buffer, chunk, 0);
    if (n > 0) {
      conn.recv_offset += static_cast<std::size_t>(n);
    } else if (n == 0) {
      return false;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    } else {
      return false;
    }
  }

  if (conn.recv_offset == total) {
    const auto latency =
        std::chrono::duration<double, std::milli>(now - conn.last_send).count();
    ++stats.requests;
    stats.total_latency_ms += latency;
    if (latency > stats.max_latency_ms) stats.max_latency_ms = latency;
    if (latency < stats.min_latency_ms) stats.min_latency_ms = latency;
    if (conn.awaiting_reply && inflight > 0) --inflight;
    conn.awaiting_reply = false;
    conn.recv_offset = 0;
  }

  return true;
}

void WorkerMain(WorkerArgs args, ThreadStats* stats) {
  if (args.connection_count <= 0) return;
  ThreadStats local_stats;
  const auto& payload = *args.payload;

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd == -1) {
    std::perror("epoll_create1");
    return;
  }

  std::vector<Connection> connections(args.connection_count);
  int active = 0;
  int inflight = 0;

  for (auto& conn : connections) {
    if (!PrepareConnection(conn, *args.endpoints, local_stats)) continue;
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.ptr = &conn;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn.fd, &ev) == -1) {
      std::perror("epoll_ctl add");
      CloseConnection(conn, epfd, active, inflight);
      continue;
    }
    ++active;
    if (conn.connected) {
      auto now = std::chrono::steady_clock::now();
      bool before = conn.awaiting_reply;
      if (!TrySend(conn, payload, true, now)) {
        CloseConnection(conn, epfd, active, inflight);
      } else if (!before && conn.awaiting_reply) {
        ++inflight;
      }
    }
  }

  auto stop_time =
      std::chrono::steady_clock::now() + std::chrono::seconds(args.duration_sec);

  const int max_events = std::max(32, args.connection_count);
  std::vector<epoll_event> events(static_cast<std::size_t>(max_events));

  while (active > 0) {
    bool allow_before_wait = std::chrono::steady_clock::now() < stop_time;

    int timeout_ms = allow_before_wait ? 100 : 200;
    int n = epoll_wait(epfd, events.data(), max_events, timeout_ms);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::perror("epoll_wait");
      break;
    }

    auto now = std::chrono::steady_clock::now();
    bool allow_new_send = now < stop_time;
    if (n == 0) {
      if (!allow_new_send && inflight == 0) break;
      continue;
    }

    for (int i = 0; i < n; ++i) {
      auto* conn = static_cast<Connection*>(events[i].data.ptr);
      if (!conn || !conn->alive) continue;

      uint32_t ev = events[i].events;
      if (ev & (EPOLLERR | EPOLLHUP)) {
        CloseConnection(*conn, epfd, active, inflight);
        continue;
      }

      if (!conn->connected) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1 ||
            err != 0) {
          CloseConnection(*conn, epfd, active, inflight);
          continue;
        }
        conn->connected = true;
      }

      if (ev & EPOLLIN) {
        if (!TryReceive(*conn, payload, local_stats, inflight, now)) {
          CloseConnection(*conn, epfd, active, inflight);
          continue;
        }
        if (!conn->awaiting_reply && allow_new_send) {
          bool before = conn->awaiting_reply;
          if (!TrySend(*conn, payload, true, now)) {
            CloseConnection(*conn, epfd, active, inflight);
          } else if (!before && conn->awaiting_reply) {
            ++inflight;
          }
        }
      }

      if ((ev & EPOLLOUT) && conn->alive) {
        bool before = conn->awaiting_reply;
        bool ok = TrySend(*conn, payload, allow_new_send, now);
        if (!ok) {
          CloseConnection(*conn, epfd, active, inflight);
          continue;
        }
        if (!before && conn->awaiting_reply) {
          ++inflight;
        }
      }
    }

    if (!allow_new_send && inflight == 0) break;
  }

  for (auto& conn : connections) {
    if (conn.alive) {
      CloseConnection(conn, epfd, active, inflight);
    }
  }

  ::close(epfd);
  *stats = local_stats;
}

}  // namespace

int main(int argc, char* argv[]) {
  Options opts;
  if (!ParseArgs(argc, argv, &opts)) {
    return EXIT_FAILURE;
  }

  if (opts.connections <= 0 || opts.duration <= 0 || opts.payload_size <= 0 ||
      opts.threads <= 0) {
    std::cerr << "Error: all numeric parameters must be positive integers.\n";
    return EXIT_FAILURE;
  }

  std::cout << "Starting Echo Performance Test (C++ client)\n"
            << "  Target: " << opts.host << ":" << opts.port << "\n"
            << "  Connections: " << opts.connections << "\n"
            << "  Duration: " << opts.duration << " seconds\n"
            << "  Message Size: " << opts.payload_size << " bytes\n"
            << "  Threads: " << opts.threads << "\n"
            << "----------------------------------------\n";

  std::vector<Endpoint> endpoints;
  try {
    endpoints = ResolveEndpoints(opts.host, opts.port);
  } catch (const std::exception& ex) {
    std::cerr << "Failed to resolve host: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }

  std::string payload(static_cast<std::size_t>(opts.payload_size), 'A');

  const int base = opts.connections / opts.threads;
  const int rem = opts.connections % opts.threads;

  std::vector<std::thread> workers;
  std::vector<ThreadStats> stats(
      static_cast<std::size_t>(opts.threads));

  auto test_start = std::chrono::steady_clock::now();

  for (int i = 0; i < opts.threads; ++i) {
    int conn_i = base + (i < rem ? 1 : 0);
    if (conn_i <= 0) continue;
    WorkerArgs args{
        .worker_id = i,
        .connection_count = conn_i,
        .duration_sec = opts.duration,
        .endpoints = &endpoints,
        .payload = &payload,
    };
    workers.emplace_back(WorkerMain, args, &stats[static_cast<std::size_t>(i)]);
  }

  for (auto& t : workers) {
    if (t.joinable()) t.join();
  }

  auto test_end = std::chrono::steady_clock::now();
  double actual_duration =
      std::chrono::duration<double>(test_end - test_start).count();

  std::uint64_t total_requests = 0;
  long double total_latency = 0.0;
  double max_latency = 0.0;
  double min_latency = std::numeric_limits<double>::infinity();
  std::uint64_t connect_failures = 0;

  for (const auto& s : stats) {
    total_requests += s.requests;
    total_latency += s.total_latency_ms;
    if (s.max_latency_ms > max_latency) max_latency = s.max_latency_ms;
    if (s.min_latency_ms < min_latency) min_latency = s.min_latency_ms;
    connect_failures += s.connect_failures;
  }

  std::cout << "\n--- Performance Results ---\n";
  std::cout << "Total Test Duration: " << actual_duration << " seconds\n";
  std::cout << "Total Requests Completed: " << total_requests << "\n";
  if (connect_failures > 0) {
    std::cout << "Connection Failures: " << connect_failures << "\n";
  }

  if (total_requests == 0) {
    std::cout << "No successful requests were completed.\n";
    return EXIT_SUCCESS;
  }

  double avg_latency = static_cast<double>(total_latency / total_requests);
  double qps = total_requests / actual_duration;
  if (!std::isfinite(min_latency)) {
    min_latency = 0.0;
  }

  std::cout << "Throughput (QPS): " << qps << " requests/second\n";
  std::cout << "Average Latency: " << avg_latency << " ms\n";
  std::cout << "Max Latency: " << max_latency << " ms\n";
  std::cout << "Min Latency: " << min_latency << " ms\n";

  return EXIT_SUCCESS;
}


