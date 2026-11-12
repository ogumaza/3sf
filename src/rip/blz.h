// SPDX-License-Identifier: MIT

// The 3DS's "BLZ" compression of a program's .code.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace threesf::rip
{

// Decompresses 3DS "BLZ" (bottom-up LZSS, as used for a compressed .code). Returns nullopt if the data isn't valid BLZ.
std::optional<std::vector<uint8_t>> BlzDecompress(const std::vector<uint8_t>& data);

} // namespace threesf::rip
