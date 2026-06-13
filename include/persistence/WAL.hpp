#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "persistence/SHA256.hpp"

namespace persistence {

enum class RecordType : uint8_t {
    PUT    = 1,
    DELETE = 2
};

/// Outcome of walking the log chain during recovery or verification.
enum class RecoveryStatus : uint8_t {
    Ok,            ///< The whole chain verified cleanly.
    TruncatedTail, ///< A partial/torn write at the end was discarded (crash recovery).
    Tampered       ///< An entry was modified, reordered, or deleted in place.
};

struct RecoveryResult {
    RecoveryStatus status = RecoveryStatus::Ok;
    uint64_t entriesVerified = 0; ///< Count of entries whose CRC + hash chain verified.
    uint64_t tamperOffset = 0;    ///< Byte offset of the first bad entry (Tampered only).
    uint64_t tamperSeq = 0;       ///< Sequence number expected at that point (Tampered only).
    Sha256Digest headHash{};      ///< Chain hash after the last verified entry (the checkpoint anchor).
};

/// A tamper-evidence anchor: the chain head an agent would ship to a trusted
/// remote so that later truncation of the local log becomes detectable.
struct Checkpoint {
    uint64_t seq = 0;    ///< Entries written so far in the current generation.
    Sha256Digest head{}; ///< Chain hash of the last entry (all-zero when empty).
};

/**
 * Append-only write-ahead log with a SHA-256 hash chain for tamper evidence.
 * Entry layout: CRC32 | seq | timestamp | epoch | key_len | value_len | type |
 * key | value | chain_hash, where chain_hash = SHA256(prev_hash | seq..value).
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
    bool reset();                  ///< Truncates to the header; starts a fresh chain generation.

    [[nodiscard]] Checkpoint head() const; ///< Current chain anchor (seq + head hash).

    /**
     * Replays valid entries into @p visitor; skips entries at or below @p minEpochExclusive.
     * Verifies the hash chain: a torn tail is truncated, but tampering is reported
     * (and the file is left intact so the evidence survives).
     */
    RecoveryResult recover(
        std::function<void(RecordType, const std::string&, const std::string&, int64_t)> visitor,
        std::optional<uint64_t> minEpochExclusive = std::nullopt
    );

    /// Read-only chain verification — never mutates the file. For a verifier tool.
    [[nodiscard]] RecoveryResult verify();

    void close(); ///< Flushes pending writes before closing the fd.

private:
    bool writeHeader();    ///< Writes magic+version+flags and fsyncs.
    bool validateHeader(); ///< Reads and checks magic+version of an existing file.

    /// Shared walk used by recover() and verify(). Verifies CRC, chain hash, and
    /// seq monotonicity. @p allowTruncate discards a torn tail in-place.
    RecoveryResult walkChain(
        const std::function<void(RecordType, const std::string&, const std::string&, int64_t)>& visitor,
        std::optional<uint64_t> minEpochExclusive,
        bool allowTruncate);

    std::string m_path;
    int m_fd = -1;
    uint64_t m_epoch = 0;
    uint64_t m_seq = 0;          ///< Next sequence number to assign.
    Sha256Digest m_headHash{};   ///< Chain hash of the last appended entry.
    size_t m_syncEveryN = 1;
    size_t m_pendingWrites = 0;
    mutable std::mutex m_mutex;
};

} // namespace persistence
