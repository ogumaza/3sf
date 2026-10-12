// SPDX-License-Identifier: MIT

// How 3SF reads sound archives with ntdlib::sar: the 3DS's files, with the checks and messages of 3SF's player.

#pragma once

#include "ntdlib/sar/formats.h"
#include "xsflib/binary.h"

namespace threesf
{

inline const ntdlib::sar::Options kSarOptions{ntdlib::sar::Flavor::kCtr,
                                              xsflib::ReaderOptions{xsflib::BoundsMessage::kCutShort}, false,
                                              ntdlib::sar::Rules::kStopAtFirstProblem};

} // namespace threesf
