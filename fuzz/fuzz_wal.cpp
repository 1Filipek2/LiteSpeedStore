#include "persistence/WAL.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <unistd.h>

// libFuzzer entry point.
// Writes arbitrary bytes to a temp WAL file, then calls recover().
// Invariant: recover() must never crash, abort, or leak — regardless of input.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Unique path per process so parallel fuzzing instances don't collide.
    const std::string path = "/tmp/fuzz_wal_" + std::to_string(::getpid()) + ".wal";

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
    {
        return 0;
    }

    // Prepend a valid 16-byte header (magic 'WSL1' LE, version 1, flags 0) so the
    // WAL constructor accepts the file and the fuzz input exercises the entry
    // parser in recover() rather than being rejected at the header check.
    static const uint8_t header[persistence::WAL::kHeaderSize] = {
        0x57, 0x53, 0x4C, 0x31,                        // magic   = 0x314C5357
        0x02, 0x00, 0x00, 0x00,                        // version = 2
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // flags = 0
    };
    auto writeAll = [fd](const uint8_t* buf, size_t len)
    {
        while (len > 0)
        {
            ssize_t n = ::write(fd, buf, len);
            if (n <= 0)
            {
                break;
            }
            buf += static_cast<size_t>(n);
            len -= static_cast<size_t>(n);
        }
    };
    writeAll(header, sizeof(header));
    writeAll(data, size);
    ::close(fd);

    try
    {
        persistence::WAL wal(path);
        wal.recover([](persistence::RecordType, const std::string&, const std::string&, int64_t) {});
    }
    catch (...)
    {
    }

    ::unlink(path.c_str());
    return 0;
}
