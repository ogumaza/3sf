// SPDX-License-Identifier: MIT

// The 3SF file format (see format.h and docs/3sf.md).

#include "threesf/format.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/ascii.h"
#include "common/bytes.h"

namespace threesf
{
namespace
{

std::string Hex(uint32_t value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", static_cast<unsigned>(value));

    return buf;
}

// Diagnostic suffixes for unsupported descriptor flags and output modes.
const char* FlagsExpected()
{
    return "; version 1 requires only bit 0 (DSP required) to be set";
}

const char* OutputModeExpected()
{
    return "; version 1 supports only stereo (1)";
}

// The byte that a set's MEM chunks leave at `address`, or nothing if none of them covers it. A later chunk overwrites
// an earlier one, as when the set is loaded.
std::optional<uint8_t> LoadedByte(const std::vector<MemChunk>& memory, uint32_t address)
{
    for (auto it = memory.rbegin(); it != memory.rend(); ++it)
    {
        if (address >= it->address && address - it->address < it->data.size())
        {
            return it->data[address - it->address];
        }
    }

    return std::nullopt;
}

std::optional<uint32_t> LoadedWord(const std::vector<MemChunk>& memory, uint32_t address)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        const std::optional<uint8_t> byte = LoadedByte(memory, address + i);
        if (!byte)
        {
            return std::nullopt;
        }

        value |= uint32_t{*byte} << (8 * i);
    }

    return value;
}

// Returns why a version 1 player can't play a game-mode set with this driver block, or nothing if it can. A set without
// the block (no magic at its address) passes: it doesn't use 3SF's driver.
std::optional<std::string> CheckDriverBlock(const std::vector<MemChunk>& memory)
{
    if (LoadedWord(memory, DriverBlock::kAddress) != DriverBlock::kMagic)
    {
        return std::nullopt;
    }

    const std::optional<uint32_t> version = LoadedWord(memory, DriverBlock::kAddress + DriverBlock::kVersionOffset);
    const std::optional<uint32_t> mode = LoadedWord(memory, DriverBlock::kAddress + DriverBlock::kOutputModeOffset);
    if (!version || !mode)
    {
        return "the driver block is cut short";
    }

    if (*version != DriverBlock::kVersion)
    {
        return "driver block version " + std::to_string(*version) + "; this player supports version " +
               std::to_string(DriverBlock::kVersion);
    }

    if (*mode != kOutputStereo)
    {
        return "the driver block's output mode is " + std::to_string(*mode) + OutputModeExpected();
    }

    return std::nullopt;
}

// Times past these are taken as malformed. They keep the arithmetic far from overflowing, here and where the players
// turn times into sample counts.
constexpr long long kMaxTimeField = 1'000'000'000; // hours or minutes
constexpr double kMaxTimeMs = 1e13;                // about 317 years

// Limits on a set, so that a malformed one can't make the loader recurse, load or inflate without end: _lib nesting,
// files in a set, and the size of one file's program.
constexpr int kMaxLibDepth = 10;
constexpr std::size_t kMaxSetFiles = 64;
constexpr std::size_t kMaxProgramSize = std::size_t{1} << 30;

void Put32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; i++)
    {
        v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    }
}

std::string Trim(const std::string& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= 0x20)
    {
        a++;
    }

    while (b > a && static_cast<unsigned char>(s[b - 1]) <= 0x20)
    {
        b--;
    }

    return s.substr(a, b - a);
}

Tags ParseTags(const char* text, std::size_t size)
{
    Tags tags;
    std::string last;
    std::size_t pos = 0;
    while (pos < size)
    {
        std::size_t end = pos;
        while (end < size && text[end] != '\n')
        {
            end++;
        }

        std::string line(text + pos, end - pos);
        pos = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }

        const std::string name = AsciiLower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        if (name.empty())
        {
            continue;
        }

        auto it = tags.find(name);
        if (it != tags.end() && name == last)
        {
            it->second += "\n" + value; // multi-line value
        }
        else
        {
            tags[name] = value;
        }

        last = name;
    }

    return tags;
}

// The directory part of `path`, with its trailing separator. Paths only go to a FileReader, which may not use the file
// system (foobar2000 passes its own paths), so they stay strings.
std::string DirectoryOf(const std::string& path, std::string_view separators)
{
    const std::size_t separator = path.find_last_of(separators);
    return separator == std::string::npos ? std::string() : path.substr(0, separator + 1);
}

} // namespace

std::optional<std::size_t> TagAreaOffset(const std::vector<uint8_t>& data)
{
    if (data.size() < 16 || std::memcmp(data.data(), "PSF", 3) != 0)
    {
        return std::nullopt;
    }

    const uint64_t end = 16ull + LoadLe32(&data[4]) + LoadLe32(&data[8]);
    if (end > data.size())
    {
        return std::nullopt;
    }

    return static_cast<std::size_t>(end);
}

std::optional<std::string> ParsePsf(const std::vector<uint8_t>& data, PsfFile& out, bool with_program)
{
    if (data.size() < 16 || std::memcmp(data.data(), "PSF", 3) != 0)
    {
        return "not a PSF file";
    }

    out.version = data[3];
    const uint32_t reserved_size = LoadLe32(&data[4]);
    const uint32_t program_size = LoadLe32(&data[8]);
    const uint32_t crc = LoadLe32(&data[12]);
    if (16ull + reserved_size + program_size > data.size())
    {
        return "truncated PSF file";
    }

    out.reserved.assign(data.begin() + 16, data.begin() + 16 + reserved_size);
    out.program.clear();
    out.tags.clear();

    if (program_size && with_program)
    {
        const uint8_t* compressed = data.data() + 16 + reserved_size;
        if (crc32(0, compressed, program_size) != crc)
        {
            return "program CRC mismatch";
        }

        z_stream zs{};
        if (inflateInit(&zs) != Z_OK)
        {
            return "zlib init failed";
        }

        zs.next_in = const_cast<uint8_t*>(compressed);
        zs.avail_in = program_size;
        std::vector<uint8_t> buf(1 << 20);
        int ret;
        do
        {
            zs.next_out = buf.data();
            zs.avail_out = static_cast<uInt>(buf.size());
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END)
            {
                inflateEnd(&zs);
                return "corrupt program data";
            }

            const std::size_t produced = buf.size() - zs.avail_out;
            if (produced > kMaxProgramSize - out.program.size())
            {
                inflateEnd(&zs);
                return "program larger than 1 GiB";
            }

            out.program.insert(out.program.end(), buf.data(), buf.data() + produced);
        } while (ret != Z_STREAM_END && (zs.avail_in > 0 || zs.avail_out == 0));

        inflateEnd(&zs);
        if (ret != Z_STREAM_END)
        {
            return "truncated program data";
        }
    }

    const std::size_t tag_start = 16ull + reserved_size + program_size;
    if (data.size() >= tag_start + 5 && std::memcmp(data.data() + tag_start, "[TAG]", 5) == 0)
    {
        out.tags = ParseTags(reinterpret_cast<const char*>(data.data() + tag_start + 5), data.size() - tag_start - 5);
    }

    return std::nullopt;
}

std::vector<uint8_t> WritePsf(uint8_t version, const std::vector<uint8_t>& reserved,
                              const std::vector<uint8_t>& program, const Tags& tags)
{
    std::vector<uint8_t> compressed;
    uint32_t crc = 0;
    if (!program.empty())
    {
        uLongf size = compressBound(static_cast<uLong>(program.size()));
        compressed.resize(size);
        if (compress2(compressed.data(), &size, program.data(), static_cast<uLong>(program.size()),
                      Z_BEST_COMPRESSION) != Z_OK)
        {
            throw std::runtime_error("zlib couldn't compress the program");
        }

        compressed.resize(size);
        crc = static_cast<uint32_t>(crc32(0, compressed.data(), static_cast<uInt>(compressed.size())));
    }

    std::vector<uint8_t> out = {'P', 'S', 'F', version};
    Put32(out, static_cast<uint32_t>(reserved.size()));
    Put32(out, static_cast<uint32_t>(compressed.size()));
    Put32(out, crc);
    out.insert(out.end(), reserved.begin(), reserved.end());
    out.insert(out.end(), compressed.begin(), compressed.end());

    const std::string text = FormatTagArea(tags);
    out.insert(out.end(), text.begin(), text.end());

    return out;
}

std::string FormatTagArea(const Tags& tags)
{
    if (tags.empty())
    {
        return {};
    }

    std::string text = "[TAG]";
    for (const auto& [k, v] : tags)
    {
        std::size_t start = 0;
        while (true)
        {
            const std::size_t nl = v.find('\n', start);
            text += k + "=" + v.substr(start, nl == std::string::npos ? std::string::npos : nl - start) + "\n";
            if (nl == std::string::npos)
            {
                break;
            }

            start = nl + 1;
        }
    }

    return text;
}

std::optional<DecimalNumber> ReadDecimal(std::string_view text)
{
    std::size_t at = 0;
    const bool negative = !text.empty() && text[0] == '-';
    if (!text.empty() && (text[0] == '-' || text[0] == '+'))
    {
        at++;
    }

    // Up to 19 significant digits fit in 64 bits. Later digits before the decimal point scale the value, and
    // later digits after it are dropped.
    constexpr int kMaxDigits = 19;
    uint64_t digits = 0;
    int significant = 0;
    int exponent = 0; // of ten
    bool any_digit = false;
    bool point = false;
    for (; at < text.size(); at++)
    {
        const char c = text[at];
        if ((c == '.' || c == ',') && !point)
        {
            point = true;
            continue;
        }

        if (c < '0' || c > '9')
        {
            break;
        }

        any_digit = true;
        if (significant < kMaxDigits)
        {
            significant += digits != 0 || c != '0' ? 1 : 0;
            digits = digits * 10 + static_cast<uint64_t>(c - '0');
            exponent -= point ? 1 : 0;
        }
        else if (!point)
        {
            exponent++;
        }
    }

    if (!any_digit)
    {
        return std::nullopt;
    }

    // An exponent counts only with a digit: "1e" is the number 1 followed by a letter.
    constexpr int kMaxExponent = 100'000;
    std::size_t end = at;
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E'))
    {
        std::size_t e = at + 1;
        const bool negative_exponent = e < text.size() && text[e] == '-';
        if (e < text.size() && (text[e] == '-' || text[e] == '+'))
        {
            e++;
        }

        const std::size_t first = e;
        int value = 0;
        for (; e < text.size() && text[e] >= '0' && text[e] <= '9'; e++)
        {
            value = std::min(value * 10 + (text[e] - '0'), kMaxExponent);
        }

        if (e > first)
        {
            exponent += negative_exponent ? -value : value;
            end = e;
        }
    }

    // While the digits fit in a double's 53 bits and the power of ten is at most 10^22, both are exact, and one
    // multiplication or division rounds the value once and gives what std::strtod gives. Longer numbers and larger
    // exponents round more than once and can differ from std::strtod in the last bit.
    constexpr std::array<double, 23> kPowersOfTen = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                                     1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                                     1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    constexpr int kLargestPower = static_cast<int>(kPowersOfTen.size()) - 1;
    double value = static_cast<double>(digits);
    for (; exponent > kLargestPower; exponent -= kLargestPower)
    {
        value *= kPowersOfTen[kLargestPower];
    }

    for (; exponent < -kLargestPower; exponent += kLargestPower)
    {
        value /= kPowersOfTen[kLargestPower];
    }

    value = exponent < 0 ? value / kPowersOfTen[-exponent] : value * kPowersOfTen[exponent];

    return DecimalNumber{negative ? -value : value, end};
}

long long ParseTime(const std::string& text)
{
    const std::string time = Trim(text);
    if (time.empty())
    {
        return -1;
    }

    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t colon = time.find(':', start);
        parts.push_back(time.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (colon == std::string::npos)
        {
            break;
        }

        start = colon + 1;
    }

    if (parts.size() > 3)
    {
        return -1;
    }

    long long whole = 0; // hours and minutes, in minutes
    for (std::size_t i = 0; i + 1 < parts.size(); i++)
    {
        char* end = nullptr;
        const long long v = std::strtoll(parts[i].c_str(), &end, 10);
        if (parts[i].empty() || !end || *end || v < 0 || v > kMaxTimeField)
        {
            return -1;
        }

        whole = whole * 60 + v;
    }

    const std::string seconds_text = Trim(parts.back());
    const std::optional<DecimalNumber> seconds = ReadDecimal(seconds_text);
    if (!seconds || seconds->length != seconds_text.size())
    {
        return -1;
    }

    // The comparisons are written so that infinity fails them.
    const double ms = (static_cast<double>(whole) * 60.0 + seconds->value) * 1000.0 + 0.5;
    if (!(seconds->value >= 0) || !(ms <= kMaxTimeMs))
    {
        return -1;
    }

    return static_cast<long long>(ms);
}

std::string FormatTime(long long ms)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);

    return buf;
}

namespace
{

// A whole number in decimal, or in hex with a 0x prefix, either with an optional minus sign. Unlike std::strtol, this
// doesn't depend on the C locale, and it takes no white space or other characters around the number.
std::optional<int64_t> ParseInteger(std::string_view text)
{
    const bool negative = text.starts_with('-');
    if (negative)
    {
        text.remove_prefix(1);
    }

    const bool hex = text.starts_with("0x") || text.starts_with("0X");
    if (hex)
    {
        text.remove_prefix(2);
    }

    uint32_t n = 0;
    const char* last = text.data() + text.size();
    const auto [end, error] = std::from_chars(text.data(), last, n, hex ? 16 : 10);
    if (text.empty() || error != std::errc{} || end != last)
    {
        return std::nullopt;
    }

    return negative ? -static_cast<int64_t>(n) : static_cast<int64_t>(n);
}

} // namespace

std::optional<Variables> ParseVariables(std::string_view text)
{
    constexpr std::string_view kSeparators = ", \t\r\n";

    Variables variables;
    std::size_t at = 0;
    while ((at = text.find_first_not_of(kSeparators, at)) != std::string_view::npos)
    {
        const std::size_t end = std::min(text.find_first_of(kSeparators, at), text.size());
        const std::string_view assignment = text.substr(at, end - at);
        at = end;

        const std::size_t equals = assignment.find('=');
        if (equals == std::string_view::npos)
        {
            return std::nullopt;
        }

        const std::optional<int64_t> index = ParseInteger(assignment.substr(0, equals));
        const std::optional<int64_t> value = ParseInteger(assignment.substr(equals + 1));
        if (!index || !value || *index < 0 || *index > 31 || *value < INT16_MIN || *value > INT16_MAX)
        {
            return std::nullopt;
        }

        variables[static_cast<int>(*index)] = static_cast<int16_t>(*value);
    }

    if (variables.empty())
    {
        return std::nullopt;
    }

    return variables;
}

std::string FormatVariables(const Variables& variables)
{
    std::string text;
    for (const auto& [index, value] : variables)
    {
        text += (text.empty() ? "" : ", ") + std::to_string(index) + "=" + std::to_string(value);
    }

    return text;
}

std::vector<uint8_t> ProcessDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    Put32(v, kMagic);
    Put32(v, version);
    Put32(v, entry);
    Put32(v, stack_size);
    Put32(v, priority);
    Put32(v, app_memory);
    Put32(v, status_address);
    Put32(v, flags);

    return v;
}

std::optional<ProcessDescriptor> ProcessDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 32 || LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ProcessDescriptor d;
    d.version = LoadLe32(&data[4]);
    d.entry = LoadLe32(&data[8]);
    d.stack_size = LoadLe32(&data[12]);
    d.priority = LoadLe32(&data[16]);
    d.app_memory = LoadLe32(&data[20]);
    d.status_address = LoadLe32(&data[24]);
    d.flags = LoadLe32(&data[28]);

    return d;
}

std::optional<std::string> ProcessDescriptor::Check() const
{
    if (version != kVersion)
    {
        return "process descriptor version " + std::to_string(version) + "; this player supports version " +
               std::to_string(kVersion);
    }

    if (app_memory == 0)
    {
        return "the process descriptor gives the program no application memory";
    }

    if (app_memory > kMaxAppMemory)
    {
        return "the process descriptor asks for " + std::to_string(app_memory) +
               " bytes of application memory, more than the 178 MiB a 3DS gives a program";
    }

    if (flags != kFlagDspRequired)
    {
        return "the process descriptor's flags are " + Hex(flags) + FlagsExpected();
    }

    return std::nullopt;
}

std::vector<uint8_t> ArchiveDescriptor::Serialize() const
{
    std::vector<uint8_t> v;
    Put32(v, kMagic);
    Put32(v, version);
    Put32(v, flags);
    Put32(v, output_mode);

    for (const std::string* path : {&archive_path, &firmware_path})
    {
        v.push_back(static_cast<uint8_t>(path->size()));
        v.push_back(static_cast<uint8_t>(path->size() >> 8));
        v.insert(v.end(), path->begin(), path->end());
    }

    while (v.size() % 4)
    {
        v.push_back(0);
    }

    return v;
}

std::optional<ArchiveDescriptor> ArchiveDescriptor::Parse(const std::vector<uint8_t>& data)
{
    if (data.size() < 20 || LoadLe32(data.data()) != kMagic)
    {
        return std::nullopt;
    }

    ArchiveDescriptor d;
    d.version = LoadLe32(&data[4]);
    d.flags = LoadLe32(&data[8]);
    d.output_mode = LoadLe32(&data[12]);

    std::size_t pos = 16;
    for (std::string* path : {&d.archive_path, &d.firmware_path})
    {
        if (pos + 2 > data.size())
        {
            return std::nullopt;
        }

        const std::size_t len = data[pos] | (data[pos + 1] << 8);
        pos += 2;
        if (pos + len > data.size())
        {
            return std::nullopt;
        }

        path->assign(reinterpret_cast<const char*>(&data[pos]), len);
        pos += len;
    }

    return d;
}

std::optional<std::string> ArchiveDescriptor::Check() const
{
    if (version != kVersion)
    {
        return "archive descriptor version " + std::to_string(version) + "; this player supports version " +
               std::to_string(kVersion);
    }

    if (flags != kFlagDspRequired)
    {
        return "the archive descriptor's flags are " + Hex(flags) + FlagsExpected();
    }

    if (output_mode != kOutputStereo)
    {
        return "the archive descriptor's output mode is " + std::to_string(output_mode) + OutputModeExpected();
    }

    return std::nullopt;
}

void ProgramBuilder::Chunk(const char type[4], const std::vector<uint8_t>& payload)
{
    program_.insert(program_.end(), type, type + 4);
    Put32(program_, static_cast<uint32_t>(payload.size()));
    program_.insert(program_.end(), payload.begin(), payload.end());

    while (program_.size() % 4)
    {
        program_.push_back(0);
    }
}

void ProgramBuilder::AddMemory(uint32_t address, const void* data, std::size_t size)
{
    std::vector<uint8_t> payload;
    Put32(payload, address);
    const auto* p = static_cast<const uint8_t*>(data);
    payload.insert(payload.end(), p, p + size);
    Chunk("MEM ", payload);
}

void ProgramBuilder::AddFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(path.size()));
    payload.push_back(static_cast<uint8_t>(path.size() >> 8));
    payload.insert(payload.end(), path.begin(), path.end());

    while (payload.size() % 4)
    {
        payload.push_back(0);
    }

    payload.insert(payload.end(), data.begin(), data.end());
    Chunk("FILE", payload);
}

void ProgramBuilder::AddSound(uint32_t sound_id)
{
    std::vector<uint8_t> payload;
    Put32(payload, sound_id);
    Chunk("SND ", payload);
}

std::optional<std::string> ParseProgram(const std::vector<uint8_t>& program, ProgramChunks& out)
{
    auto& mem = out.memory;
    auto& files = out.files;
    std::size_t pos = 0;
    while (pos + 8 <= program.size())
    {
        const uint8_t* type = &program[pos];
        const uint32_t size = LoadLe32(&program[pos + 4]);
        pos += 8;
        if (size > program.size() - pos)
        {
            return "truncated chunk";
        }

        const uint8_t* payload = &program[pos];
        if (std::memcmp(type, "MEM ", 4) == 0)
        {
            if (size < 4)
            {
                return "bad MEM chunk";
            }

            mem.push_back({LoadLe32(payload), std::vector<uint8_t>(payload + 4, payload + size)});
        }
        else if (std::memcmp(type, "FILE", 4) == 0)
        {
            if (size < 2)
            {
                return "bad FILE chunk";
            }

            const uint32_t len = payload[0] | (payload[1] << 8);
            const uint32_t data_start = (2 + len + 3) & ~3u;
            if (data_start > size)
            {
                return "bad FILE chunk";
            }

            files.push_back({std::string(reinterpret_cast<const char*>(payload + 2), len),
                             std::vector<uint8_t>(payload + data_start, payload + size)});
        }
        else if (std::memcmp(type, "SND ", 4) == 0)
        {
            if (size < 4)
            {
                return "bad SND chunk";
            }

            out.sound = LoadLe32(payload);
        }

        pos += (size + 3) & ~3u;
    }

    return std::nullopt;
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& data)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }

    // One read of the whole file: copying it a byte at a time took 170 ms for an 18 MB library.
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0);
    if (size < 0 || !f)
    {
        // Read streams with unknown sizes, such as pipes, until EOF.
        f.clear();
        data.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }

    data.resize(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    data.resize(static_cast<std::size_t>(f.gcount()));

    return true;
}

namespace
{

std::optional<std::string> LoadRecursive(const std::string& path, const FileReader& reader, std::string_view separators,
                                         LoadedSet& out, int depth, bool top)
{
    if (depth > kMaxLibDepth)
    {
        return "_lib nesting too deep";
    }

    if (out.sources.size() >= kMaxSetFiles)
    {
        return "more than 64 files in the set";
    }

    std::vector<uint8_t> data;
    if (!reader(path, data))
    {
        return "can't read " + path;
    }

    PsfFile psf;
    if (auto err = ParsePsf(data, psf))
    {
        return path + ": " + *err;
    }

    if (psf.version != kPsfVersion3sf)
    {
        char version[8];
        std::snprintf(version, sizeof(version), "%02x", psf.version);
        return path + ": not a 3SF file (PSF version 0x" + version + ")";
    }

    const std::string dir = DirectoryOf(path, separators);
    auto lib = psf.tags.find("_lib");
    if (lib != psf.tags.end() && !lib->second.empty())
    {
        if (auto err = LoadRecursive(dir + lib->second, reader, separators, out, depth + 1, false))
        {
            return err;
        }
    }

    ProgramChunks chunks;
    if (auto err = ParseProgram(psf.program, chunks))
    {
        return path + ": " + *err;
    }

    for (auto& m : chunks.memory)
    {
        out.memory.push_back(std::move(m));
    }

    for (auto& f : chunks.files)
    {
        out.files.push_back(std::move(f));
    }

    if (chunks.sound)
    {
        out.sound = chunks.sound;
    }

    // The last descriptor in loading order decides the mode.
    if (auto d = ProcessDescriptor::Parse(psf.reserved))
    {
        if (auto err = d->Check())
        {
            return path + ": " + *err;
        }

        out.descriptor = d;
        out.archive.reset();
    }
    else if (auto a = ArchiveDescriptor::Parse(psf.reserved))
    {
        if (auto err = a->Check())
        {
            return path + ": " + *err;
        }

        out.archive = a;
        out.descriptor.reset();
    }

    out.sources.push_back(path);

    for (int n = 2;; n++)
    {
        auto it = psf.tags.find("_lib" + std::to_string(n));
        if (it == psf.tags.end())
        {
            break;
        }

        // An empty one names no library, as an empty _lib does.
        if (it->second.empty())
        {
            continue;
        }

        if (auto err = LoadRecursive(dir + it->second, reader, separators, out, depth + 1, false))
        {
            return err;
        }
    }

    if (top)
    {
        out.tags = psf.tags;
    }

    return std::nullopt;
}

} // namespace

std::optional<std::string> LoadSet(const std::string& path, const FileReader& reader, LoadedSet& out,
                                   std::string_view separators)
{
    out = LoadedSet{};

    if (auto err = LoadRecursive(path, reader, separators, out, 0, true))
    {
        return err;
    }

    if (!out.descriptor && !out.archive)
    {
        return "no process or archive descriptor in " + path + " or its libraries";
    }

    if (out.archive && !out.sound)
    {
        return "no sound selected (SND chunk) in " + path + " or its libraries";
    }

    if (out.descriptor)
    {
        if (auto err = CheckDriverBlock(out.memory))
        {
            return path + ": " + *err;
        }
    }

    return std::nullopt;
}

} // namespace threesf
