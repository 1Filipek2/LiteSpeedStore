#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <memory>
#include <vector>
#include <mutex>

// one entry for database stored in memory
struct Record {
    std::string value;
    long long timestamp;

    Record(std::string v, long long ts) 
        : value(std::move(v)), timestamp(ts) {}
};

class StorageEngine {
public:
    StorageEngine() = default;
    
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;

    void set(const std::string& key, std::string value);
    std::optional<std::string> get(const std::string& key) const;
    bool remove(const std::string& key);

    size_t count() const; // diagnostics

private:
    std::unordered_map<std::string, std::unique_ptr<Record>> m_data; // O(1) lookups test

    mutable std::mutex m_mutex; // mutex for thread-safety
};