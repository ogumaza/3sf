// SPDX-License-Identifier: MIT

// Teakra prints unmodelled MMIO accesses to stdout. Tools that want a clean stdout call SilenceStdout() and write their
// own output to stderr (or files).

#pragma once

#include <cstdio>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace threesf
{

inline void SilenceStdout()
{
    std::fflush(stdout);
#if defined(_WIN32)
    std::freopen("NUL", "w", stdout);
#else
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0)
    {
        dup2(fd, 1);
        close(fd);
    }
#endif
}

} // namespace threesf
