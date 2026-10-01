// SPDX-License-Identifier: MIT

// 3sfplay: renders .3sf/.mini3sf files to WAV or FLAC at the native 32728 Hz.
//
// usage: 3sfplay [options] <file> [<file> ...]
//   -o, --output FILE  the output file, WAV or FLAC by its extension (default: the input's name with .wav, next to
//                      it). It takes a single input.
//   --length T         override the length tag (seconds, m:s or h:m:s)
//   --fade T           override the fade tag
//   --default T        length when there's no length tag (default and fade: PlaybackOptions)
//   --info             print the tags and loaded files, and don't render
//   --version          print the version
//
// The files are rendered one after another, and one that fails doesn't stop the rest. A .3sflib among them is skipped,
// since it's a library that the others load. Dropping files on the program gives it the files as inputs.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "cli/audio_out.h"
#include "cli/console.h"
#include "common/ascii.h"
#include "common/quiet_stdout.h"
#include "threesf/playback.h"

using namespace threesf;
namespace fs = std::filesystem;

namespace
{

// The help, with the default length (minutes and seconds) and fade (seconds) to fill in.
constexpr char kUsage[] =
    "usage: 3sfplay [options] <file> [<file> ...]\n"
    "\n"
    "Renders each .mini3sf (or .3sf) file to a WAV file next to it, at the DSP's native 32728 Hz,\n"
    "for the length and fade its tags give. A .3sflib among the files is skipped, since it's a\n"
    "library that the others load. Files can also be dropped on the program.\n"
    "\n"
    "options:\n"
    "  -o, --output FILE  write FILE instead, as WAV or FLAC by its extension (one input only)\n"
    "  --length T         play for T instead of the tagged length (seconds, m:s or h:m:s)\n"
    "  --fade T           fade out over T instead of the tagged fade\n"
    "  --default T        the length of a file without a length tag (default %lld:%02lld, with a %lld s\n"
    "                     fade)\n"
    "  --info             print the tags and the files each input loads, and don't render\n"
    "  --version          show the version\n"
    "  -h, --help         show this help\n";

// Prints the help to `out`.
void PrintUsage(std::FILE* out)
{
    std::fprintf(out, kUsage, PlaybackOptions::kDefaultLengthMs / 60000, PlaybackOptions::kDefaultLengthMs / 1000 % 60,
                 PlaybackOptions::kDefaultFadeMs / 1000);
}

struct Options
{
    PlaybackOptions playback;
    std::string out;
    bool info = false;
};

// A number of frames as a PSF time, to the nearest millisecond.
std::string FramesToTime(uint64_t frames)
{
    return FormatTime(static_cast<long long>((frames * 1000 + Playback::kSampleRate / 2) / Playback::kSampleRate));
}

// Prints the files a set loads and its tags.
void PrintInfo(Playback& playback)
{
    const auto& set = playback.GetPlayer().GetLoadedSet();
    for (const auto& s : set.sources)
    {
        std::fprintf(stderr, "file: %s\n", s.c_str());
    }

    for (const auto& [k, v] : playback.GetTags())
    {
        std::fprintf(stderr, "%s=%s\n", k.c_str(), v.c_str());
    }

    std::fprintf(stderr, "%zu memory chunks, %zu files; length %s + fade %s%s\n", set.memory.size(), set.files.size(),
                 FramesToTime(playback.PlayFrames()).c_str(), FramesToTime(playback.FadeFrames()).c_str(),
                 playback.HasLengthTag() ? "" : " (defaults)");
}

// Renders `in` to `out`, or prints what it loads with --info. Returns the exit code.
int Render(const std::string& in, const fs::path& out, const Options& o)
{
    Playback playback;
    if (!playback.Open(in, ReadWholeFile, o.playback))
    {
        std::fprintf(stderr, "error: %s\n", playback.Error().c_str());
        return 1;
    }

    if (o.info)
    {
        PrintInfo(playback);
        return 0;
    }

    const auto t0 = std::chrono::steady_clock::now();

    if (!playback.Start())
    {
        std::fprintf(stderr, "error: %s\n", playback.Error().c_str());
        return 1;
    }

    const auto t1 = std::chrono::steady_clock::now();

    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<std::size_t>(playback.LengthFrames()) * 2);
    std::vector<int16_t> block(2 * 4096);
    while (true)
    {
        const std::size_t n = playback.Render(block.data(), 4096);
        if (n == 0)
        {
            break;
        }

        pcm.insert(pcm.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(2 * n));
    }

    // After an emulation error, the audio up to it is still written, and the exit code says the render failed.
    const bool failed = playback.GetPlayer().GetState() == Player::State::kError;
    if (failed)
    {
        std::fprintf(stderr, "error after %.2f s: %s\n",
                     playback.Position() / static_cast<double>(Playback::kSampleRate), playback.Error().c_str());
        if (pcm.empty())
        {
            return 1;
        }
    }

    const auto t2 = std::chrono::steady_clock::now();
    const double boot = std::chrono::duration<double>(t1 - t0).count();
    const double secs = std::chrono::duration<double>(t2 - t1).count();

    if (const auto err = WriteAudio(out.string(), pcm, Playback::kSampleRate))
    {
        std::fprintf(stderr, "error: %s\n", err->c_str());
        return 1;
    }

    // The macOS droplet shows the line before "output:" and opens the output's folder.
    const double audio = pcm.size() / 2.0 / Playback::kSampleRate;
    std::fprintf(stderr, "%s: %.2f s rendered in %.2f s (%.2fx real time) after %.2f s of boot\n",
                 out.filename().string().c_str(), audio, secs, audio / secs, boot);
    std::fprintf(stderr, "output: %s\n", fs::absolute(out).lexically_normal().string().c_str());

    return failed ? 1 : 0;
}

// Reads the options and inputs. Returns 0 to go on, -1 when there's nothing more to do (after --help or --version), or
// the exit code.
int ParseArgs(int argc, char** argv, Options& o, std::vector<std::string>& inputs)
{
    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        long long* time = nullptr;
        if (a == "-h" || a == "--help")
        {
            PrintUsage(stdout);
            return -1;
        }
        else if (a == "--version")
        {
            std::puts("3sfplay " THREESF_VERSION);
            return -1;
        }
        else if (a == "-o" || a == "--output")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
                return 2;
            }

            o.out = argv[++i];
        }
        else if (a == "--length")
        {
            time = &o.playback.length_override_ms;
        }
        else if (a == "--fade")
        {
            time = &o.playback.fade_override_ms;
        }
        else if (a == "--default")
        {
            time = &o.playback.default_length_ms;
        }
        else if (a == "--info")
        {
            o.info = true;
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            std::fprintf(stderr, "error: unknown option %s (see --help)\n", a.c_str());
            return 2;
        }
        else
        {
            inputs.push_back(a);
        }

        if (time)
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
                return 2;
            }

            const std::string v = argv[++i];
            *time = ParseTime(v);
            if (*time < 0)
            {
                std::fprintf(stderr, "error: bad time for %s: '%s'\n", a.c_str(), v.c_str());
                return 2;
            }
        }
    }

    if (inputs.empty())
    {
        PrintUsage(stderr);
        return 2;
    }

    if (!o.out.empty() && inputs.size() > 1)
    {
        std::fprintf(stderr, "error: -o names one output file, so it takes a single input\n");
        return 2;
    }

    return 0;
}

// Returns true if `path` names a .3sflib.
bool IsLibrary(const std::string& path)
{
    return AsciiLower(fs::path(path).extension().string()) == ".3sflib";
}

int Run(int argc, char** argv)
{
    Options o;
    std::vector<std::string> inputs;
    if (const int parsed = ParseArgs(argc, argv, o, inputs))
    {
        return parsed < 0 ? 0 : parsed;
    }

    // An input that isn't there stops everything before anything is written. A dropped file is always there, so this
    // catches mistyped paths, and an output given the old way, after the input.
    for (std::size_t i = 0; i < inputs.size(); i++)
    {
        std::error_code ec;
        if (!fs::exists(inputs[i], ec))
        {
            std::fprintf(stderr, "error: %s: no such file or folder%s\n", inputs[i].c_str(),
                         i ? " (-o names the output file)" : "");
            return 1;
        }
    }

    SilenceStdout(); // Teakra reports unmodelled MMIO on stdout

    // Render each input once, continuing after errors. Return the highest exit code.
    std::set<fs::path> done;
    int result = 0;
    for (std::size_t i = 0; i < inputs.size(); i++)
    {
        const std::string& in = inputs[i];
        if (inputs.size() > 1)
        {
            // The name of a folder given with a slash at the end is the folder's.
            const fs::path path = fs::absolute(in).lexically_normal();
            const fs::path name = path.has_filename() ? path.filename() : path.parent_path().filename();
            std::fprintf(stderr, "%s%s:\n", i ? "\n" : "", name.string().c_str());
        }

        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(in, ec);
        if (!done.insert(ec ? fs::absolute(in) : canonical).second)
        {
            std::fprintf(stderr, "skipped, since it's given more than once\n");
            continue;
        }

        if (fs::is_directory(in, ec))
        {
            std::fprintf(stderr, "error: %s is a folder, not a 3SF file\n", in.c_str());
            result = std::max(result, 1);
            continue;
        }

        if (IsLibrary(in))
        {
            std::fprintf(stderr, "skipped %s: it's the library that the .mini3sf files load\n", in.c_str());
            continue;
        }

        const fs::path out = o.out.empty() ? fs::path(in).replace_extension(".wav") : fs::path(o.out);
        try
        {
            result = std::max(result, Render(in, out, o));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", in.c_str(), e.what());
            result = std::max(result, 1);
        }
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    SetUpConsole();

    // Report unhandled errors, including allocation failures during long renders.
    int result = 1;
    try
    {
        result = Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
    }

    KeepConsoleOpen();

    return result;
}
