#include "StorageEngine.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
namespace {
std::string snapshotPathFor(const std::string& walPath) {
    std::filesystem::path path(walPath);
    if (path.extension() == ".wal") {
        path.replace_extension(".snap");
        return path.string();
    }
    return walPath + ".snap";
}
void cleanup(const std::string& walPath) {
    const auto snapPath = snapshotPathFor(walPath);
    if (std::filesystem::exists(walPath)) {
        std::filesystem::remove(walPath);
    }
    if (std::filesystem::exists(snapPath)) {
        std::filesystem::remove(snapPath);
    }
}
void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[Fail] " << message << std::endl;
        std::exit(1);
    }
}
} // namespace
void runTest() {
    const std::string dbPath = "test_persist.wal";
    const std::string snapPath = snapshotPathFor(dbPath);
    cleanup(dbPath);
    std::cout << "Starting Persistence + Snapshot Test..." << std::endl;
    {
        StorageEngine db(dbPath);
        db.set("key1", "val1", 10.0);
        db.set("key1", "val2", 20.0);
        require(db.historyCount("key1") == 2, "key1 should have 2 records before snapshot");
        require(db.snapshot(), "snapshot() should succeed");
        require(std::filesystem::exists(snapPath), "snapshot file should exist after snapshot()");
        require(std::filesystem::file_size(dbPath) == 0, "WAL should be rotated/truncated after snapshot()");
        db.set("key1", "val3", 30.0);
        db.set("key2", "temp", 5.0);
        require(db.remove("key2"), "key2 remove should succeed");
        std::cout << "[Pass] Run 1: Snapshot created and WAL rotated." << std::endl;
    }
    {
        StorageEngine db(dbPath);
        require(db.historyCount("key1") == 3, "key1 should recover 3 records after snapshot + WAL replay");
        require(db.get("key1") && *db.get("key1") == "val3", "key1 should recover latest value val3");
        require(std::abs(db.getAverage("key1").value() - 20.0) < 0.001, "key1 average should be 20.0 after recovery");
        require(db.historyCount("key2") == 0, "key2 should be deleted after recovery");
        require(db.count() == 1, "only one key should remain after recovery");
        require(db.snapshot(), "second snapshot() should also succeed");
        require(std::filesystem::file_size(dbPath) == 0, "WAL should be empty after second snapshot()");
        std::cout << "[Pass] Run 2: Recovery after snapshot is correct." << std::endl;
    }
    {
        StorageEngine db(dbPath);
        require(db.historyCount("key1") == 3, "key1 should still have 3 records after second restart");
        require(db.get("key1") && *db.get("key1") == "val3", "key1 should still recover latest value val3");
        require(std::abs(db.getAverage("key1").value() - 20.0) < 0.001, "key1 average should stay 20.0 after second restart");
        require(db.count() == 1, "store should still contain one key after second restart");
        std::cout << "[Pass] Run 3: Second restart recovery successful." << std::endl;
    }
    cleanup(dbPath);
    std::cout << "All persistence + snapshot tests passed!" << std::endl;
}
int main() {
    runTest();
    return 0;
}
