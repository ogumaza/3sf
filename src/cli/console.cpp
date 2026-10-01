// SPDX-License-Identifier: MIT

#include "cli/console.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace threesf
{
namespace
{

#ifdef _WIN32
UINT original_output_cp = 0; // the console's code page before SetUpConsole, or 0 if it wasn't changed

void RestoreConsole()
{
    if (original_output_cp != 0)
    {
        SetConsoleOutputCP(original_output_cp);
    }
}

// Restores the code page when Ctrl+C or closing the window ends the tool, which skips the atexit functions, and then
// lets the default handler end the process.
BOOL WINAPI OnConsoleControl(DWORD)
{
    RestoreConsole();

    return FALSE;
}
#endif

} // namespace

void SetUpConsole()
{
#ifdef _WIN32
    const UINT cp = GetConsoleOutputCP();
    if (cp != 0 && cp != CP_UTF8 && SetConsoleOutputCP(CP_UTF8))
    {
        original_output_cp = cp;
        std::atexit(RestoreConsole);
        SetConsoleCtrlHandler(OnConsoleControl, TRUE);
    }
#endif
}

void KeepConsoleOpen()
{
#ifdef _WIN32
    // A console that Explorer made for the tool has only the tool's process on it. Standard output is silenced (see
    // SilenceStdout), so the prompt goes to standard error.
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) == 1)
    {
        std::fprintf(stderr, "\nPress Enter to close this window.");
        std::getchar();
    }
#endif
}

} // namespace threesf
