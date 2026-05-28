#include "persistence/WAL.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <unistd.h>

// libFuzzer entry point.
// Writes arbitrary bytes to a temp WAL file, then calls recover().
// Invariant: recover() must never crash, abort, or leak — regardless of input.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Unique path per process so parallel fuzzing instances don't collide.
    const std::string path = "/tmp/fuzz_wal_" + std::to_string(::getpid()) + ".wal";

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return 0;
    const uint8_t* ptr = data;
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t n = ::write(fd, ptr, remaining);
        if (n <= 0) break;
        ptr += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }
    ::close(fd);

    try {
        persistence::WAL wal(path);
        wal.recover(
            [](persistence::RecordType, const std::string&, const std::string&, int64_t) {}
        );
    } catch (...) {}

    ::unlink(path.c_str());
    return 0;
}
