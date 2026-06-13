#include "persistence/Endian.hpp"
#include "persistence/WAL.hpp"

#include <cstdint>
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

    // 1. The agent records a few endpoint events into the hash-chained log.
    {
        persistence::WAL wal(kDemoWal);
        wal.append(persistence::RecordType::PUT, "process.start", "C:\\Temp\\evil.exe", 1);
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

        // Locate the first byte of event #0's value:
        // header + crc(4) + fixed(33), with key_len read from the fixed header.
        const uint8_t* base = reinterpret_cast<const uint8_t*>(bytes.data());
        const uint32_t key_len = persistence::getLE32(base + persistence::WAL::kHeaderSize + 4 + 24);
        const size_t target = persistence::WAL::kHeaderSize + 4 + 33 + key_len;
        if (target < bytes.size()) {
            bytes[target] = static_cast<char>(bytes[target] ^ 0x20);
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
