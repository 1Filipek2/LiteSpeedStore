#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Shared helpers for the test suite. Defined inline so they can be included
// from multiple test translation units without ODR violations.
namespace test {

inline const std::string WAL_PATH  = "test_persist.wal";
inline const std::string SNAP_PATH = "test_persist.snap";

inline void cleanup() {
    std::filesystem::remove(WAL_PATH);
    std::filesystem::remove(SNAP_PATH);
    std::filesystem::remove(SNAP_PATH + ".tmp");
}

inline std::vector<char> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>());
}

inline void writeAll(const std::string& path, const std::vector<char>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace test
