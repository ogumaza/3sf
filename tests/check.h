// SPDX-License-Identifier: MIT

// The checks the unit tests share. A failed check is reported and counted, and the test carries on, so one run lists
// every failure.

#pragma once

#include <cstdio>
#include <string>

// The number of checks that have failed so far.
inline int failures = 0;

#define THREESF_CHECK(cond)                                                               \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

// Reports and counts a failure if `ok` is false, described by `what`, for a check whose message has to say more than
// its condition, such as which case failed.
inline void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::fprintf(stderr, "CHECK failed: %s\n", what.c_str());
        failures++;
    }
}
