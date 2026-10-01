#pragma once
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include "common_types.h"

// Throws rather than aborting, so that a host (a player plug-in) survives an emulation failure.
// The exception carries the message, so it isn't printed here as well.
[[noreturn]] TEAKRA_NOINLINE inline void Assert(const char* expression, const char* file,
                                                int line) {
    throw std::runtime_error(std::string("Teakra: assertion '") + expression + "' failed, file '" +
                             file + "' line " + std::to_string(line));
}

#define ASSERT(EXPRESSION) ((EXPRESSION) ? (void)0 : Assert(#EXPRESSION, __FILE__, __LINE__))
#define UNREACHABLE() Assert("UNREACHABLE", __FILE__, __LINE__)
