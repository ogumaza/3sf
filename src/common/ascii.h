// SPDX-License-Identifier: MIT

// ASCII lowercasing for tag names, file names and extensions. Unlike std::tolower it doesn't depend on the C locale,
// which a host program such as foobar2000 may have changed.

#pragma once

#include <string>

namespace threesf
{

inline std::string AsciiLower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return s;
}

} // namespace threesf
