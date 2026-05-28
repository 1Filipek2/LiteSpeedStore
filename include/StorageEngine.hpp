#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <memory>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include "persistence/WAL.hpp"

// one entry for database stored in memory
struct Record {
    std::string value;
    double duration;
    long long timestamp;

    Record(std::string v, long long ts, double d) 
        : value(std::move(v)), duration(d), timestamp(ts) {}
};

class StorageEngine {
public:
    static constexpr size_t kUnlimitedHistory = 0;

    explicit StorageEngine(const std::string& dbPath = "litespeed.wal",
                           size_t maxHistoryPerKey = kUnlimitedHistory,
                           size_t syncEveryN = 1);

    // In-memory only — no WAL, no persistence. Suitable for profiling/metrics.
    static StorageEngine makeInMemory(size_t maxHistoryPerKey = kUnlimitedHistory);
    
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    void set(const std::string& key, std::string value, double duration);
    std::optional<std::string> get(const std::string& key) const;
    bool remove(const std::string& key);
    std::optional<double> getAverage(const std::string& key) const;
    // Throws std::runtime_error on failure
    void snapshot();

    size_t count() const;
    size_t historyCount(const std::string& key) const;
    void recover();

private:
    struct InMemoryTag {};
    StorageEngine(InMemoryTag, size_t maxHistoryPerKey);

    std::unordered_map<std::string, std::vector<Record>> m_data;
    std::unique_ptr<persistence::WAL> m_wal;
    std::string m_walPath;
    std::string m_snapshotPath;
    std::optional<uint64_t> m_snapshotEpoch;

    size_t m_maxHistoryPerKey;
    mutable std::shared_mutex m_mutex;
};