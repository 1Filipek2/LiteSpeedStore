#include "persistence/WAL.hpp"
#include "persistence/CRC32.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <stdexcept>

namespace persistence {

WAL::WAL(const std::string& path) : m_path(path) {
    m_fd = ::open(m_path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (m_fd == -1) {
        throw std::runtime_error("Failed to open WAL file: " + path);
    }
}

WAL::~WAL() {
    close();
}

void WAL::close() {
    if (m_fd != -1) {
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
    }
}

void WAL::append(RecordType type, const std::string& key, const std::string& value, int64_t timestamp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd == -1) return;

    LogEntry header;
    header.timestamp_low = static_cast<uint32_t>(timestamp & 0xFFFFFFFF);
    header.timestamp_high = static_cast<uint32_t>((timestamp >> 32) & 0xFFFFFFFF);
    header.key_len = static_cast<uint32_t>(key.size());
    header.value_len = static_cast<uint32_t>(value.size());
    header.type = type;
    header.crc = 0; // Placeholder

    // Serialize to buffer to calculate CRC
    std::vector<uint8_t> buffer;
    buffer.reserve(sizeof(LogEntry) + key.size() + value.size());
    
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&header);
    // Push header (skipping CRC first 4 bytes for calculation)
    buffer.insert(buffer.end(), p + sizeof(uint32_t), p + sizeof(LogEntry));
    
    // Push Data
    buffer.insert(buffer.end(), key.begin(), key.end());
    buffer.insert(buffer.end(), value.begin(), value.end());

    // Calculate CRC
    uint32_t computed_crc = CRC32::calculate(buffer.data(), buffer.size());
    
    // Write CRC followed by buffer
    if (::write(m_fd, &computed_crc, sizeof(computed_crc)) != sizeof(computed_crc)) {
        throw std::runtime_error("Failed to write CRC to WAL");
    }
    if (::write(m_fd, buffer.data(), buffer.size()) != static_cast<ssize_t>(buffer.size())) {
        throw std::runtime_error("Failed to write entry to WAL");
    }

    // Explicit sync for durability (fsync discipline)
    if (::fsync(m_fd) == -1) {
        throw std::runtime_error("fsync failed");
    }
}

bool WAL::recover(std::function<void(RecordType, const std::string&, const std::string&, int64_t)> visitor) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd == -1) return false;

    // Reposition to start
    if (::lseek(m_fd, 0, SEEK_SET) == -1) {
        return false;
    }

    struct stat st;
    if (::fstat(m_fd, &st) == -1) return false;
    
    off_t file_size = st.st_size;
    off_t current_pos = 0;
    bool clean_eof = true;

    while (current_pos < file_size) {
        LogEntry header;
        // Peek header
        ssize_t header_size = sizeof(LogEntry);
        // We read the CRC + rest
        // But we need to read strictly to check for partial writes
        
        uint32_t stored_crc;
        ssize_t r = ::read(m_fd, &stored_crc, sizeof(stored_crc));
        if (r == 0) break; // EOF exactly at boundary
        if (r != sizeof(stored_crc)) {
             // Partial CRC write
             clean_eof = false;
             break;
        }

        // Read rest of header
        // Reconstruct temp buffer for CRC check
        std::vector<uint8_t> check_buffer;
        
        // We need to read the rest of the struct (sizeof(LogEntry) - 4)
        uint8_t rest_of_header[sizeof(LogEntry) - 4];
        r = ::read(m_fd, rest_of_header, sizeof(rest_of_header));
        if (r != sizeof(rest_of_header)) {
            clean_eof = false;
            break;
        }
        
        // Copy into our header struct for easy access
        const uint8_t* ptr = rest_of_header;
        std::memcpy(&header.timestamp_low, ptr, 4); ptr += 4;
        std::memcpy(&header.timestamp_high, ptr, 4); ptr += 4;
        std::memcpy(&header.key_len, ptr, 4); ptr += 4;
        std::memcpy(&header.value_len, ptr, 4); ptr += 4;
        std::memcpy(&header.type, ptr, 1);
        
        check_buffer.insert(check_buffer.end(), rest_of_header, rest_of_header + sizeof(rest_of_header));

        // Read Key
        std::string key(header.key_len, '\0');
        if (header.key_len > 0) {
            r = ::read(m_fd, &key[0], header.key_len);
            if (r != header.key_len) {
                clean_eof = false;
                break;
            }
            check_buffer.insert(check_buffer.end(), key.begin(), key.end());
        }

        // Read Value
        std::string value(header.value_len, '\0');
        if (header.value_len > 0) {
            r = ::read(m_fd, &value[0], header.value_len);
            if (r != header.value_len) {
                clean_eof = false;
                break;
            }
            check_buffer.insert(check_buffer.end(), value.begin(), value.end());
        }

        // Verify CRC
        uint32_t calculated = CRC32::calculate(check_buffer.data(), check_buffer.size());
        if (calculated != stored_crc) {
            std::cerr << "CRC Mismatch at offset " << current_pos << "! Corrupted entry." << std::endl;
            clean_eof = false;
            break; // Stop replay
        }

        // Valid entry, callback
        int64_t ts = (static_cast<int64_t>(header.timestamp_high) << 32) | header.timestamp_low;
        visitor(header.type, key, value, ts);

        // Update successful position
        current_pos = ::lseek(m_fd, 0, SEEK_CUR);
    }

    if (!clean_eof) {
        std::cerr << "Detected corruption or partial write at end of WAL. Truncating to " << current_pos << std::endl;
        if (::ftruncate(m_fd, current_pos) == -1) {
            std::cerr << "Failed to truncate WAL!" << std::endl;
            return false;
        }
        // Seek back to end for future writes
        ::lseek(m_fd, 0, SEEK_END);
    }

    return true;
}

} // namespace persistence
