#include "persistence/CRC32.hpp"
#include "persistence/Endian.hpp"
#include "persistence/Snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

// libFuzzer entry point for the snapshot parser.
// The fuzz input is treated as the snapshot payload and wrapped in a valid
// header (magic + version + matching CRC) so load() gets past the integrity
// check and actually exercises the length-driven parsing in Snapshot::load().
// Invariant: load() must never crash, over-read, or over-allocate on any input.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string path = "/tmp/fuzz_snap_" + std::to_string(::getpid()) + ".snap";

    std::vector<uint8_t> file;
    persistence::putLE32(file, 0x534E4150u); // "SNAP" magic
    persistence::putLE32(file, 1u);          // version
    persistence::putLE32(file, persistence::CRC32::calculate(data, size));
    file.insert(file.end(), data, data + size);

    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return 0;
    const uint8_t* ptr = file.data();
    size_t remaining = file.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, ptr, remaining);
        if (n <= 0) break;
        ptr += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }
    ::close(fd);

    persistence::SnapshotImage image;
    (void)persistence::Snapshot::load(path, image);

    ::unlink(path.c_str());
    return 0;
}
