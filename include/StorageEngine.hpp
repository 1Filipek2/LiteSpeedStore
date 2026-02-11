#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <memory>
#include <vector>
#include <mutex>
#include <shared_mutex>
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
    // Default constructor uses default WAL path "litespeed.wal"
    StorageEngine();
    explicit StorageEngine(const std::string& dbPath);
    
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    void set(const std::string& key, std::string value, double duration);
    std::optional<std::string> get(const std::string& key) const;
    bool remove(const std::string& key);
    double getAverage(const std::string& key) const;

    size_t count() const; // how many unique keys i have

    size_t historyCount(const std::string& key) const;

    // Recovers state from disk
    void recover();

private:
    std::unordered_map<std::string, std::vector<std::unique_ptr<Record>>> m_data; // O(1) lookups test
    std::unique_ptr<persistence::WAL> m_wal;

    mutable std::shared_mutex m_mutex; // shared_mutex for multiple readers
};