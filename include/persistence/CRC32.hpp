#pragma once
#include <cstdint>
#include <array>

namespace persistence {

class CRC32 {
public:
    static uint32_t calculate(const void* data, size_t length) {
        uint32_t crc = 0xFFFFFFFF;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < length; ++i) {
            crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
        }
        return ~crc;
    }

private:
    static constexpr std::array<uint32_t, 256> table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                if (c & 1) {
                    c = 0xEDB88320 ^ (c >> 1);
                } else {
                    c >>= 1;
                }
            }
            t[i] = c;
        }
        return t;
    }();
};

} // namespace persistence
