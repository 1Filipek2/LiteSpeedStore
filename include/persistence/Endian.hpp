#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace persistence
{

/**
 * Explicit little-endian (de)serialization helpers.
 *
 * On-disk files (WAL, snapshot) write every integer byte-by-byte in
 * little-endian order so they stay portable regardless of host byte order.
 */

/// Appends @p v as 4 little-endian bytes.
inline void putLE32(std::vector<uint8_t>& buf, uint32_t v)
{
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

/// Appends @p v as 8 little-endian bytes.
inline void putLE64(std::vector<uint8_t>& buf, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
    {
        buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

/// Reads a little-endian uint32 from @p p (4 bytes).
inline uint32_t getLE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

/// Reads a little-endian uint64 from @p p (8 bytes).
inline uint64_t getLE64(const uint8_t* p)
{
    uint64_t v = 0;

    for (int i = 0; i < 8; ++i)
    {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

/// Reinterprets a double as its IEEE-754 bit pattern, so the on-disk layout is
/// independent of how the host lays out floating-point types.
inline uint64_t doubleToBits(double d)
{
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

/// Inverse of doubleToBits().
inline double bitsToDouble(uint64_t bits)
{
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

} // namespace persistence
