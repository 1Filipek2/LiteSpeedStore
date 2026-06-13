#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "StorageEngine.hpp"
#include "persistence/SHA256.hpp"
#include "persistence/WAL.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

static const std::string WAL_PATH  = "test_persist.wal";
static const std::string SNAP_PATH = "test_persist.snap";

static std::vector<char> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>());
}

static void writeAll(const std::string& path, const std::vector<char>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static std::string toHex(const persistence::Sha256Digest& d) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(d.size() * 2);
    for (uint8_t b : d) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0x0F]);
    }
    return s;
}

static void cleanup() {
    if (std::filesystem::exists(WAL_PATH))  std::filesystem::remove(WAL_PATH);
    if (std::filesystem::exists(SNAP_PATH)) std::filesystem::remove(SNAP_PATH);
    if (std::filesystem::exists(SNAP_PATH + ".tmp")) std::filesystem::remove(SNAP_PATH + ".tmp");
}

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

TEST_CASE("SHA256 matches known FIPS 180-4 test vectors", "[sha256]") {
    const std::string empty;
    REQUIRE(toHex(persistence::SHA256::hash(
                reinterpret_cast<const uint8_t*>(empty.data()), empty.size()))
            == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const std::string abc = "abc";
    REQUIRE(toHex(persistence::SHA256::hash(
                reinterpret_cast<const uint8_t*>(abc.data()), abc.size()))
            == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("Hash chain: a clean log verifies and exposes a checkpoint", "[tamper]") {
    cleanup();

    {
        StorageEngine db(WAL_PATH);
        db.set("a", "1", 1.0);
        db.set("b", "2", 2.0);
    }

    persistence::WAL wal(WAL_PATH);
    const persistence::RecoveryResult result = wal.recover(
        [](persistence::RecordType, const std::string&, const std::string&, int64_t) {});
    REQUIRE(result.status == persistence::RecoveryStatus::Ok);

    const persistence::Checkpoint cp = wal.head();
    REQUIRE(cp.seq == 2); // two entries chained
    bool headIsNonZero = false;
    for (uint8_t b : cp.head) if (b != 0) headIsNonZero = true;
    REQUIRE(headIsNonZero);

    cleanup();
}

TEST_CASE("Tamper evidence: flipping one byte mid-log is detected", "[tamper]") {
    cleanup();

    {
        StorageEngine db(WAL_PATH);
        db.set("key1", "val1", 1.0);
        db.set("key2", "val2", 2.0);
        db.set("key3", "val3", 3.0);
    }

    // Flip a byte inside the first entry (which has entries after it, so this is
    // an interior modification, not a torn tail). We locate event #0 by its key
    // rather than hard-coding a byte offset.
    std::vector<char> bytes = readAll(WAL_PATH);
    const std::string needle = "key1";
    auto it = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
    REQUIRE(it != bytes.end());
    const size_t target = static_cast<size_t>(it - bytes.begin());
    bytes[target] = static_cast<char>(bytes[target] ^ 0x01);
    writeAll(WAL_PATH, bytes);

    // Read-only verification reports tampering at the first entry, file untouched.
    {
        persistence::WAL wal(WAL_PATH);
        const persistence::RecoveryResult r = wal.verify();
        REQUIRE(r.status == persistence::RecoveryStatus::Tampered);
        REQUIRE(r.tamperOffset == persistence::WAL::kHeaderSize);
    }
    REQUIRE(readAll(WAL_PATH).size() == bytes.size()); // verify() did not truncate

    // The engine refuses to load a tampered journal.
    REQUIRE_THROWS_AS(StorageEngine(WAL_PATH), std::runtime_error);

    cleanup();
}

TEST_CASE("Tamper evidence: deleting a middle entry breaks the chain", "[tamper]") {
    cleanup();

    // Three records with identical key/value sizes => three equal-length entries.
    {
        StorageEngine db(WAL_PATH);
        db.set("kA", "vv", 1.0);
        db.set("kB", "vv", 2.0);
        db.set("kC", "vv", 3.0);
    }

    std::vector<char> bytes = readAll(WAL_PATH);
    const size_t header = persistence::WAL::kHeaderSize;
    const size_t entrySize = (bytes.size() - header) / 3;
    REQUIRE((bytes.size() - header) % 3 == 0);

    // Splice out the middle entry, leaving a structurally valid file whose chain
    // and seq no longer line up.
    std::vector<char> spliced;
    spliced.insert(spliced.end(), bytes.begin(),
                   bytes.begin() + static_cast<std::ptrdiff_t>(header + entrySize));
    spliced.insert(spliced.end(),
                   bytes.begin() + static_cast<std::ptrdiff_t>(header + 2 * entrySize),
                   bytes.end());
    writeAll(WAL_PATH, spliced);

    persistence::WAL wal(WAL_PATH);
    const persistence::RecoveryResult r = wal.verify();
    REQUIRE(r.status == persistence::RecoveryStatus::Tampered);

    cleanup();
}

TEST_CASE("Crash injection: a half-written tail entry is discarded on recovery", "[crash]") {
    cleanup();

    {
        StorageEngine db(WAL_PATH);
        db.set("k", "v1", 1.0);
        db.set("k", "v2", 2.0);
        db.set("k", "v3", 3.0);
    }

    // Simulate a crash mid-append: chop the last entry short so its trailing
    // hash can no longer be read in full.
    const auto size = std::filesystem::file_size(WAL_PATH);
    REQUIRE(size > 20);
    std::filesystem::resize_file(WAL_PATH, size - 20);

    {
        StorageEngine db(WAL_PATH);
        REQUIRE(db.historyCount("k") == 2);       // torn tail dropped, prefix kept
        REQUIRE(db.get("k").value() == "v2");
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
