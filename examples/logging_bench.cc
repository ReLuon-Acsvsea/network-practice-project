#include "netx/logging.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{

    struct Options
    {
        std::string mode = "mpmc";          // sync | blocking | mpmc
        std::string log_file = "/dev/null"; // 默认写到 /dev/null，避免磁盘瓶颈
        int threads = 4;
        std::uint64_t messages_per_thread = 1'000'000;
    };

    void PrintUsage(const char *prog)
    {
        std::fprintf(stderr,
                     "Usage: %s [--mode sync|blocking|mpmc] [--threads N] "
                     "[--messages-per-thread N] [--log-file PATH]\n",
                     prog);
    }

    bool ParseArgs(int argc, char *argv[], Options *opts)
    {
        for (int i = 1; i < argc; ++i)
        {
            const char *arg = argv[i];
            if (std::strcmp(arg, "--mode") == 0 && i + 1 < argc)
            {
                opts->mode = argv[++i];
            }
            else if (std::strcmp(arg, "--threads") == 0 && i + 1 < argc)
            {
                opts->threads = std::atoi(argv[++i]);
            }
            else if (std::strcmp(arg, "--messages-per-thread") == 0 &&
                     i + 1 < argc)
            {
                opts->messages_per_thread =
                    static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
            }
            else if (std::strcmp(arg, "--log-file") == 0 && i + 1 < argc)
            {
                opts->log_file = argv[++i];
            }
            else if (std::strcmp(arg, "--help") == 0 ||
                     std::strcmp(arg, "-h") == 0)
            {
                PrintUsage(argv[0]);
                return false;
            }
            else
            {
                std::fprintf(stderr, "Unknown argument: %s\n", arg);
                PrintUsage(argv[0]);
                return false;
            }
        }
        if (opts->threads <= 0 || opts->messages_per_thread == 0)
        {
            std::fprintf(stderr, "threads must be > 0 and messages-per-thread > 0\n");
            return false;
        }
        if (opts->mode != "sync" && opts->mode != "blocking" &&
            opts->mode != "mpmc")
        {
            std::fprintf(stderr, "Invalid mode: %s (expected sync|blocking|mpmc)\n",
                         opts->mode.c_str());
            return false;
        }
        return true;
    }

    netx::LoggingConfig MakeConfig(const Options &opts)
    {
        netx::LoggingConfig cfg;
        if (opts.mode == "sync")
        {
            cfg.async = false;
        }
        else
        {
            cfg.async = true;
            cfg.queue_type = (opts.mode == "blocking")
                                 ? netx::LogQueueType::kBlocking
                                 : netx::LogQueueType::kMPMC;
            cfg.queue_capacity = 1 << 14; // 16384
        }
        cfg.log_file = opts.log_file.c_str();
        cfg.flush_interval_ms = 200;
        return cfg;
    }

    void WorkerBody(int id, std::uint64_t messages)
    {
        for (std::uint64_t i = 0; i < messages; ++i)
        {
            // 日志内容保持简单，避免格式化本身成为瓶颈
            LOG_INFO << "bench thread=" << id << " i=" << i;
        }
    }

} // namespace

int main(int argc, char *argv[])
{
    Options opts;
    if (!ParseArgs(argc, argv, &opts))
    {
        return EXIT_FAILURE;
    }

    std::printf("netx logging benchmark\n");
    std::printf("  mode=%s\n", opts.mode.c_str());
    std::printf("  threads=%d\n", opts.threads);
    std::printf("  messages-per-thread=%llu\n",
                static_cast<unsigned long long>(opts.messages_per_thread));
    std::printf("  log-file=%s\n", opts.log_file.c_str());

    netx::LoggingConfig cfg = MakeConfig(opts);
    netx::InitLogging(cfg);

    const std::uint64_t total_messages =
        static_cast<std::uint64_t>(opts.threads) * opts.messages_per_thread;

    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(opts.threads));
    for (int i = 0; i < opts.threads; ++i)
    {
        workers.emplace_back(WorkerBody, i, opts.messages_per_thread);
    }
    for (auto &t : workers)
    {
        t.join();
    }

    netx::ShutdownLogging();

    auto end = std::chrono::steady_clock::now();
    double seconds =
        std::chrono::duration<double>(end - start).count();

    double mps = total_messages / seconds;

    std::printf("\nResult:\n");
    std::printf("  total-messages=%llu\n",
                static_cast<unsigned long long>(total_messages));
    std::printf("  duration=%.6f seconds\n", seconds);
    std::printf("  throughput=%.2f messages/second\n", mps);

    return EXIT_SUCCESS;
}
