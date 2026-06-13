#include "persistence/WAL.hpp"
#include "persistence/CRC32.hpp"
#include "persistence/Endian.hpp"
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
constexpr uint32_t WAL_MAGIC   = 0x314C5357; // bytes 'W','S','L','1' on disk (little-endian)
constexpr uint32_t WAL_VERSION = 1;
// Fixed-size portion of each entry after the CRC:
// timestamp_low(4) + timestamp_high(4) + epoch(8) + key_len(4) + value_len(4) + type(1).
constexpr size_t ENTRY_FIXED_SIZE = 25;
} // namespace
WAL::WAL(const std::string& path, size_t syncEveryN)
    : m_path(path), m_syncEveryN(syncEveryN < 1 ? 1 : syncEveryN) {
    m_fd = ::open(m_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (m_fd == -1) {
        throw std::runtime_error("Failed to open WAL file: " + path);
    }
    struct stat st;
    if (::fstat(m_fd, &st) == -1) {
        ::close(m_fd);
        m_fd = -1;
        throw std::runtime_error("Failed to stat WAL file: " + path);
    }
    if (st.st_size == 0) {
        if (!writeHeader()) {
            ::close(m_fd);
            m_fd = -1;
            throw std::runtime_error("Failed to write WAL header: " + path);
        }
    } else if (!validateHeader()) {
        ::close(m_fd);
        m_fd = -1;
        throw std::runtime_error("Invalid or corrupt WAL header: " + path);
    }
}
WAL::~WAL() {
    close();
}
bool WAL::writeHeader() {
    std::vector<uint8_t> header;
    putLE32(header, WAL_MAGIC);
    putLE32(header, WAL_VERSION);
    putLE64(header, uint64_t{0}); // flags — reserved for future use (e.g. encryption)
    if (!writeExact(m_fd, header.data(), header.size())) return false;
    return ::fsync(m_fd) == 0;
}
bool WAL::validateHeader() {
    if (::lseek(m_fd, 0, SEEK_SET) == -1) return false;
    uint8_t header[kHeaderSize];
    if (!readExact(m_fd, header, kHeaderSize)) return false;
    if (getLE32(header) != WAL_MAGIC) return false;
    if (getLE32(header + 4) != WAL_VERSION) return false;
    return true;
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
    if (!writeHeader()) { // a rotated WAL still needs its header
        return false;
    }
    m_pendingWrites = 0;
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
    const uint64_t ts = static_cast<uint64_t>(timestamp);
    const uint32_t timestamp_low = static_cast<uint32_t>(ts & 0xFFFFFFFFu);
    const uint32_t timestamp_high = static_cast<uint32_t>((ts >> 32) & 0xFFFFFFFFu);
    const uint32_t key_len = static_cast<uint32_t>(key.size());
    const uint32_t value_len = static_cast<uint32_t>(value.size());
    const uint8_t type_byte = static_cast<uint8_t>(type);
    std::vector<uint8_t> buffer;
    buffer.reserve(ENTRY_FIXED_SIZE + key.size() + value.size());
    putLE32(buffer, timestamp_low);
    putLE32(buffer, timestamp_high);
    putLE64(buffer, m_epoch);
    putLE32(buffer, key_len);
    putLE32(buffer, value_len);
    buffer.push_back(type_byte);
    buffer.insert(buffer.end(), key.begin(), key.end());
    buffer.insert(buffer.end(), value.begin(), value.end());
    const uint32_t computed_crc = CRC32::calculate(buffer.data(), buffer.size());
    // Write CRC + entry in a single call to minimise the torn-write window.
    std::vector<uint8_t> out;
    out.reserve(sizeof(computed_crc) + buffer.size());
    putLE32(out, computed_crc);
    out.insert(out.end(), buffer.begin(), buffer.end());
    if (!writeExact(m_fd, out.data(), out.size())) {
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
    struct stat st;
    if (::fstat(m_fd, &st) == -1) return false;
    if (::lseek(m_fd, static_cast<off_t>(kHeaderSize), SEEK_SET) == -1) {
        return false;
    }
    off_t current_pos = static_cast<off_t>(kHeaderSize);
    bool clean_eof = true;
    while (current_pos < st.st_size) {
        uint8_t crc_bytes[4];
        if (!readExact(m_fd, crc_bytes, sizeof(crc_bytes))) {
            clean_eof = false;
            break;
        }
        const uint32_t stored_crc = getLE32(crc_bytes);
        uint8_t fixed[ENTRY_FIXED_SIZE];
        if (!readExact(m_fd, fixed, ENTRY_FIXED_SIZE)) {
            clean_eof = false;
            break;
        }
        const uint32_t key_len   = getLE32(fixed + 16);
        const uint32_t value_len = getLE32(fixed + 20);
        const uint8_t type_byte  = fixed[24];
        // reject entries with unreasonably large fields before allocating memory.
        // a corrupted entry could otherwise trigger a multi-GiB allocation before CRC is checked.
        constexpr uint32_t MAX_FIELD_SIZE = 1u << 20; // 1 MiB
        if (key_len > MAX_FIELD_SIZE || value_len > MAX_FIELD_SIZE) {
            clean_eof = false;
            break;
        }
        std::string key(key_len, '\0');
        if (key_len > 0 && !readExact(m_fd, &key[0], key_len)) {
            clean_eof = false;
            break;
        }
        std::string value(value_len, '\0');
        if (value_len > 0 && !readExact(m_fd, &value[0], value_len)) {
            clean_eof = false;
            break;
        }
        std::vector<uint8_t> check_buffer;
        check_buffer.reserve(ENTRY_FIXED_SIZE + key_len + value_len);
        check_buffer.insert(check_buffer.end(), fixed, fixed + ENTRY_FIXED_SIZE);
        check_buffer.insert(check_buffer.end(), key.begin(), key.end());
        check_buffer.insert(check_buffer.end(), value.begin(), value.end());
        const uint32_t calculated = CRC32::calculate(check_buffer.data(), check_buffer.size());
        if (calculated != stored_crc) {
            std::cerr << "CRC Mismatch at offset " << current_pos << "! Corrupted entry." << std::endl;
            clean_eof = false;
            break;
        }
        const uint64_t epoch = getLE64(fixed + 8);
        if (!minEpochExclusive || epoch > *minEpochExclusive) {
            const uint32_t timestamp_low  = getLE32(fixed + 0);
            const uint32_t timestamp_high = getLE32(fixed + 4);
            const int64_t ts = static_cast<int64_t>(
                (static_cast<uint64_t>(timestamp_high) << 32) | timestamp_low);
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
