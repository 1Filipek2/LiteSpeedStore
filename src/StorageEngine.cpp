#include "StorageEngine.hpp"
#include <chrono>

void StorageEngine::set(const std::string& key, std::string value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    
    m_data[key] = std::make_unique<Record>(std::move(value), now); //testing unique+move for max effi
}

std::optional<std::string> StorageEngine::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_data.find(key);
    if (it != m_data.end()) {
        return it->second->value;
    }
    
    return std::nullopt; 
}

bool StorageEngine::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_data.erase(key) > 0;
}

size_t StorageEngine::count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_data.size();
}