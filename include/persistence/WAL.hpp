#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace persistence {

enum class RecordType : uint8_t {
    PUT    = 1,
    DELETE = 2
};

/**
 * Append-only write-ahead log with CRC32 integrity checks.
 * Entry layout: CRC32 | timestamp | epoch | key_len | value_len | type | key | value.
 */
class WAL {
public:
    /// Size of the on-disk file header: magic(4) + version(4) + flags(8).
    static constexpr size_t kHeaderSize = 16;

    /**
     * @param syncEveryN  fsync() every N appends; 1 = sync on every write.
     * @throws std::runtime_error if the file cannot be opened or has a corrupt header.
     */
    explicit WAL(const std::string& path, size_t syncEveryN = 1);
    ~WAL();

    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    void append(RecordType type, const std::string& key, const std::string& value, int64_t timestamp); ///< Throws on write or fsync failure.
    [[nodiscard]] uint64_t epoch() const;
    void setEpoch(uint64_t epoch); ///< Called after snapshot rotation to fence old entries.
    bool reset();                  ///< Truncates to zero; keeps the fd open.

    /**
     * Replays valid entries into @p visitor; skips entries at or below @p minEpochExclusive.
     * Truncates trailing corruption.
     * @return false if corruption was encountered.
     */
    bool recover(
        std::function<void(RecordType, const std::string&, const std::string&, int64_t)> visitor,
        std::optional<uint64_t> minEpochExclusive = std::nullopt
    );

    void sync();  ///< Forces fsync regardless of syncEveryN.
    void close(); ///< Flushes pending writes before closing the fd.

private:
    bool writeHeader();    ///< Writes magic+version+flags and fsyncs. Caller must hold no lock concerns (single-threaded paths only).
    bool validateHeader(); ///< Reads and checks magic+version of an existing file.

    std::string m_path;
    int m_fd = -1;
    uint64_t m_epoch = 0;
    size_t m_syncEveryN = 1;
    size_t m_pendingWrites = 0;
    mutable std::mutex m_mutex;
};

} // namespace persistence
