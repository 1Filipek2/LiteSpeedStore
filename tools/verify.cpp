#include "persistence/WAL.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

// Read-only integrity check for a LiteSpeed WAL file — the "verifier" an agent
// would run (or a remote service) to confirm a journal has not been tampered with.
// Exit codes: 0 = intact / torn tail, 1 = tampering detected, 2 = usage or I/O error.

namespace {
std::string toHex(const persistence::Sha256Digest& d) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(d.size() * 2);
    for (uint8_t b : d) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0x0F]);
    }
    return s;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: litespeed-verify <wal-file>\n";
        return 2;
    }
    // The WAL constructor would create the file if missing; a verifier must not.
    if (!std::filesystem::exists(argv[1])) {
        std::cerr << "error: no such file: " << argv[1] << "\n";
        return 2;
    }
    try {
        persistence::WAL wal(argv[1]);
        const persistence::RecoveryResult r = wal.verify();
        switch (r.status) {
            case persistence::RecoveryStatus::Ok:
                std::cout << "OK    chain intact — " << r.entriesVerified << " entries verified\n"
                          << "      head: " << toHex(r.headHash) << "\n";
                return 0;
            case persistence::RecoveryStatus::TruncatedTail:
                std::cout << "WARN  torn tail after " << r.entriesVerified
                          << " entries (incomplete final write)\n";
                return 0;
            case persistence::RecoveryStatus::Malformed:
                std::cout << "ERROR field exceeds the size limit at offset " << r.tamperOffset
                          << " (after " << r.entriesVerified << " valid entries)\n"
                          << "      not tamper evidence: a foreign or stale file\n";
                return 2;
            case persistence::RecoveryStatus::Tampered:
                std::cout << "FAIL  TAMPERING DETECTED at offset " << r.tamperOffset
                          << " (after " << r.entriesVerified << " valid entries)\n";
                return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
