#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace persistence
{

struct SnapshotRecord
{
    std::string value;
    double duration;
    int64_t timestamp; ///< Nanoseconds since Unix epoch.
};

using SnapshotState = std::unordered_map<std::string, std::vector<SnapshotRecord>>;

struct SnapshotImage
{
    uint64_t epoch = 0; ///< WAL entries at or below this epoch are skipped on recovery.
    SnapshotState state;
};

/** Writes and reads binary snapshots using a tmp-then-rename strategy. */
class Snapshot
{
public:
    static bool save(const std::string& path,
                     const SnapshotImage& image); ///< Leaves existing snapshot untouched on failure.
    static bool load(const std::string& path,
                     SnapshotImage& image); ///< Returns false if absent, truncated, or CRC mismatch.
};

} // namespace persistence
