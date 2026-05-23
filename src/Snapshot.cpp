#include "persistence/Snapshot.hpp"
#include "persistence/CRC32.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace persistence {
namespace {

template <typename T>
void appendPod(std::vector<uint8_t>& buf, const T& value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), p, p + sizeof(T));
}

template <typename T>
bool readPod(const std::vector<uint8_t>& buf, size_t& offset, T& value) {
    if (offset + sizeof(T) > buf.size()) return false;
    std::memcpy(&value, buf.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

} // namespace

bool Snapshot::save(const std::string& path, const SnapshotImage& image) {
    constexpr uint32_t MAGIC   = 0x534E4150; // "SNAP"
    constexpr uint32_t VERSION = 1;

    // serialize all data into a payload buffer first so we can compute the CRC
    // before writing the file — avoids re-opening the file to patch in the checksum.
    std::vector<uint8_t> payload;
    appendPod(payload, image.epoch);

    const uint32_t keyCount = static_cast<uint32_t>(image.state.size());
    appendPod(payload, keyCount);

    for (const auto& [key, records] : image.state) {
        const uint32_t keyLen = static_cast<uint32_t>(key.size());
        appendPod(payload, keyLen);
        payload.insert(payload.end(), key.begin(), key.end());

        const uint32_t recordCount = static_cast<uint32_t>(records.size());
        appendPod(payload, recordCount);

        for (const auto& rec : records) {
            appendPod(payload, rec.timestamp);
            appendPod(payload, rec.duration);
            const uint32_t valueLen = static_cast<uint32_t>(rec.value.size());
            appendPod(payload, valueLen);
            payload.insert(payload.end(), rec.value.begin(), rec.value.end());
        }
    }

    const uint32_t crc = CRC32::calculate(payload.data(), payload.size());

    const std::string tmpPath = path + ".tmp";
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out.write(reinterpret_cast<const char*>(&MAGIC),   sizeof(MAGIC));
    out.write(reinterpret_cast<const char*>(&VERSION), sizeof(VERSION));
    out.write(reinterpret_cast<const char*>(&crc),     sizeof(crc));
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));

    out.close();
    if (!out) return false;

    // rename() is atomic on POSIX — a crash between write and rename leaves
    // the original .snap file intact, so recovery always has a valid snapshot.
    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    return !ec;
}

bool Snapshot::load(const std::string& path, SnapshotImage& image) {
    constexpr uint32_t EXPECTED_MAGIC   = 0x534E4150; // "SNAP"
    constexpr uint32_t EXPECTED_VERSION = 1;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    uint32_t magic = 0, version = 0, storedCRC = 0;
    in.read(reinterpret_cast<char*>(&magic),     sizeof(magic));
    in.read(reinterpret_cast<char*>(&version),   sizeof(version));
    in.read(reinterpret_cast<char*>(&storedCRC), sizeof(storedCRC));
    if (!in) return false;

    if (magic != EXPECTED_MAGIC || version != EXPECTED_VERSION) return false;

    // read everything after the 12-byte header and verify the checksum.
    // CRC covers the same byte range that save() computed it over.
    const std::vector<uint8_t> payload{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()
    };

    if (CRC32::calculate(payload.data(), payload.size()) != storedCRC) return false;

    size_t offset = 0;

    if (!readPod(payload, offset, image.epoch)) return false;

    uint32_t keyCount = 0;
    if (!readPod(payload, offset, keyCount)) return false;

    image.state.clear();

    for (uint32_t i = 0; i < keyCount; ++i) {
        uint32_t keyLen = 0;
        if (!readPod(payload, offset, keyLen)) return false;
        if (offset + keyLen > payload.size()) return false;

        std::string key(reinterpret_cast<const char*>(payload.data() + offset), keyLen);
        offset += keyLen;

        uint32_t recordCount = 0;
        if (!readPod(payload, offset, recordCount)) return false;

        auto& records = image.state[key];
        records.reserve(recordCount);

        for (uint32_t j = 0; j < recordCount; ++j) {
            SnapshotRecord rec;
            if (!readPod(payload, offset, rec.timestamp)) return false;
            if (!readPod(payload, offset, rec.duration))  return false;

            uint32_t valueLen = 0;
            if (!readPod(payload, offset, valueLen)) return false;
            if (offset + valueLen > payload.size()) return false;

            rec.value.assign(
                reinterpret_cast<const char*>(payload.data() + offset), valueLen);
            offset += valueLen;

            records.push_back(std::move(rec));
        }
    }

    return true;
}

} // namespace persistence
