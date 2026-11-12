// SPDX-License-Identifier: MIT

// A check that format_test and aux_effects_test share: a parser of a tag's text refuses each text of a list.

#pragma once

#include <initializer_list>
#include <string>

#include "xsflib/testing/check.h"

namespace threesf_test
{

// Checks that `parse` refuses each text of `bad`.
template <typename Parse>
void CheckRefused(Parse parse, std::initializer_list<const char*> bad)
{
    for (const char* text : bad)
    {
        xsflib::testing::Check(!parse(text), std::string("\"") + text + "\" is refused");
    }
}

} // namespace threesf_test
