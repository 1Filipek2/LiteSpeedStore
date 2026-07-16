#include "persistence/WAL.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

// End-to-end tamper-evidence demo: an "agent" records events, a "verifier"
// confirms the chain, an "attacker" edits one byte, and the verifier catches it.

namespace {
const char* kDemoWal = "/tmp/litespeed_demo.wal";

std::string toHexPrefix(const persistence::Sha256Digest& d, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n && i < d.size(); ++i) {
        s.push_back(hex[d[i] >> 4]);
        s.push_back(hex[d[i] & 0x0F]);
    }
    return s;
}

void printVerify(const persistence::RecoveryResult& r) {
    switch (r.status) {
        case persistence::RecoveryStatus::Ok:
            std::cout << "[verify]   chain intact — " << r.entriesVerified
                      << " entries verified  [OK]\n";
            break;
        case persistence::RecoveryStatus::TruncatedTail:
            std::cout << "[verify]   torn tail after " << r.entriesVerified << " entries\n";
            break;
        case persistence::RecoveryStatus::Malformed:
            std::cout << "[verify]   field over the size limit at offset " << r.tamperOffset
                      << " — foreign or stale file\n";
            break;
        case persistence::RecoveryStatus::Tampered:
            std::cout << "[verify]   TAMPERING DETECTED at offset " << r.tamperOffset
                      << " (after " << r.entriesVerified << " valid entries)  [FAIL]\n";
            break;
    }
}
} // namespace

int main() {
    std::remove(kDemoWal);

    std::cout << "LiteSpeedStore - tamper-evidence demo\n"
              << "=====================================\n\n";

    const std::string firstValue = "C:\\Temp\\evil.exe"; // event #0's recorded data

    // 1. The agent records a few endpoint events into the hash-chained log.
    {
        persistence::WAL wal(kDemoWal);
        wal.append(persistence::RecordType::PUT, "process.start", firstValue, 1);
        wal.append(persistence::RecordType::PUT, "net.connect",   "203.0.113.7:443", 2);
        wal.append(persistence::RecordType::PUT, "file.delete",   "C:\\Windows\\prefetch\\evil.pf", 3);
        const persistence::Checkpoint cp = wal.head();
        std::cout << "[agent]    recorded 3 events to " << kDemoWal << "\n"
                  << "[agent]    checkpoint to anchor remotely:  seq=" << cp.seq
                  << "  head=" << toHexPrefix(cp.head, 8) << "...\n\n";
    }

    // 2. The verifier checks the chain — all good.
    {
        persistence::WAL wal(kDemoWal);
        printVerify(wal.verify());
    }
    std::cout << "\n";

    // 3. The attacker edits one byte of a recorded event to cover their tracks.
    {
        std::ifstream in(kDemoWal, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        in.close();

        // Find event #0's recorded value in the raw log and flip its first byte
        // (no need to know the on-disk field offsets).
        auto it = std::search(bytes.begin(), bytes.end(), firstValue.begin(), firstValue.end());
        if (it != bytes.end()) {
            const size_t target = static_cast<size_t>(it - bytes.begin());
            bytes[target] = static_cast<char>(bytes[target] ^ 0x20); // toggle ASCII case
            std::ofstream out(kDemoWal, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.close();
            std::cout << "[attacker] flipped 1 byte at offset " << target
                      << " (inside event #0's recorded data)\n\n";
        }
    }

    // 4. The verifier runs again — the chain no longer matches.
    {
        persistence::WAL wal(kDemoWal);
        printVerify(wal.verify());
    }

    std::cout << "\nThe edited entry breaks its own hash, and every later entry chains\n"
                 "onto the old value - so a single byte invalidates the rest.\n";

    std::remove(kDemoWal);
    return 0;
}
