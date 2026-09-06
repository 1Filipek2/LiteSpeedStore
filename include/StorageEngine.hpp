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

struct Record
{
    std::string value;
    double duration;     ///< Milliseconds.
    long long timestamp; ///< Nanoseconds since Unix epoch.

    Record(std::string v, long long ts, double d) : value(std::move(v)), duration(d), timestamp(ts) {}
};

/** Thread-safe key-value store. Reads use shared_lock; writes use unique_lock. */
class StorageEngine
{
public:
    static constexpr size_t kUnlimitedHistory = 0; ///< 0 = no cap.

    /// Largest value set() accepts: duration and value share one WAL field.
    static constexpr size_t kMaxFieldSize = persistence::WAL::kMaxFieldSize - sizeof(double);

    /**
     * @param syncEveryN  fsync() every N appends. 1 = every write is durable;
     *                    higher values trade durability for throughput.
     */
    explicit StorageEngine(const std::string& dbPath = "litespeed.wal", size_t maxHistoryPerKey = kUnlimitedHistory,
                           size_t syncEveryN = 1);

    /** No WAL, no persistence - suitable for profiling/metrics use. */
    static StorageEngine makeInMemory(size_t maxHistoryPerKey = kUnlimitedHistory);

    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    void set(const std::string& key, std::string value, double duration);
    std::optional<std::string> get(const std::string& key) const;
    bool remove(const std::string& key); ///< Returns false if key was not present.
    std::optional<double> getAverage(const std::string& key) const;
    void snapshot(); ///< Throws std::runtime_error on I/O failure.

    size_t count() const;
    size_t historyCount(const std::string& key) const;
    void recover();

private:
    struct InMemoryTag
    {
    };
    StorageEngine(InMemoryTag, size_t maxHistoryPerKey);

    /// Appends to a key's history, evicting the oldest past the cap. Caller must
    /// hold m_mutex; shared by set() and recovery so both apply the cap identically.
    void appendCapped(const std::string& key, std::string value, double duration, long long timestamp);

    std::unordered_map<std::string, std::vector<Record>> m_data;
    std::unique_ptr<persistence::WAL> m_wal;
    std::string m_walPath;
    std::string m_snapshotPath;
    std::optional<uint64_t> m_snapshotEpoch;

    size_t m_maxHistoryPerKey;
    mutable std::shared_mutex m_mutex;
};
