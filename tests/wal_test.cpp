#include <catch2/catch_test_macros.hpp>

#include "StorageEngine.hpp"
#include "persistence/WAL.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace test;

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

TEST_CASE("Field size: a corrupt mid-log length is reported, not truncated away", "[wal][corruption]") {
    cleanup();

    {
        StorageEngine db(WAL_PATH);
        db.set("kA", "vv", 1.0);
        db.set("kB", "vv", 2.0);
        db.set("kC", "vv", 3.0);
    }

    // Blow up the first entry's value_len. Without the cap this reads past EOF,
    // looks like a torn tail, and takes the two valid entries after it. Located by
    // content: the fixed fields end with key_len(4) | value_len(4) | type(1).
    std::vector<char> bytes = readAll(WAL_PATH);
    const std::string needle = "kA";
    auto it = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
    REQUIRE(it != bytes.end());
    const size_t valueLenPos = static_cast<size_t>(it - bytes.begin()) - 5;
    for (size_t i = 0; i < 4; ++i) bytes[valueLenPos + i] = char(0xFF);
    writeAll(WAL_PATH, bytes);

    {
        persistence::WAL wal(WAL_PATH);
        const persistence::RecoveryResult r = wal.verify();
        REQUIRE(r.status == persistence::RecoveryStatus::Malformed);
        REQUIRE(r.tamperOffset == persistence::WAL::kHeaderSize);
    }

    // Refused as a foreign/stale file, and the evidence is left on disk intact.
    REQUIRE_THROWS_AS(StorageEngine(WAL_PATH), std::runtime_error);
    REQUIRE(readAll(WAL_PATH).size() == bytes.size());

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
