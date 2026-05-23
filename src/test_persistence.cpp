#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "StorageEngine.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static const std::string WAL_PATH  = "test_persist.wal";
static const std::string SNAP_PATH = "test_persist.snap";

static void cleanup() {
    if (std::filesystem::exists(WAL_PATH))  std::filesystem::remove(WAL_PATH);
    if (std::filesystem::exists(SNAP_PATH)) std::filesystem::remove(SNAP_PATH);
}

TEST_CASE("Persistence lifecycle: snapshot, WAL rotation, and recovery", "[persistence]") {
    cleanup();

    // Phase 1: write data, take snapshot, verify WAL is rotated
    {
        StorageEngine db(WAL_PATH);
        db.set("key1", "val1", 10.0);
        db.set("key1", "val2", 20.0);
        REQUIRE(db.historyCount("key1") == 2);
        REQUIRE(db.snapshot());
        REQUIRE(std::filesystem::exists(SNAP_PATH));
        REQUIRE(std::filesystem::file_size(WAL_PATH) == 0);
        db.set("key1", "val3", 30.0);
        db.set("key2", "temp", 5.0);
        REQUIRE(db.remove("key2"));
    }

    // Phase 2: reopen, verify full recovery from snapshot + WAL replay
    {
        StorageEngine db(WAL_PATH);
        REQUIRE(db.historyCount("key1") == 3);
        REQUIRE(db.get("key1").value() == "val3");
        REQUIRE(db.getAverage("key1").value() == Catch::Approx(20.0).epsilon(0.001));
        REQUIRE(db.historyCount("key2") == 0);
        REQUIRE(db.count() == 1);
        REQUIRE(db.snapshot());
    }

    // Phase 3: second restart, state must still be consistent
    {
        StorageEngine db(WAL_PATH);
        REQUIRE(db.historyCount("key1") == 3);
        REQUIRE(db.get("key1").value() == "val3");
        REQUIRE(db.getAverage("key1").value() == Catch::Approx(20.0).epsilon(0.001));
        REQUIRE(db.count() == 1);
    }

    cleanup();
}

TEST_CASE("Thread safety: concurrent reads and writes do not crash or corrupt", "[concurrency]") {
    cleanup();

    StorageEngine db(WAL_PATH);

    constexpr int NUM_WRITERS  = 4;
    constexpr int NUM_READERS  = 4;
    constexpr int OPS_PER_THREAD = 50;

    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_WRITERS; ++i) {
        threads.emplace_back([&db, i]() {
            for (int j = 0; j < OPS_PER_THREAD; ++j) {
                db.set("key" + std::to_string(i),
                       "val" + std::to_string(j),
                       static_cast<double>(j));
            }
        });
    }

    for (int i = 0; i < NUM_READERS; ++i) {
        threads.emplace_back([&db, i]() {
            for (int j = 0; j < OPS_PER_THREAD; ++j) {
                db.get("key" + std::to_string(i % NUM_WRITERS));
                db.count();
            }
        });
    }

    for (auto& t : threads) t.join();

    REQUIRE(db.count() == NUM_WRITERS);

    cleanup();
}

TEST_CASE("WAL recovery: garbage appended at end is truncated gracefully", "[wal][corruption]") {
    cleanup();

    // Write one clean entry, then close the engine
    {
        StorageEngine db(WAL_PATH);
        db.set("before", "good", 1.0);
    }

    // Simulate a torn write — append garbage bytes to the WAL
    {
        std::ofstream f(WAL_PATH, std::ios::binary | std::ios::app);
        REQUIRE(f.is_open());
        const std::array<char, 8> garbage{0x01, 0x02, char(0xFF), char(0xFE),
                                          char(0xAB), char(0xCD), 0x00, 0x11};
        f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }

    // Reopen — WAL must truncate the garbage and recover the clean entry
    {
        StorageEngine db(WAL_PATH);
        REQUIRE(db.get("before").has_value());
        REQUIRE(db.get("before").value() == "good");
    }

    cleanup();
}
