#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace persistence
{

/**
 * Standard CRC-32 (IEEE 802.3), the same one zip and gzip use.
 *
 * Catches accidental corruption in WAL and snapshot records - a torn write or a
 * bad sector. It is not a security check: an attacker can recompute a CRC after
 * editing a record, which is what the SHA-256 chain in WAL is for.
 */
class CRC32
{
public:
    static uint32_t calculate(const void* data, std::size_t length)
    {
        uint32_t crc = 0xFFFFFFFF;
        const uint8_t* p = static_cast<const uint8_t*>(data);

        for (std::size_t i = 0; i < length; ++i)
        {
            crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
        }
        return ~crc;
    }

private:
    /// Lookup table, built at compile time: one entry per byte value, each the
    /// remainder left by dividing that byte through 0xEDB88320 - the bit-reversed
    /// form of the CRC-32 polynomial, which is what lets calculate() shift right.
    static constexpr std::array<uint32_t, 256> table = []()
    {
        std::array<uint32_t, 256> t{};

        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
            {
                if (c & 1)
                {
                    c = 0xEDB88320 ^ (c >> 1);
                }
                else
                {
                    c >>= 1;
                }
            }
            t[i] = c;
        }
        return t;
    }();
};

} // namespace persistence
