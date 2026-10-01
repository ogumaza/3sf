// SPDX-License-Identifier: MIT

// Address types for the emulated 3DS.

#pragma once

#include <cstdint>

namespace threesf
{

// Physical address (3DS FCRAM is mapped at 0x20000000).
using PAddr = uint32_t;

} // namespace threesf
