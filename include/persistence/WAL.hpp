#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace persistence {

enum class RecordType : uint8_t {
    PUT = 1,
    DELETE = 2
};

class WAL {
public:
    explicit WAL(const std::string& path, size_t syncEveryN = 1);
    ~WAL();

    // Delete copy/move to prevent fd issues
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    void append(RecordType type, const std::string& key, const std::string& value, int64_t timestamp);
    [[nodiscard]] uint64_t epoch() const;
    void setEpoch(uint64_t epoch);
    bool reset();

    // Iterates over valid entries in the log. 
    // Returns true if recovery was clean, false if some corruption was encountered (and truncated).
    bool recover(
        std::function<void(RecordType, const std::string&, const std::string&, int64_t)> visitor,
        std::optional<uint64_t> minEpochExclusive = std::nullopt
    );

    void sync();
    void close();

private:
    std::string m_path;
    int m_fd = -1;
    uint64_t m_epoch = 0;
    size_t m_syncEveryN = 1;
    size_t m_pendingWrites = 0;
    mutable std::mutex m_mutex;
};

} // namespace persistence