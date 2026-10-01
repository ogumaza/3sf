// SPDX-License-Identifier: MIT

// The console the command-line tools print to. Dropping files on 3sfrip.exe or 3sfplay.exe, or double-clicking one,
// gives the tool a console window of its own, which closes as soon as the tool exits.

#pragma once

namespace threesf
{

// Sets the Windows console to UTF-8 and restores its code page on exit. Does nothing on other platforms.
void SetUpConsole();

// Waits for Enter before the tool exits if it has a Windows console window of its own, so the output can be read. Does
// nothing elsewhere.
void KeepConsoleOpen();

} // namespace threesf
