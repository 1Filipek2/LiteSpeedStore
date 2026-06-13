#include "persistence/WAL.hpp"
#include "persistence/CRC32.hpp"
#include "persistence/Endian.hpp"
#include "persistence/SHA256.hpp"
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
constexpr uint32_t WAL_VERSION = 2;          // v2 adds seq + chain_hash per entry
// Fixed-size portion of each entry after the CRC:
// seq(8) + timestamp_low(4) + timestamp_high(4) + epoch(8) + key_len(4) + value_len(4) + type(1).
constexpr size_t ENTRY_FIXED_SIZE = 33;
constexpr size_t HASH_SIZE = 32;
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
Checkpoint WAL::head() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return Checkpoint{m_seq, m_headHash};
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
    // a rotated log starts a fresh chain generation.
    m_seq = 0;
    m_headHash = Sha256Digest{};
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
void WAL::append(RecordType type, const std::string& key, const std::string& value, int64_t timestamp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fd == -1) return;
    const uint64_t ts = static_cast<uint64_t>(timestamp);
    const uint32_t timestamp_low = static_cast<uint32_t>(ts & 0xFFFFFFFFu);
    const uint32_t timestamp_high = static_cast<uint32_t>((ts >> 32) & 0xFFFFFFFFu);
    const uint32_t key_len = static_cast<uint32_t>(key.size());
    const uint32_t value_len = static_cast<uint32_t>(value.size());
    const uint8_t type_byte = static_cast<uint8_t>(type);

    std::vector<uint8_t> fixed;
    fixed.reserve(ENTRY_FIXED_SIZE);
    putLE64(fixed, m_seq);
    putLE32(fixed, timestamp_low);
    putLE32(fixed, timestamp_high);
    putLE64(fixed, m_epoch);
    putLE32(fixed, key_len);
    putLE32(fixed, value_len);
    fixed.push_back(type_byte);

    // chain_hash = SHA256(prev_hash | fixed | key | value): binds this entry to
    // every entry before it, so any in-place change downstream breaks the chain.
    SHA256 ctx;
    ctx.update(m_headHash.data(), m_headHash.size());
    ctx.update(fixed.data(), fixed.size());
    ctx.update(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    ctx.update(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    const Sha256Digest chainHash = ctx.digest();

    std::vector<uint8_t> entry;
    entry.reserve(fixed.size() + key.size() + value.size() + HASH_SIZE);
    entry.insert(entry.end(), fixed.begin(), fixed.end());
    entry.insert(entry.end(), key.begin(), key.end());
    entry.insert(entry.end(), value.begin(), value.end());
    entry.insert(entry.end(), chainHash.begin(), chainHash.end());

    const uint32_t computed_crc = CRC32::calculate(entry.data(), entry.size());
    // Write CRC + entry in a single call to minimise the torn-write window.
    std::vector<uint8_t> out;
    out.reserve(sizeof(computed_crc) + entry.size());
    putLE32(out, computed_crc);
    out.insert(out.end(), entry.begin(), entry.end());
    if (!writeExact(m_fd, out.data(), out.size())) {
        throw std::runtime_error("Failed to write entry to WAL");
    }

    m_headHash = chainHash;
    ++m_seq;
    ++m_pendingWrites;
    if (m_pendingWrites >= m_syncEveryN) {
        if (::fsync(m_fd) == -1) {
            throw std::runtime_error("fsync failed");
        }
        m_pendingWrites = 0;
    }
}
RecoveryResult WAL::walkChain(
    const std::function<void(RecordType, const std::string&, const std::string&, int64_t)>& visitor,
    std::optional<uint64_t> minEpochExclusive,
    bool allowTruncate,
    uint64_t& outSeq,
    Sha256Digest& outHead) {
    RecoveryResult result;
    outSeq = 0;
    outHead = Sha256Digest{};
    if (m_fd == -1) return result;

    struct stat st;
    if (::fstat(m_fd, &st) == -1) return result;
    if (::lseek(m_fd, static_cast<off_t>(kHeaderSize), SEEK_SET) == -1) return result;

    off_t current_pos = static_cast<off_t>(kHeaderSize);
    Sha256Digest prevHash{};
    uint64_t expectedSeq = 0;
    bool torn = false;

    while (current_pos < st.st_size) {
        // A short read here means the writer was interrupted mid-entry — a torn
        // tail from a crash, which is legitimately discarded.
        uint8_t crc_bytes[4];
        if (!readExact(m_fd, crc_bytes, sizeof(crc_bytes))) { torn = true; break; }
        const uint32_t stored_crc = getLE32(crc_bytes);

        uint8_t fixed[ENTRY_FIXED_SIZE];
        if (!readExact(m_fd, fixed, ENTRY_FIXED_SIZE)) { torn = true; break; }
        const uint64_t seq       = getLE64(fixed + 0);
        const uint32_t key_len   = getLE32(fixed + 24);
        const uint32_t value_len = getLE32(fixed + 28);
        const uint8_t type_byte  = fixed[32];

        // An absurd length on a fully-present header is corruption, not a torn
        // write — flag it rather than silently truncating valid entries after it.
        constexpr uint32_t MAX_FIELD_SIZE = 1u << 20; // 1 MiB
        if (key_len > MAX_FIELD_SIZE || value_len > MAX_FIELD_SIZE) {
            return RecoveryResult{RecoveryStatus::Tampered,
                                  static_cast<uint64_t>(current_pos), expectedSeq};
        }

        std::string key(key_len, '\0');
        if (key_len > 0 && !readExact(m_fd, &key[0], key_len)) { torn = true; break; }
        std::string value(value_len, '\0');
        if (value_len > 0 && !readExact(m_fd, &value[0], value_len)) { torn = true; break; }
        uint8_t hash_bytes[HASH_SIZE];
        if (!readExact(m_fd, hash_bytes, HASH_SIZE)) { torn = true; break; }

        // From here the entry is fully present: any inconsistency is tampering.
        std::vector<uint8_t> entry;
        entry.reserve(ENTRY_FIXED_SIZE + key_len + value_len + HASH_SIZE);
        entry.insert(entry.end(), fixed, fixed + ENTRY_FIXED_SIZE);
        entry.insert(entry.end(), key.begin(), key.end());
        entry.insert(entry.end(), value.begin(), value.end());
        entry.insert(entry.end(), hash_bytes, hash_bytes + HASH_SIZE);
        if (CRC32::calculate(entry.data(), entry.size()) != stored_crc) {
            return RecoveryResult{RecoveryStatus::Tampered,
                                  static_cast<uint64_t>(current_pos), expectedSeq};
        }

        SHA256 ctx;
        ctx.update(prevHash.data(), prevHash.size());
        ctx.update(fixed, ENTRY_FIXED_SIZE);
        ctx.update(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        ctx.update(reinterpret_cast<const uint8_t*>(value.data()), value.size());
        const Sha256Digest computed = ctx.digest();
        if (std::memcmp(computed.data(), hash_bytes, HASH_SIZE) != 0) {
            return RecoveryResult{RecoveryStatus::Tampered,
                                  static_cast<uint64_t>(current_pos), expectedSeq};
        }
        if (seq != expectedSeq) { // a gap means an entry was deleted or reordered
            return RecoveryResult{RecoveryStatus::Tampered,
                                  static_cast<uint64_t>(current_pos), seq};
        }

        const uint64_t epoch = getLE64(fixed + 16);
        if (visitor && (!minEpochExclusive || epoch > *minEpochExclusive)) {
            const uint32_t timestamp_low  = getLE32(fixed + 8);
            const uint32_t timestamp_high = getLE32(fixed + 12);
            const int64_t entryTs = static_cast<int64_t>(
                (static_cast<uint64_t>(timestamp_high) << 32) | timestamp_low);
            visitor(static_cast<RecordType>(type_byte), key, value, entryTs);
        }

        prevHash = computed;
        expectedSeq = seq + 1;
        outSeq = expectedSeq;
        outHead = computed;

        current_pos = ::lseek(m_fd, 0, SEEK_CUR);
        if (current_pos == static_cast<off_t>(-1)) { torn = true; break; }
    }

    if (torn) {
        result.status = RecoveryStatus::TruncatedTail;
        std::cerr << "Detected corruption or partial write at end of WAL. Truncating to "
                  << current_pos << std::endl;
        if (allowTruncate) {
            if (::ftruncate(m_fd, current_pos) == -1) {
                std::cerr << "Failed to truncate WAL!" << std::endl;
            }
            ::lseek(m_fd, 0, SEEK_END);
        }
    }
    return result;
}
RecoveryResult WAL::recover(
    std::function<void(RecordType, const std::string&, const std::string&, int64_t)> visitor,
    std::optional<uint64_t> minEpochExclusive
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t seq = 0;
    Sha256Digest head{};
    RecoveryResult result = walkChain(visitor, minEpochExclusive, /*allowTruncate=*/true, seq, head);
    // Adopt the verified chain state so subsequent appends continue the chain.
    // On tampering we leave the file untouched and do not advance state.
    if (result.status != RecoveryStatus::Tampered) {
        m_seq = seq;
        m_headHash = head;
    }
    return result;
}
RecoveryResult WAL::verify() {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t seq = 0;
    Sha256Digest head{};
    std::function<void(RecordType, const std::string&, const std::string&, int64_t)> noVisitor;
    return walkChain(noVisitor, std::nullopt, /*allowTruncate=*/false, seq, head);
}
} // namespace persistence
