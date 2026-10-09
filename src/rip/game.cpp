// SPDX-License-Identifier: MIT

// Loads a game for ripping (see game.h). The container formats (NCSD, NCCH, ExeFS, IVFC/RomFS, CIA and TMD) are as
// 3dbrew documents them.

#include "rip/game.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/ascii.h"
#include "common/bytes.h"

namespace threesf::rip
{
namespace
{

namespace fs = std::filesystem;

constexpr uint64_t kMaxFileSize = 1ull << 30; // largest single file read into memory

// An image's media unit is 0x200 << n bytes. Real images use 0; anything past this would overflow the offsets.
constexpr uint8_t kMaxMediaUnitShift = 16;

uint16_t Be16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

uint64_t Be64(const uint8_t* p)
{
    return (static_cast<uint64_t>(Be32(p)) << 32) | Be32(p + 4);
}

uint64_t AlignUp(uint64_t x, uint64_t a)
{
    return (x + a - 1) / a * a;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& data)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }

    f.seekg(0, std::ios::end);
    const auto size = static_cast<uint64_t>(f.tellg());
    if (size > kMaxFileSize)
    {
        return false;
    }

    f.seekg(0);
    data.resize(static_cast<std::size_t>(size));

    return !size || static_cast<bool>(f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)));
}

// Random access to an image file on disk.
class Image
{
public:
    explicit Image(const std::string& path) : file_(path, std::ios::binary)
    {
        if (file_)
        {
            file_.seekg(0, std::ios::end);
            size_ = static_cast<uint64_t>(file_.tellg());
        }
    }

    bool Ok() const
    {
        return static_cast<bool>(file_);
    }

    uint64_t Size() const
    {
        return size_;
    }

    bool Read(uint64_t offset, void* dst, uint64_t n)
    {
        if (offset > size_ || n > size_ - offset)
        {
            return false;
        }

        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));

        return static_cast<bool>(file_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n)));
    }

    bool Read(uint64_t offset, uint64_t n, std::vector<uint8_t>& out)
    {
        if (n > kMaxFileSize)
        {
            return false;
        }

        out.resize(static_cast<std::size_t>(n));

        return Read(offset, out.data(), n);
    }

private:
    std::ifstream file_;
    uint64_t size_ = 0;
};

std::string Utf16ToUtf8(const uint8_t* p, uint32_t bytes)
{
    std::string out;
    for (uint32_t i = 0; i + 1 < bytes; i += 2)
    {
        uint32_t c = LoadLe16(p + i);
        if (c >= 0xd800 && c < 0xdc00 && i + 3 < bytes)
        {
            const uint32_t lo = LoadLe16(p + i + 2);
            if (lo >= 0xdc00 && lo < 0xe000)
            {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i += 2;
            }
        }

        if (c < 0x80)
        {
            out += static_cast<char>(c);
        }
        else if (c < 0x800)
        {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char>(0xe0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
        else
        {
            out += static_cast<char>(0xf0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
    }

    return out;
}

struct RomFsEntry
{
    uint64_t offset; // in the image
    uint64_t size;
};

// Lists the files of a level-3 RomFS image that starts at `base` in the image.
std::optional<std::string> ListRomFs(Image& img, uint64_t base, uint64_t limit,
                                     std::map<std::string, RomFsEntry>& files)
{
    uint8_t h[0x28];
    if (!img.Read(base, h, sizeof(h)))
    {
        return "truncated RomFS";
    }

    const uint32_t dir_meta_off = LoadLe32(h + 0x0c), dir_meta_size = LoadLe32(h + 0x10);
    const uint32_t file_meta_off = LoadLe32(h + 0x1c), file_meta_size = LoadLe32(h + 0x20);
    const uint32_t data_off = LoadLe32(h + 0x24);
    if (LoadLe32(h) != 0x28 || dir_meta_size > (16u << 20) || file_meta_size > (64u << 20))
    {
        return "not a RomFS (bad level-3 header)";
    }

    std::vector<uint8_t> dirs, filemeta;
    if (!img.Read(base + dir_meta_off, dir_meta_size, dirs) ||
        !img.Read(base + file_meta_off, file_meta_size, filemeta))
    {
        return "truncated RomFS metadata";
    }

    constexpr uint32_t kNone = 0xffffffff;
    std::string error;
    int visited = 0;
    std::function<void(uint32_t, const std::string&, int)> walk = [&](uint32_t dir, const std::string& path, int depth)
    {
        if (depth > 64 || uint64_t{dir} + 0x18 > dirs.size() || ++visited > 1000000)
        {
            error = "corrupt RomFS directory tree";
            return;
        }

        for (uint32_t f = LoadLe32(&dirs[dir + 0x0c]); f != kNone && error.empty();)
        {
            if (uint64_t{f} + 0x20 > filemeta.size())
            {
                error = "corrupt RomFS file entry";
                return;
            }

            const uint8_t* e = &filemeta[f];
            const uint32_t name_len = LoadLe32(e + 0x1c);
            if (uint64_t{f} + 0x20 + name_len > filemeta.size() || ++visited > 1000000)
            {
                error = "corrupt RomFS file entry";
                return;
            }

            const uint64_t off = base + data_off + LoadLe64(e + 0x08), size = LoadLe64(e + 0x10);
            if (off > limit || size > limit - off)
            {
                error = "RomFS file outside the RomFS";
                return;
            }

            files[path + Utf16ToUtf8(e + 0x20, name_len)] = {off, size};
            f = LoadLe32(e + 0x04);
        }

        for (uint32_t c = LoadLe32(&dirs[dir + 0x08]); c != kNone && error.empty();)
        {
            if (uint64_t{c} + 0x18 > dirs.size())
            {
                error = "corrupt RomFS directory entry";
                return;
            }

            const uint32_t name_len = LoadLe32(&dirs[c + 0x14]);
            if (uint64_t{c} + 0x18 + name_len > dirs.size())
            {
                error = "corrupt RomFS directory entry";
                return;
            }

            walk(c, path + Utf16ToUtf8(&dirs[c + 0x18], name_len) + "/", depth + 1);
            c = LoadLe32(&dirs[c + 0x04]);
        }
    };

    walk(0, "", 0);
    if (!error.empty())
    {
        return error;
    }

    return std::nullopt;
}

// An NCCH (a CXI) at `base`.
std::optional<std::string> LoadNcch(const std::shared_ptr<Image>& img, uint64_t base, uint64_t limit, GameFiles& out)
{
    uint8_t h[0x200];
    if (!img->Read(base, h, sizeof(h)) || std::memcmp(h + 0x100, "NCCH", 4) != 0)
    {
        char message[40];
        std::snprintf(message, sizeof(message), "no NCCH at 0x%llx", static_cast<unsigned long long>(base));
        return message;
    }

    const uint8_t* flags = h + 0x188;
    if (!(flags[7] & 0x04))
    {
        return "the image is encrypted: decrypt it first (only decrypted images are supported)";
    }

    if (!(flags[5] & 0x02))
    {
        return "the NCCH isn't an application (CXI)";
    }

    if (flags[6] > kMaxMediaUnitShift)
    {
        return "the NCCH's media unit size is invalid";
    }

    const uint64_t mu = 0x200ull << flags[6];
    const uint32_t exheader_size = LoadLe32(h + 0x180);
    if (exheader_size < 0x400)
    {
        return "the NCCH has no extended header";
    }

    if (!img->Read(base + 0x200, 0x800, out.exheader))
    {
        return "truncated extended header";
    }

    // Read .code from the ExeFS, and decompress it if the exheader says it's compressed.
    const uint64_t exefs = base + LoadLe32(h + 0x1a0) * mu;
    uint8_t eh[0x200];
    if (!LoadLe32(h + 0x1a4) || !img->Read(exefs, eh, sizeof(eh)))
    {
        return "the NCCH has no ExeFS";
    }

    bool found = false;
    for (int i = 0; i < 10; i++)
    {
        const uint8_t* e = eh + i * 16;
        if (std::memcmp(e, ".code\0\0\0", 8) != 0)
        {
            continue;
        }

        if (!img->Read(exefs + 0x200 + LoadLe32(e + 8), LoadLe32(e + 12), out.code))
        {
            return "truncated .code";
        }

        found = true;
    }
    if (!found)
    {
        return "the ExeFS has no .code (is the image really decrypted?)";
    }

    if (out.exheader[0x0d] & 1)
    {
        auto code = BlzDecompress(out.code);
        if (!code)
        {
            return "can't decompress .code";
        }

        out.code = std::move(*code);
    }

    // The RomFS is an IVFC image, with the level-3 file system after its header and master hash.
    const uint64_t romfs = base + LoadLe32(h + 0x1b0) * mu;
    const uint64_t romfs_size = LoadLe32(h + 0x1b4) * mu;
    uint8_t ivfc[0x60];
    if (!romfs_size || !img->Read(romfs, ivfc, sizeof(ivfc)))
    {
        return "the NCCH has no RomFS";
    }

    if (std::memcmp(ivfc, "IVFC", 4) != 0 || LoadLe32(ivfc + 4) != 0x10000)
    {
        return "the RomFS has no IVFC header (is the image really decrypted?)";
    }

    const uint32_t block_log2 = LoadLe32(ivfc + 0x4c);
    if (block_log2 > 24)
    {
        return "bad IVFC header";
    }

    const uint64_t level3 = romfs + AlignUp(0x60 + static_cast<uint64_t>(LoadLe32(ivfc + 0x08)), 1ull << block_log2);
    auto entries = std::make_shared<std::map<std::string, RomFsEntry>>();
    if (auto err = ListRomFs(*img, level3, std::min(limit, romfs + romfs_size), *entries))
    {
        return err;
    }

    out.romfs_files.clear();
    for (const auto& [path, e] : *entries)
    {
        out.romfs_files.push_back(path);
    }

    out.read_romfs = [img, entries](const std::string& path, std::vector<uint8_t>& data)
    {
        auto it = entries->find(path);
        return it != entries->end() && img->Read(it->second.offset, it->second.size, data);
    };

    return std::nullopt;
}

std::optional<std::string> LoadCia(const std::shared_ptr<Image>& img, GameFiles& out)
{
    uint8_t h[0x20];
    if (!img->Read(0, h, sizeof(h)))
    {
        return "truncated CIA";
    }

    const uint64_t cert = AlignUp(LoadLe32(h), 64);
    const uint64_t ticket = AlignUp(cert + LoadLe32(h + 0x08), 64);
    const uint64_t tmd = AlignUp(ticket + LoadLe32(h + 0x0c), 64);
    const uint64_t content = AlignUp(tmd + LoadLe32(h + 0x10), 64);

    uint8_t sig[4];
    if (!img->Read(tmd, sig, 4))
    {
        return "truncated CIA (TMD)";
    }

    uint64_t sig_size;
    switch (Be32(sig))
    {
    case 0x10000:
    case 0x10003:
        sig_size = 0x200 + 0x3c;
        break;
    case 0x10001:
    case 0x10004:
        sig_size = 0x100 + 0x3c;
        break;
    case 0x10002:
    case 0x10005:
        sig_size = 0x3c + 0x40;
        break;
    default:
        return "bad TMD signature type";
    }

    const uint64_t tmd_header = tmd + 4 + sig_size;
    uint8_t th[0xc4];
    if (!img->Read(tmd_header, th, sizeof(th)))
    {
        return "truncated TMD";
    }

    if (Be16(th + 0x9e) == 0)
    {
        return "the CIA has no contents";
    }

    // The first content chunk record: the main NCCH, stored first.
    uint8_t chunk[0x30];
    if (!img->Read(tmd_header + 0xc4 + 0x900, chunk, sizeof(chunk)))
    {
        return "truncated TMD";
    }

    if (Be16(chunk + 6) & 1)
    {
        return "the CIA's contents are encrypted: decrypt it first (only decrypted images are supported)";
    }

    const uint64_t size = Be64(chunk + 8);
    if (content > img->Size() || size > img->Size() - content)
    {
        return "truncated CIA contents";
    }

    return LoadNcch(img, content, content + size, out);
}

} // namespace

uint64_t GameFiles::ProgramId() const
{
    if (exheader.size() < 0x208)
    {
        return 0;
    }

    return LoadLe64(exheader.data() + 0x200);
}

std::vector<std::string> GameFiles::SoundArchives() const
{
    std::vector<std::string> out;
    for (const auto& p : romfs_files)
    {
        const std::string lower = AsciiLower(p);
        if (lower.size() > 6 && lower.compare(lower.size() - 6, 6, ".bcsar") == 0)
        {
            out.push_back(p);
        }
    }

    return out;
}

std::optional<std::vector<uint8_t>> BlzDecompress(const std::vector<uint8_t>& data)
{
    if (data.size() < 8)
    {
        return std::nullopt;
    }

    const uint32_t top_bottom = LoadLe32(&data[data.size() - 8]);
    const uint32_t extra = LoadLe32(&data[data.size() - 4]);
    const std::size_t top = top_bottom & 0xffffff, bottom = top_bottom >> 24;
    if (bottom < 8 || bottom > 11 || top < bottom || top > data.size())
    {
        return std::nullopt;
    }

    const uint64_t out_size = data.size() + static_cast<uint64_t>(extra);
    if (out_size > kMaxFileSize)
    {
        return std::nullopt;
    }

    // The part before `stop` is stored uncompressed; the rest is decoded from the end down.
    std::vector<uint8_t> out(static_cast<std::size_t>(out_size));
    std::copy(data.begin(), data.end(), out.begin());
    const std::size_t stop = data.size() - top;
    std::size_t src = data.size() - bottom;
    std::size_t dst = out.size();
    while (src > stop)
    {
        const uint8_t flags = data[--src];
        for (int i = 0; i < 8 && src > stop; i++)
        {
            if (flags & (0x80 >> i))
            {
                if (src - stop < 2)
                {
                    return std::nullopt;
                }

                const uint8_t hi = data[--src];
                const uint8_t lo = data[--src];
                const std::size_t len = (hi >> 4) + 3;
                const std::size_t offset = (((hi & 0x0f) << 8) | lo) + 3;
                if (len > dst - stop || dst + offset > out.size())
                {
                    return std::nullopt;
                }

                for (std::size_t j = 0; j < len; j++)
                {
                    --dst;
                    out[dst] = out[dst + offset];
                }
            }
            else
            {
                if (dst <= stop)
                {
                    return std::nullopt;
                }

                out[--dst] = data[--src];
            }
        }
    }
    if (dst != stop)
    {
        return std::nullopt;
    }

    return out;
}

std::optional<std::string> LoadGameDirectory(const std::string& dir, GameFiles& out)
{
    out = GameFiles{};

    const fs::path exefs = fs::path(dir) / "exefs";
    std::error_code exefs_error;
    if (!fs::is_directory(exefs, exefs_error))
    {
        return dir + " isn't an extracted game: it has no exefs folder";
    }

    if (!ReadFile(exefs / "exheader.bin", out.exheader) || out.exheader.size() < 0x400)
    {
        return "can't read " + (exefs / "exheader.bin").string();
    }

    if (!ReadFile(exefs / "code.bin", out.code))
    {
        return "can't read " + (exefs / "code.bin").string();
    }

    // A compressed code.bin is shorter than the text, rodata and data pages together.
    const uint64_t pages = static_cast<uint64_t>(LoadLe32(&out.exheader[0x14])) + LoadLe32(&out.exheader[0x24]) +
                           LoadLe32(&out.exheader[0x34]);
    if (out.code.size() < pages * 0x1000)
    {
        auto code = BlzDecompress(out.code);
        if (!code || code->size() < pages * 0x1000)
        {
            return "code.bin is truncated or not valid BLZ";
        }

        out.code = std::move(*code);
    }

    // Folders that are symbolic links are followed, but not into a folder that holds the link. That would loop. An
    // entry that can't be read, such as a broken link, is passed over.
    const fs::path romfs = fs::path(dir) / "romfs";
    std::error_code ec;
    std::vector<fs::path> ancestors = {fs::canonical(romfs, ec)}; // the folders that hold the current entry
    for (fs::recursive_directory_iterator it(romfs, fs::directory_options::follow_directory_symlink, ec), end;
         !ec && it != end; it.increment(ec))
    {
        std::error_code entry_error;
        if (it->is_directory(entry_error))
        {
            ancestors.resize(static_cast<std::size_t>(it.depth()) + 1);
            const fs::path folder = fs::canonical(it->path(), entry_error);
            if (entry_error || std::find(ancestors.begin(), ancestors.end(), folder) != ancestors.end())
            {
                it.disable_recursion_pending();
            }

            ancestors.push_back(folder);
        }
        else if (it->is_regular_file(entry_error))
        {
            out.romfs_files.push_back(it->path().lexically_relative(romfs).generic_string());
        }
    }
    if (ec)
    {
        return "can't list " + romfs.string();
    }

    std::sort(out.romfs_files.begin(), out.romfs_files.end());

    out.read_romfs = [romfs](const std::string& path, std::vector<uint8_t>& data)
    {
        return ReadFile(romfs / fs::path(path), data);
    };

    return std::nullopt;
}

namespace
{

std::optional<std::string> LoadGameImage(const std::string& path, GameFiles& out)
{
    out = GameFiles{};

    auto img = std::make_shared<Image>(path);
    if (!img->Ok())
    {
        return "can't open " + path;
    }

    uint8_t h[0x200];
    if (!img->Read(0, h, sizeof(h)))
    {
        return path + " is too small for a 3DS image";
    }

    if (std::memcmp(h + 0x100, "NCSD", 4) == 0)
    {
        // .3ds/.cci: partition 0 is the application.
        if (h[0x188 + 6] > kMaxMediaUnitShift)
        {
            return "the NCSD image's media unit size is invalid";
        }

        const uint64_t mu = 0x200ull << h[0x188 + 6];
        const uint64_t offset = LoadLe32(h + 0x120) * mu, size = LoadLe32(h + 0x124) * mu;
        if (!size)
        {
            return "the NCSD image has no partition 0";
        }

        return LoadNcch(img, offset, offset + size, out);
    }

    if (std::memcmp(h + 0x100, "NCCH", 4) == 0)
    {
        return LoadNcch(img, 0, img->Size(), out); // .cxi
    }

    // A CIA starts with the size of its header (0x2020).
    if (LoadLe32(h) == 0x2020)
    {
        return LoadCia(img, out);
    }

    return path + " isn't a 3DS image (NCSD, NCCH or CIA)";
}

} // namespace

std::optional<std::string> LoadGame(const std::string& path, GameFiles& out)
{
    std::error_code ec;
    if (fs::is_directory(path, ec))
    {
        return LoadGameDirectory(path, out);
    }

    return LoadGameImage(path, out);
}

} // namespace threesf::rip
