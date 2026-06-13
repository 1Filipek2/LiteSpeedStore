#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "StorageEngine.hpp"
#include "persistence/WAL.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace test;

TEST_CASE("Persistence lifecycle: snapshot, WAL rotation, and recovery", "[persistence]") {
    cleanup();

    // Phase 1: write data, take snapshot, verify WAL is rotated
    {
        StorageEngine db(WAL_PATH);
        db.set("key1", "val1", 10.0);
        db.set("key1", "val2", 20.0);
        REQUIRE(db.historyCount("key1") == 2);
        REQUIRE_NOTHROW(db.snapshot());
        REQUIRE(std::filesystem::exists(SNAP_PATH));
        // After rotation the WAL is empty of entries but retains its header.
        REQUIRE(std::filesystem::file_size(WAL_PATH) == persistence::WAL::kHeaderSize);
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
        REQUIRE_NOTHROW(db.snapshot());
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

TEST_CASE("History cap: oldest record is evicted when limit is reached", "[history]") {
    cleanup();

    StorageEngine db(WAL_PATH, 3);

    db.set("key", "val1", 1.0);
    db.set("key", "val2", 2.0);
    db.set("key", "val3", 3.0);
    REQUIRE(db.historyCount("key") == 3);

    // 4th write evicts the oldest — count stays at 3
    db.set("key", "val4", 4.0);
    REQUIRE(db.historyCount("key") == 3);
    REQUIRE(db.get("key").value() == "val4");

    cleanup();
}

TEST_CASE("History cap is enforced during WAL replay after restart", "[history][persistence]") {
    cleanup();

    // Write 6 records under a cap of 3, with no snapshot — everything is in the WAL.
    {
        StorageEngine db(WAL_PATH, 3);
        for (int i = 0; i < 6; ++i) {
            db.set("key", "val" + std::to_string(i), static_cast<double>(i));
        }
        REQUIRE(db.historyCount("key") == 3);
    }

    // Reopen: recovery replays all 6 WAL entries and must re-apply the cap,
    // so the post-restart state matches the live state (3 newest records).
    {
        StorageEngine db(WAL_PATH, 3);
        REQUIRE(db.historyCount("key") == 3);
        REQUIRE(db.get("key").value() == "val5");
        // average of the 3 surviving durations: (3 + 4 + 5) / 3 == 4.0
        REQUIRE(db.getAverage("key").value() == Catch::Approx(4.0).epsilon(0.001));
    }

    cleanup();
}

TEST_CASE("Crash injection: a stale snapshot .tmp is ignored", "[crash]") {
    cleanup();

    {
        StorageEngine db(WAL_PATH);
        db.set("k", "v1", 1.0);
        db.snapshot();           // a good snapshot is committed
        db.set("k", "v2", 2.0);  // and one more entry lands in the rotated WAL
    }

    // Simulate a crash during the next snapshot, before the atomic rename:
    // a partial .tmp is left behind next to the valid .snap.
    {
        std::ofstream tmp(SNAP_PATH + ".tmp", std::ios::binary | std::ios::trunc);
        tmp << "garbage-partial-snapshot";
    }

    {
        StorageEngine db(WAL_PATH);
        REQUIRE(db.get("k").value() == "v2");   // loads the good .snap, replays the WAL
        REQUIRE(db.historyCount("k") == 2);
    }
    REQUIRE(std::filesystem::exists(SNAP_PATH + ".tmp")); // engine never consumes the .tmp

    cleanup();
}
