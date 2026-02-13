#include "StorageEngine.hpp"
#include <chrono>
#include <cstring>
#include <iostream>

namespace {
    std::string serialize(double duration, const std::string& value) {
        std::string blob;
        blob.resize(sizeof(double) + value.size());
        std::memcpy(blob.data(), &duration, sizeof(double));
        std::memcpy(blob.data() + sizeof(double), value.data(), value.size());
        return blob;
    }

    std::pair<double, std::string> deserialize(const std::string& blob) {
        if (blob.size() < sizeof(double)) return {0.0, ""};
        double duration;
        std::memcpy(&duration, blob.data(), sizeof(double));
        return {duration, blob.substr(sizeof(double))};
    }
}

StorageEngine::StorageEngine() : StorageEngine("litespeed.wal") {}

StorageEngine::StorageEngine(const std::string& dbPath) {
    m_wal = std::make_unique<persistence::WAL>(dbPath);
    recover();
}

void StorageEngine::recover() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_data.clear();
    
    m_wal->recover([this](persistence::RecordType type, const std::string& key, const std::string& blob, int64_t timestamp) {
        if (type == persistence::RecordType::PUT) {
            auto [duration, value] = deserialize(blob);
            m_data[key].push_back(std::make_unique<Record>(std::move(value), timestamp, duration));
        } else if (type == persistence::RecordType::DELETE) {
            m_data.erase(key);
        }
    });
}

void StorageEngine::set(const std::string& key, std::string value, double duration) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    
    // WAL Append
    if (m_wal) {
        std::string blob = serialize(duration, value);
        m_wal->append(persistence::RecordType::PUT, key, blob, now);
    }

    m_data[key].push_back(std::make_unique<Record>(std::move(value), now, duration)); 
}

std::optional<std::string> StorageEngine::get(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    
    auto it = m_data.find(key);
    if (it != m_data.end() && !it->second.empty()) {
        return it->second.back()->value;
    }
    
    return std::nullopt; 
}

double StorageEngine::getAverage(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto it = m_data.find(key);
    if (it != m_data.end() && !it->second.empty()) {
        double sum = 0.0;
        for(const auto& record : it->second) {
            sum += record->duration;
        }
        return sum / it->second.size();   
    }
    return 0.0;
}

bool StorageEngine::remove(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    
    // WAL Append
    if (m_wal) {
         // Timestamp for delete isn't critical but good for ordering
         auto now = std::chrono::system_clock::now().time_since_epoch().count();
         m_wal->append(persistence::RecordType::DELETE, key, "", now);
    }

    return m_data.erase(key) > 0;
}

size_t StorageEngine::count() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_data.size();
}

size_t StorageEngine::historyCount(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_data.find(key);
    if (it != m_data.end()) {
        return it->second.size();
    }
    return 0;
}