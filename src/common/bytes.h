// SPDX-License-Identifier: MIT

// Little-endian loads and stores on byte buffers. The 3DS is little-endian, and so is every file format 3SF reads and
// writes, so these give the same values on any host.

#pragma once

#include <cstdint>

namespace threesf
{

inline uint16_t LoadLe16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t LoadLe32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t LoadLe64(const uint8_t* p)
{
    return LoadLe32(p) | (static_cast<uint64_t>(LoadLe32(p + 4)) << 32);
}

inline void StoreLe16(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

inline void StoreLe32(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

inline void StoreLe64(uint8_t* p, uint64_t value)
{
    StoreLe32(p, static_cast<uint32_t>(value));
    StoreLe32(p + 4, static_cast<uint32_t>(value >> 32));
}

} // namespace threesf
