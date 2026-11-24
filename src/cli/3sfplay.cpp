// SPDX-License-Identifier: MIT

// 3sfplay: renders .3sf/.mini3sf files to WAV or FLAC at the native 32728 Hz, on xsflib's play tool
// (xsflib/cli/play.h). The tool gives the options: -o for the output file of a single input, the times in place of the
// tags, the fade's curve, the album settings, --info and --version.
//
// The files are rendered one after another, and one that fails doesn't stop the rest. A .3sflib among them is skipped:
// it's a library that the others load. Dropping files on the program gives it the files as inputs.

#include "ntdlib/ctr/dsp/quiet_stdout.h"
#include "xsflib/cli/play.h"

#include "threesf/player.h"

using namespace threesf;

namespace
{

// The help's description of what the tool does.
constexpr char kDescription[] =
    "Renders each .mini3sf (or .3sf) file to a WAV file next to it, at the DSP's native 32728 Hz,\n"
    "using its length and fade tags. Libraries (.3sflib), which the .mini3sf files load, are\n"
    "skipped. You can also drop files on the program.\n";

} // namespace

int main(int argc, char** argv)
{
    xsflib::cli::PlayTool<Player> tool;
    tool.name = "3sfplay";
    tool.version = THREESF_VERSION;
    tool.description = kDescription;
    tool.output_option = "write FILE instead, as WAV or FLAC by its extension (one input only)";
    tool.library_extension = ".3sflib";
    tool.mini_extension = ".mini3sf";
    tool.a_file = "a 3SF file";
    tool.requires_a_value = "needs a value";
    tool.describe_set = xsflib::cli::DescribeLoadedChunks<Player>;
    // Teakra prints to stdout. The renders' messages go to stderr.
    tool.before_inputs = ntdlib::ctr::SilenceStdout;

    return xsflib::cli::RunPlayTool(tool, argc, argv);
}
