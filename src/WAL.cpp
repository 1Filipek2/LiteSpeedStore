#include "persistence/WAL.hpp"
#include "persistence/CRC32.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
namespace persistence {
namespace {
bool writeExact(int fd, const void* data, size_t size) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size) {
        ssize_t rc = ::write(fd, ptr + written, size - written);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) return false;
        written += static_cast<size_t>(rc);
    }
    return true;
}
bool readExact(int fd, void* data, size_t size) {
    auto* ptr = static_cast<uint8_t*>(data);
    size_t readBytes = 0;
    while (readBytes < size) {
        ssize_t rc = ::read(fd, ptr + readBytes, size - readBytes);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) return false;
        readBytes += static_cast<size_t>(rc);
    }
    return true;
}
template <typename T>
void appendPod(std::vector<uint8_t>& buffer, const T& value) {
    const auto* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
}
template <typename T>
bool readPod(int fd, T& value) {
    return readExact(fd, &value, sizeof(T));
}
} // namespace
WAL::WAL(const std::string& path, size_t syncEveryN)
    : m_path(path), m_syncEveryN(syncEveryN < 1 ? 1 : syncEveryN) {
    m_fd = ::open(m_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (m_fd == -1) {
        throw std::runtime_error("Failed to open WAL file: " + path);
    }
}
WAL::~WAL() {
    close();
}
uint64_t WAL::epoch() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_epoch;
}
void WAL::setEpoch(uint64_t epoch) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_epoch = epoch;
}
bool WAL::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd == -1) return false;
    if (::fsync(m_fd) == -1) {
        std::cerr << "WAL fsync before reset failed" << std::endl;
    }
    if (::ftruncate(m_fd, 0) == -1) {
        return false;
    }
    if (::lseek(m_fd, 0, SEEK_SET) == -1) {
        return false;
    }
    return true;
}
void WAL::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd != -1) {
        if (m_pendingWrites > 0) {
            ::fsync(m_fd);
            m_pendingWrites = 0;
        }
        ::close(m_fd);
        m_fd = -1;
    }
}
void WAL::sync() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd != -1) {
        if (::fsync(m_fd) == -1) {
             std::cerr << "WAL fsync failed" << std::endl;
        }
        m_pendingWrites = 0;
    }
}
void WAL::append(RecordType type, const std::string& key, const std::string& value, int64_t timestamp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd == -1) return;
    const uint32_t timestamp_low = static_cast<uint32_t>(timestamp & 0xFFFFFFFF);
    const uint32_t timestamp_high = static_cast<uint32_t>((timestamp >> 32) & 0xFFFFFFFF);
    const uint64_t epoch = m_epoch;
    const uint32_t key_len = static_cast<uint32_t>(key.size());
    const uint32_t value_len = static_cast<uint32_t>(value.size());
    const uint8_t type_byte = static_cast<uint8_t>(type);
    std::vector<uint8_t> buffer;
    buffer.reserve(sizeof(timestamp_low) + sizeof(timestamp_high) + sizeof(epoch) + sizeof(key_len) + sizeof(value_len) + sizeof(type_byte) + key.size() + value.size());
    appendPod(buffer, timestamp_low);
    appendPod(buffer, timestamp_high);
    appendPod(buffer, epoch);
    appendPod(buffer, key_len);
    appendPod(buffer, value_len);
    appendPod(buffer, type_byte);
    buffer.insert(buffer.end(), key.begin(), key.end());
    buffer.insert(buffer.end(), value.begin(), value.end());
    const uint32_t computed_crc = CRC32::calculate(buffer.data(), buffer.size());
    if (!writeExact(m_fd, &computed_crc, sizeof(computed_crc))) {
        throw std::runtime_error("Failed to write CRC to WAL");
    }
    if (!buffer.empty() && !writeExact(m_fd, buffer.data(), buffer.size())) {
        throw std::runtime_error("Failed to write entry to WAL");
    }
    ++m_pendingWrites;
    if (m_pendingWrites >= m_syncEveryN) {
        if (::fsync(m_fd) == -1) {
            throw std::runtime_error("fsync failed");
        }
        m_pendingWrites = 0;
    }
}
bool WAL::recover(
    std::function<void(RecordType, const std::string&, const std::string&, int64_t)> visitor,
    std::optional<uint64_t> minEpochExclusive
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd == -1) return false;
    if (::lseek(m_fd, 0, SEEK_SET) == -1) {
        return false;
    }
    struct stat st;
    if (::fstat(m_fd, &st) == -1) return false;
    off_t current_pos = 0;
    bool clean_eof = true;
    while (current_pos < st.st_size) {
        uint32_t stored_crc = 0;
        if (!readPod(m_fd, stored_crc)) {
            clean_eof = false;
            break;
        }
        uint32_t timestamp_low = 0;
        uint32_t timestamp_high = 0;
        uint64_t epoch = 0;
        uint32_t key_len = 0;
        uint32_t value_len = 0;
        uint8_t type_byte = 0;
        if (!readPod(m_fd, timestamp_low) ||
            !readPod(m_fd, timestamp_high) ||
            !readPod(m_fd, epoch) ||
            !readPod(m_fd, key_len) ||
            !readPod(m_fd, value_len) ||
            !readPod(m_fd, type_byte)) {
            clean_eof = false;
            break;
        }
        // reject entries with unreasonably large fields before allocating memory.
        // a corrupted entry could otherwise trigger a multi-GiB allocation before CRC is checked.
        constexpr uint32_t MAX_FIELD_SIZE = 1u << 20; // 1 MiB
        if (key_len > MAX_FIELD_SIZE || value_len > MAX_FIELD_SIZE) {
            clean_eof = false;
            break;
        }
        std::vector<uint8_t> check_buffer;
        check_buffer.reserve(sizeof(timestamp_low) + sizeof(timestamp_high) + sizeof(epoch) + sizeof(key_len) + sizeof(value_len) + sizeof(type_byte) + key_len + value_len);
        appendPod(check_buffer, timestamp_low);
        appendPod(check_buffer, timestamp_high);
        appendPod(check_buffer, epoch);
        appendPod(check_buffer, key_len);
        appendPod(check_buffer, value_len);
        appendPod(check_buffer, type_byte);
        std::string key(key_len, '\0');
        if (key_len > 0) {
            if (!readExact(m_fd, &key[0], key_len)) {
                clean_eof = false;
                break;
            }
            check_buffer.insert(check_buffer.end(), key.begin(), key.end());
        }
        std::string value(value_len, '\0');
        if (value_len > 0) {
            if (!readExact(m_fd, &value[0], value_len)) {
                clean_eof = false;
                break;
            }
            check_buffer.insert(check_buffer.end(), value.begin(), value.end());
        }
        const uint32_t calculated = CRC32::calculate(check_buffer.data(), check_buffer.size());
        if (calculated != stored_crc) {
            std::cerr << "CRC Mismatch at offset " << current_pos << "! Corrupted entry." << std::endl;
            clean_eof = false;
            break;
        }
        if (!minEpochExclusive || epoch > *minEpochExclusive) {
            const int64_t ts = (static_cast<int64_t>(timestamp_high) << 32) | timestamp_low;
            visitor(static_cast<RecordType>(type_byte), key, value, ts);
        }
        current_pos = ::lseek(m_fd, 0, SEEK_CUR);
        if (current_pos == static_cast<off_t>(-1)) {
            clean_eof = false;
            break;
        }
    }
    if (!clean_eof) {
        std::cerr << "Detected corruption or partial write at end of WAL. Truncating to " << current_pos << std::endl;
        if (::ftruncate(m_fd, current_pos) == -1) {
            std::cerr << "Failed to truncate WAL!" << std::endl;
            return false;
        }
        ::lseek(m_fd, 0, SEEK_END);
    }
    return true;
}
} // namespace persistence
