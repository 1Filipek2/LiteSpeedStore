#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
namespace persistence {
struct SnapshotRecord {
    std::string value;
    double duration;
    int64_t timestamp;
};
using SnapshotState = std::unordered_map<std::string, std::vector<SnapshotRecord>>;
struct SnapshotImage {
    uint64_t epoch = 0;
    SnapshotState state;
};
class Snapshot {
public:
    static bool save(const std::string& path, const SnapshotImage& image);
    static bool load(const std::string& path, SnapshotImage& image);
};
} // namespace persistence
