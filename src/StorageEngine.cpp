#include "StorageEngine.hpp"
#include "persistence/Snapshot.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>
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
std::string makeSnapshotPath(const std::string& walPath) {
    std::filesystem::path path(walPath);
    if (path.extension() == ".wal") {
        path.replace_extension(".snap");
        return path.string();
    }
    return walPath + ".snap";
}
persistence::SnapshotState exportSnapshotState(
    const std::unordered_map<std::string, std::vector<Record>>& data
) {
    persistence::SnapshotState snapshotState;
    for (const auto& [key, history] : data) {
        auto& snapshotHistory = snapshotState[key];
        snapshotHistory.reserve(history.size());
        for (const auto& record : history) {
            snapshotHistory.push_back(persistence::SnapshotRecord{record.value, record.duration, record.timestamp});
        }
    }
    return snapshotState;
}
void importSnapshotState(
    const persistence::SnapshotState& snapshotState,
    std::unordered_map<std::string, std::vector<Record>>& data
) {
    data.clear();
    for (const auto& [key, history] : snapshotState) {
        auto& records = data[key];
        records.reserve(history.size());
        for (const auto& record : history) {
            records.emplace_back(record.value, record.timestamp, record.duration);
        }
    }
}
} // namespace
StorageEngine::StorageEngine(const std::string& dbPath, size_t maxHistoryPerKey, size_t syncEveryN)
    : m_wal(std::make_unique<persistence::WAL>(dbPath, syncEveryN)),
      m_walPath(dbPath),
      m_snapshotPath(makeSnapshotPath(dbPath)),
      m_maxHistoryPerKey(maxHistoryPerKey) {
    recover();
}

StorageEngine::StorageEngine(InMemoryTag, size_t maxHistoryPerKey)
    : m_maxHistoryPerKey(maxHistoryPerKey) {}

StorageEngine StorageEngine::makeInMemory(size_t maxHistoryPerKey) {
    return StorageEngine(InMemoryTag{}, maxHistoryPerKey);
}
void StorageEngine::recover() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_data.clear();
    m_snapshotEpoch.reset();
    persistence::SnapshotImage image;
    if (persistence::Snapshot::load(m_snapshotPath, image)) {
        m_snapshotEpoch = image.epoch;
        importSnapshotState(image.state, m_data);
        m_wal->setEpoch(image.epoch + 1);
    } else {
        m_wal->setEpoch(0);
    }
    m_wal->recover([this](persistence::RecordType type, const std::string& key, const std::string& blob, int64_t timestamp) {
        if (type == persistence::RecordType::PUT) {
            auto [duration, value] = deserialize(blob);
            m_data[key].emplace_back(std::move(value), timestamp, duration);
        } else if (type == persistence::RecordType::DELETE) {
            m_data.erase(key);
        }
    }, m_snapshotEpoch);
}
void StorageEngine::set(const std::string& key, std::string value, double duration) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (m_wal) {
        std::string blob = serialize(duration, value);
        m_wal->append(persistence::RecordType::PUT, key, blob, now);
    }
    auto& history = m_data[key];
    if (m_maxHistoryPerKey != StorageEngine::kUnlimitedHistory &&
        history.size() >= m_maxHistoryPerKey) {
        history.erase(history.begin());
    }
    history.emplace_back(std::move(value), now, duration);
}
std::optional<std::string> StorageEngine::get(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_data.find(key);
    if (it != m_data.end() && !it->second.empty()) {
        return it->second.back().value;
    }
    return std::nullopt;
}
std::optional<double> StorageEngine::getAverage(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_data.find(key);
    if (it != m_data.end() && !it->second.empty()) {
        double sum = 0.0;
        for (const auto& record : it->second) {
            sum += record.duration;
        }
        return sum / static_cast<double>(it->second.size());
    }
    return std::nullopt;
}
void StorageEngine::snapshot() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (!m_wal) {
        throw std::runtime_error("snapshot() called on engine with no WAL");
    }
    persistence::SnapshotImage image;
    image.epoch = m_wal->epoch();
    image.state = exportSnapshotState(m_data);
    if (!persistence::Snapshot::save(m_snapshotPath, image)) {
        throw std::runtime_error("Failed to write snapshot to disk: " + m_snapshotPath);
    }
    m_snapshotEpoch = image.epoch;
    m_wal->setEpoch(image.epoch + 1);
    if (!m_wal->reset()) {
        throw std::runtime_error("WAL rotation failed after snapshot: " + m_walPath);
    }
}
bool StorageEngine::remove(const std::string& key) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (m_wal) {
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
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
