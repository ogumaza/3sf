// SPDX-License-Identifier: MIT

// Unit tests for the names 3sfrip gives the files of a rip (rip::FileNames).

#include <cstdio>
#include <string>

#include "check.h"
#include "rip/rip.h"

using namespace threesf;

static void TestCharacters()
{
    rip::FileNames names;

    const std::string library = names.Library("Some Game: Japan?");
    const std::string mini = names.Mini(7, "SEQ BGM/1");

    THREESF_CHECK(library == "Some_Game__Japan_.3sflib");
    THREESF_CHECK(mini == "0007 SEQ_BGM_1.mini3sf");
}

static void TestReservedNames()
{
    rip::FileNames names;

    THREESF_CHECK(names.Library("con") == "con_.3sflib");
    THREESF_CHECK(names.Library("NUL") == "NUL_.3sflib");
    THREESF_CHECK(names.Library("COM0") == "COM0_.3sflib");
    THREESF_CHECK(names.Library("LPT1.sound") == "LPT1_.sound.3sflib");
    THREESF_CHECK(names.Library("COM10") == "COM10.3sflib");
    THREESF_CHECK(names.Library("CONSOLE") == "CONSOLE.3sflib");
}

static void TestNamesDifferInMoreThanCase()
{
    rip::FileNames names;

    const std::string first = names.Library("Game Sound");
    const std::string second = names.Library("game sound");
    const std::string third = names.Library("GAME_SOUND");
    const std::string first_mini = names.Mini(1, "SE");
    const std::string second_mini = names.Mini(1, "se");

    THREESF_CHECK(first == "Game_Sound.3sflib");
    THREESF_CHECK(second == "game_sound_2.3sflib");
    THREESF_CHECK(third == "GAME_SOUND_3.3sflib");
    THREESF_CHECK(first_mini == "0001 SE.mini3sf");
    THREESF_CHECK(second_mini == "0001 se_2.mini3sf");
}

int main()
{
    TestCharacters();
    TestReservedNames();
    TestNamesDifferInMoreThanCase();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("file name tests passed\n");

    return 0;
}
