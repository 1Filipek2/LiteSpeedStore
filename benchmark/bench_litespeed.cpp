#include <StorageEngine.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using Seconds = std::chrono::duration<double>;

static void printRow(const char* label, long long ops, double elapsed_s) {
    double throughput = static_cast<double>(ops) / elapsed_s;
    std::cout << std::left  << std::setw(42) << label
              << std::right << std::setw(9)  << ops        << " ops   "
              << std::setw(8) << std::fixed << std::setprecision(1) << elapsed_s * 1000.0 << " ms   "
              << std::setw(12) << std::fixed << std::setprecision(0) << throughput << " ops/s\n";
}

int main() {
    constexpr long long SINGLE_OPS    = 500'000LL;
    constexpr long long PER_THREAD    = 200'000LL;
    constexpr int       NUM_WRITERS   = 4;
    constexpr int       NUM_READERS   = 4;

    const std::string key   = "k";
    const std::string value = "hello";

    std::cout << "LiteSpeedStore Micro-Benchmark (in-memory, no WAL)\n";
    std::cout << std::string(80, '-') << '\n';
    std::cout << std::left << std::setw(42) << "Scenario"
              << std::right << std::setw(9) << "Ops" << "       "
              << std::setw(8) << "Time" << "     "
              << std::setw(12) << "Throughput" << '\n';
    std::cout << std::string(80, '-') << '\n';

    // 1. Single-threaded set()
    {
        auto db = StorageEngine::makeInMemory();
        auto t0 = Clock::now();
        for (long long i = 0; i < SINGLE_OPS; ++i)
            db.set(key, value, 1.0);
        double s = Seconds(Clock::now() - t0).count();
        printRow("set() single-thread", SINGLE_OPS, s);
    }

    // 2. Single-threaded get()
    {
        auto db = StorageEngine::makeInMemory();
        db.set(key, value, 1.0);
        auto t0 = Clock::now();
        for (long long i = 0; i < SINGLE_OPS; ++i)
            (void)db.get(key);
        double s = Seconds(Clock::now() - t0).count();
        printRow("get() single-thread", SINGLE_OPS, s);
    }

    // 3. Single-threaded getAverage()
    {
        auto db = StorageEngine::makeInMemory();
        for (long long i = 0; i < 100LL; ++i)
            db.set(key, value, static_cast<double>(i));
        auto t0 = Clock::now();
        for (long long i = 0; i < SINGLE_OPS; ++i)
            (void)db.getAverage(key);
        double s = Seconds(Clock::now() - t0).count();
        printRow("getAverage() single-thread (100-entry hist)", SINGLE_OPS, s);
    }

    // 4. Concurrent: 4 writers + 4 readers
    {
        auto db = StorageEngine::makeInMemory();
        db.set(key, value, 0.0);

        std::atomic<long long> totalOps{0};
        auto t0 = Clock::now();

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(NUM_WRITERS + NUM_READERS));

        for (int w = 0; w < NUM_WRITERS; ++w) {
            threads.emplace_back([&db, &totalOps, key, value]() {
                for (long long i = 0; i < PER_THREAD; ++i)
                    db.set(key, value, 1.0);
                totalOps += PER_THREAD;
            });
        }
        for (int r = 0; r < NUM_READERS; ++r) {
            threads.emplace_back([&db, &totalOps, key]() {
                for (long long i = 0; i < PER_THREAD; ++i)
                    (void)db.get(key);
                totalOps += PER_THREAD;
            });
        }

        for (auto& t : threads)
            t.join();

        double s = Seconds(Clock::now() - t0).count();
        printRow("mixed 4W+4R concurrent", totalOps.load(), s);
    }

    std::cout << std::string(80, '-') << '\n';

    // 5. WAL-backed set(): group-commit trade-off (time-capped, 500ms per run)
    std::cout << "\nWAL-backed set() — group-commit durability vs. throughput (500ms each)\n";
    std::cout << std::string(80, '-') << '\n';

    auto benchWAL = [&](size_t syncEveryN, const char* label) {
        const std::string walPath = "/tmp/litespeed_bench.wal";
        std::filesystem::remove(walPath);
        {
            StorageEngine db(walPath, StorageEngine::kUnlimitedHistory, syncEveryN);
            auto t0 = Clock::now();
            auto deadline = t0 + std::chrono::milliseconds(500);
            long long ops = 0;
            while (Clock::now() < deadline) {
                db.set(key, value, 1.0);
                ++ops;
            }
            double s = Seconds(Clock::now() - t0).count();
            printRow(label, ops, s);
        }
        std::filesystem::remove(walPath);
        std::filesystem::remove("/tmp/litespeed_bench.snap");
    };

    benchWAL(1,  "WAL set() syncEveryN=1  (max durability)");
    benchWAL(16, "WAL set() syncEveryN=16");
    benchWAL(64, "WAL set() syncEveryN=64");

    std::cout << std::string(80, '-') << '\n';
    std::cout << "Higher syncEveryN = fewer fsyncs = more throughput, less durability.\n"
              << "syncEveryN=1 guarantees each write survives a crash; syncEveryN=N\n"
              << "risks losing the last N-1 writes on power failure.\n";
}
