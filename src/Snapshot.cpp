#include "persistence/Snapshot.hpp"
#include "persistence/CRC32.hpp"
#include "persistence/Endian.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace persistence
{
namespace
{

constexpr uint32_t MAGIC = 0x534E4150; // "SNAP"
constexpr uint32_t VERSION = 1;
// Header field offsets, derived from field sizes: magic(4) | version(4) | crc(4).
constexpr size_t OFF_MAGIC = 0;
constexpr size_t OFF_VERSION = OFF_MAGIC + sizeof(uint32_t);
constexpr size_t OFF_CRC = OFF_VERSION + sizeof(uint32_t);
constexpr size_t HEADER_SIZE = OFF_CRC + sizeof(uint32_t);

// minimum on-disk size of one record (timestamp + duration + value_len) and of
// one key entry (key_len + record_count). Used to reject absurd counts read from
// a corrupt/hostile file before reserving memory for them.
constexpr size_t MIN_RECORD_BYTES = sizeof(int64_t) + sizeof(uint64_t) + sizeof(uint32_t);
constexpr size_t MIN_KEY_BYTES = sizeof(uint32_t) + sizeof(uint32_t);

bool writeExact(int fd, const void* data, size_t size)
{
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size)
    {
        ssize_t rc = ::write(fd, ptr + written, size - written);
        if (rc < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (rc == 0)
        {
            return false;
        }
        written += static_cast<size_t>(rc);
    }
    return true;
}

// fsync the directory containing `path` so the rename() is itself durable.
bool fsyncParentDir(const std::string& path)
{
    std::filesystem::path p(path);
    const std::string dir = p.has_parent_path() ? p.parent_path().string() : ".";
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd == -1)
    {
        return false;
    }
    const bool ok = (::fsync(dfd) == 0);
    ::close(dfd);
    return ok;
}

bool readLE32(const std::vector<uint8_t>& buf, size_t& offset, uint32_t& value)
{
    if (offset + sizeof(uint32_t) > buf.size())
    {
        return false;
    }
    value = getLE32(buf.data() + offset);
    offset += sizeof(uint32_t);
    return true;
}

bool readLE64(const std::vector<uint8_t>& buf, size_t& offset, uint64_t& value)
{
    if (offset + sizeof(uint64_t) > buf.size())
    {
        return false;
    }
    value = getLE64(buf.data() + offset);
    offset += sizeof(uint64_t);
    return true;
}

} // namespace

bool Snapshot::save(const std::string& path, const SnapshotImage& image)
{
    // serialize all data into a payload buffer first so we can compute the CRC
    // before writing the file - avoids re-opening the file to patch in the checksum.
    std::vector<uint8_t> payload;
    putLE64(payload, image.epoch);

    const uint32_t keyCount = static_cast<uint32_t>(image.state.size());
    putLE32(payload, keyCount);

    for (const auto& [key, records] : image.state)
    {
        const uint32_t keyLen = static_cast<uint32_t>(key.size());
        putLE32(payload, keyLen);
        payload.insert(payload.end(), key.begin(), key.end());

        const uint32_t recordCount = static_cast<uint32_t>(records.size());
        putLE32(payload, recordCount);

        for (const auto& rec : records)
        {
            putLE64(payload, static_cast<uint64_t>(rec.timestamp));
            putLE64(payload, doubleToBits(rec.duration));
            const uint32_t valueLen = static_cast<uint32_t>(rec.value.size());
            putLE32(payload, valueLen);
            payload.insert(payload.end(), rec.value.begin(), rec.value.end());
        }
    }

    const uint32_t crc = CRC32::calculate(payload.data(), payload.size());

    std::vector<uint8_t> fileBuf;
    fileBuf.reserve(HEADER_SIZE + payload.size());
    putLE32(fileBuf, MAGIC);
    putLE32(fileBuf, VERSION);
    putLE32(fileBuf, crc);
    fileBuf.insert(fileBuf.end(), payload.begin(), payload.end());

    // write tmp -> fsync(file) -> rename -> fsync(dir): the rename is atomic so the
    // old snapshot survives a crash, and the fsyncs make a committed one survive power loss.
    const std::string tmpPath = path + ".tmp";
    int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
    {
        return false;
    }
    if (!writeExact(fd, fileBuf.data(), fileBuf.size()))
    {
        ::close(fd);
        return false;
    }
    if (::fsync(fd) == -1)
    {
        ::close(fd);
        return false;
    }
    if (::close(fd) == -1)
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    if (ec)
    {
        return false;
    }

    return fsyncParentDir(path);
}

bool Snapshot::load(const std::string& path, SnapshotImage& image)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }

    uint8_t header[HEADER_SIZE];
    in.read(reinterpret_cast<char*>(header), HEADER_SIZE);
    if (!in)
    {
        return false;
    }

    const uint32_t magic = getLE32(header + OFF_MAGIC);
    const uint32_t version = getLE32(header + OFF_VERSION);
    const uint32_t storedCRC = getLE32(header + OFF_CRC);
    if (magic != MAGIC || version != VERSION)
    {
        return false;
    }

    // read everything after the header and verify the checksum.
    const std::vector<uint8_t> payload{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    if (CRC32::calculate(payload.data(), payload.size()) != storedCRC)
    {
        return false;
    }

    size_t offset = 0;

    if (!readLE64(payload, offset, image.epoch))
    {
        return false;
    }

    uint32_t keyCount = 0;
    if (!readLE32(payload, offset, keyCount))
    {
        return false;
    }
    // a key entry needs at least MIN_KEY_BYTES on disk - reject impossible counts.
    if (keyCount > (payload.size() - offset) / MIN_KEY_BYTES)
    {
        return false;
    }

    image.state.clear();

    for (uint32_t i = 0; i < keyCount; ++i)
    {
        uint32_t keyLen = 0;
        if (!readLE32(payload, offset, keyLen))
        {
            return false;
        }
        if (keyLen > payload.size() - offset)
        {
            return false;
        }

        std::string key(reinterpret_cast<const char*>(payload.data() + offset), keyLen);
        offset += keyLen;

        uint32_t recordCount = 0;
        if (!readLE32(payload, offset, recordCount))
        {
            return false;
        }
        // each record needs at least MIN_RECORD_BYTES - reject impossible counts
        // before reserve() so a corrupt count can't trigger a huge allocation.
        if (recordCount > (payload.size() - offset) / MIN_RECORD_BYTES)
        {
            return false;
        }

        auto& records = image.state[key];
        records.reserve(recordCount);

        for (uint32_t j = 0; j < recordCount; ++j)
        {
            SnapshotRecord rec;
            uint64_t rawTimestamp = 0;
            uint64_t rawDuration = 0;
            if (!readLE64(payload, offset, rawTimestamp))
            {
                return false;
            }
            if (!readLE64(payload, offset, rawDuration))
            {
                return false;
            }
            rec.timestamp = static_cast<int64_t>(rawTimestamp);
            rec.duration = bitsToDouble(rawDuration);

            uint32_t valueLen = 0;
            if (!readLE32(payload, offset, valueLen))
            {
                return false;
            }
            if (valueLen > payload.size() - offset)
            {
                return false;
            }

            rec.value.assign(reinterpret_cast<const char*>(payload.data() + offset), valueLen);
            offset += valueLen;

            records.push_back(std::move(rec));
        }
    }

    return true;
}

} // namespace persistence
