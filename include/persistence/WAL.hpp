#pragma once

#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>

namespace persistence {

enum class RecordType : uint8_t {
    PUT = 1,
    DELETE = 2
};

struct LogEntry {
    uint32_t crc;
    uint32_t timestamp_low; 
    uint32_t timestamp_high;
    uint32_t key_len;
    uint32_t value_len;
    RecordType type;
    // Followed by key_data and value_data
};

class WAL {
public:
    explicit WAL(const std::string& path);
    ~WAL();

    // Delete copy/move to prevent fd issues
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    void append(RecordType type, const std::string& key, const std::string& value, int64_t timestamp);
    
    // Iterates over valid entries in the log. 
    // Returns true if recovery was clean, false if some corruption was encountered (and truncated).
    bool recover(std::function<void(RecordType, const std::string&, const std::string&, int64_t)> visitor);

    void sync();
    void close();

private:
    std::string m_path;
    int m_fd = -1;
    std::mutex m_mutex;
};

} // namespace persistence