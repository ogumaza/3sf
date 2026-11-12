// SPDX-License-Identifier: MIT

// Loads a game for ripping (see game.h). The container formats (NCSD, NCCH, ExeFS, IVFC/RomFS, CIA and TMD) are as
// 3dbrew documents them.

#include "rip/game.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/utf8.h"
#include <zlib.h>

#include "horizon/romfs.h"
#include "rip/blz.h"

namespace threesf::rip
{
namespace
{

namespace fs = std::filesystem;

constexpr uint64_t kMaxFileSize = 1ull << 30; // largest single file read into memory

// An image's media unit is 0x200 << n bytes. Real images use 0; anything past this would overflow the offsets.
constexpr uint8_t kMaxMediaUnitShift = 16;

// The offsets of the fields of an NCSD or NCCH header that 3SF reads (3dbrew, "NCSD" and "NCCH"). Both headers take
// 0x200 bytes. Both hold their magic word and their flags at the same offsets. The offsets and sizes of the partitions
// and areas count media units.
constexpr std::size_t kImageHeaderSize = 0x200;
constexpr std::size_t kImageMagic = 0x100;
constexpr std::size_t kImageFlags = 0x188;
constexpr std::size_t kNcsdPartition0Offset = 0x120;
constexpr std::size_t kNcsdPartition0Size = 0x124;
constexpr std::size_t kNcchExheaderSize = 0x180;
constexpr std::size_t kNcchExeFsOffset = 0x1a0;
constexpr std::size_t kNcchExeFsSize = 0x1a4;
constexpr std::size_t kNcchRomFsOffset = 0x1b0;
constexpr std::size_t kNcchRomFsSize = 0x1b4;

// The bytes of the flags that 3SF reads, and their bits: the NCCH's content type, the media unit's size in either
// header, and the NCCH's crypto method.
constexpr std::size_t kFlagContentType = 5;
constexpr std::size_t kFlagMediaUnit = 6;
constexpr std::size_t kFlagCrypto = 7;
constexpr uint8_t kExecutable = 0x02;
constexpr uint8_t kNoCrypto = 0x04;

// The extended header follows the NCCH's header. The loader reads it with the access descriptor after it.
constexpr std::size_t kExheaderSize = 0x400;
constexpr std::size_t kExheaderAndDescriptorSize = 0x800;

// The ExeFS's header (3dbrew, "ExeFS") takes 0x200 bytes. It starts with ten entries of 16 bytes. Each entry holds a
// file's name in 8 bytes, the file's offset from the end of the header and the file's size.
constexpr std::size_t kExeFsHeaderSize = 0x200;
constexpr std::size_t kExeFsEntries = 10;
constexpr std::size_t kExeFsEntrySize = 16;
constexpr std::size_t kExeFsNameSize = 8;
constexpr std::size_t kExeFsFileOffset = 8;
constexpr std::size_t kExeFsFileSize = 12;

// The fields of a RomFS's IVFC header that 3SF reads (3dbrew, "RomFS"): the version that marks a RomFS, the size of the
// master hash, and the size of a level-3 block as a power of 2. Level 3 starts at the first whole block after the
// header and the master hash. A block of more than 2^kMaxIvfcBlockLog2 bytes marks a damaged header.
constexpr std::size_t kIvfcHeaderSize = 0x60;
constexpr std::size_t kIvfcVersion = 0x04;
constexpr std::size_t kIvfcMasterHashSize = 0x08;
constexpr std::size_t kIvfcLevel3BlockLog2 = 0x4c;
constexpr uint32_t kIvfcRomFsVersion = 0x10000;
constexpr uint32_t kMaxIvfcBlockLog2 = 24;

// The fields of a CIA's header that 3SF reads (3dbrew, "CIA"), in the header's first 0x20 bytes: the sizes of the
// header, the certificate chain, the ticket and the TMD. The header takes kCiaHeaderBytes. The certificate chain, the
// ticket, the TMD and the contents follow it in that order, each at a multiple of kCiaAlignment bytes.
constexpr std::size_t kCiaHeaderFields = 0x20;
constexpr std::size_t kCiaHeaderSize = 0x00;
constexpr std::size_t kCiaCertificatesSize = 0x08;
constexpr std::size_t kCiaTicketSize = 0x0c;
constexpr std::size_t kCiaTmdSize = 0x10;
constexpr uint32_t kCiaHeaderBytes = 0x2020;
constexpr uint64_t kCiaAlignment = 64;

// A TMD (3dbrew, "Title metadata") starts with the signature's type in 4 bytes, then the signature and its padding. The
// sizes of a signature and of its padding depend on the signature's type: RSA-4096, RSA-2048 or ECDSA.
constexpr std::size_t kSignatureTypeSize = 4;
constexpr uint64_t kRsa4096SignatureSize = 0x200;
constexpr uint64_t kRsa2048SignatureSize = 0x100;
constexpr uint64_t kEcdsaSignatureSize = 0x3c;
constexpr uint64_t kRsaSignaturePadding = 0x3c;
constexpr uint64_t kEcdsaSignaturePadding = 0x40;

// The TMD's header follows the signature. It gives the count of contents. The 64 content info records follow the
// header, then a chunk record for each content. A chunk record gives the content's type, with bit 0 set for an
// encrypted content, and the content's size.
constexpr std::size_t kTmdHeaderSize = 0xc4;
constexpr std::size_t kTmdContentCount = 0x9e;
constexpr std::size_t kTmdContentInfoSize = 0x900;
constexpr std::size_t kChunkRecordSize = 0x30;
constexpr std::size_t kChunkType = 6;
constexpr std::size_t kChunkSize = 8;
constexpr uint16_t kChunkEncrypted = 1;

uint64_t AlignUp(uint64_t x, uint64_t a)
{
    return (x + a - 1) / a * a;
}

// The media unit of an NCSD or NCCH header, in bytes, or nothing when its flags give a size past kMaxMediaUnitShift.
std::optional<uint64_t> MediaUnit(const uint8_t* header)
{
    const uint8_t shift = header[kImageFlags + kFlagMediaUnit];
    if (shift > kMaxMediaUnitShift)
    {
        return std::nullopt;
    }

    return uint64_t{0x200} << shift;
}

// Reads one of an extracted game's files, up to kMaxFileSize bytes.
bool ReadFile(const fs::path& path, std::vector<uint8_t>& data)
{
    return xsflib::ReadFile(path, data, static_cast<std::size_t>(kMaxFileSize));
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

// A name of `bytes` bytes of UTF-16LE at `p`, in UTF-8. A surrogate without its partner keeps its three bytes, so that
// two such names stay apart.
std::string Utf16ToUtf8(const uint8_t* p, uint32_t bytes)
{
    std::string out;
    xsflib::AppendUtf16(out, std::span<const uint8_t>(p, bytes), {.lone_surrogate_bytes = true});

    return out;
}

struct RomFsEntry
{
    uint64_t offset; // in the image
    uint64_t size;
};

// The limits of a level-3 RomFS that ListRomFs reads: the sizes of its tables of folders and files, the depth of its
// folders, and the entries that the walk visits. A damaged RomFS can't take the walk past them.
constexpr uint32_t kMaxDirMetaBytes = 16u << 20;
constexpr uint32_t kMaxFileMetaBytes = 64u << 20;
constexpr int kMaxDepth = 64;
constexpr int kMaxEntries = 1000000;

// The walk of a level-3 RomFS's tree of folders. It puts each file's path and place in `files`.
class RomFsWalker
{
public:
    // `dirs` and `filemeta` are the tables of folders and files. The files' data starts at `data_start` in the image.
    // No file may reach past `limit`.
    RomFsWalker(std::span<const uint8_t> dirs, std::span<const uint8_t> filemeta, uint64_t data_start, uint64_t limit,
                std::map<std::string, RomFsEntry>& files)
        : dirs_(dirs), filemeta_(filemeta), data_start_(data_start), limit_(limit), files_(files)
    {
    }

    // Lists the files of folder `dir` and of the folders below it. `path` is the folder's path, with a '/' at its end,
    // or empty for the root. `depth` counts the folders above it. The walk stops at the first damaged entry.
    void Walk(uint32_t dir, const std::string& path, int depth)
    {
        using Layout = horizon::RomFsLayout;
        if (depth > kMaxDepth || uint64_t{dir} + Layout::kDirName > dirs_.size() || ++visited_ > kMaxEntries)
        {
            error_ = "corrupt RomFS directory tree";
            return;
        }

        for (uint32_t file = xsflib::LoadLe32(&dirs_[dir + Layout::kDirFirstFile]);
             file != Layout::kNoEntry && error_.empty();)
        {
            if (uint64_t{file} + Layout::kFileName > filemeta_.size())
            {
                error_ = "corrupt RomFS file entry";
                return;
            }

            const uint8_t* entry = &filemeta_[file];
            const uint32_t name_len = xsflib::LoadLe32(entry + Layout::kFileNameLength);
            if (uint64_t{file} + Layout::kFileName + name_len > filemeta_.size() || ++visited_ > kMaxEntries)
            {
                error_ = "corrupt RomFS file entry";
                return;
            }

            const uint64_t off = data_start_ + xsflib::LoadLe64(entry + Layout::kFileData);
            const uint64_t size = xsflib::LoadLe64(entry + Layout::kFileSize);
            if (off > limit_ || size > limit_ - off)
            {
                error_ = "RomFS file outside the RomFS";
                return;
            }

            files_[path + Utf16ToUtf8(entry + Layout::kFileName, name_len)] = {off, size};
            file = xsflib::LoadLe32(entry + Layout::kFileSibling);
        }

        for (uint32_t child = xsflib::LoadLe32(&dirs_[dir + Layout::kDirFirstDir]);
             child != Layout::kNoEntry && error_.empty();)
        {
            if (uint64_t{child} + Layout::kDirName > dirs_.size())
            {
                error_ = "corrupt RomFS directory entry";
                return;
            }

            const uint32_t name_len = xsflib::LoadLe32(&dirs_[child + Layout::kDirNameLength]);
            if (uint64_t{child} + Layout::kDirName + name_len > dirs_.size())
            {
                error_ = "corrupt RomFS directory entry";
                return;
            }

            Walk(child, path + Utf16ToUtf8(&dirs_[child + Layout::kDirName], name_len) + "/", depth + 1);
            child = xsflib::LoadLe32(&dirs_[child + Layout::kDirSibling]);
        }
    }

    // Why the walk stopped early, or empty when it listed every file.
    const std::string& Error() const
    {
        return error_;
    }

private:
    std::span<const uint8_t> dirs_;
    std::span<const uint8_t> filemeta_;
    uint64_t data_start_;
    uint64_t limit_;
    std::map<std::string, RomFsEntry>& files_;
    int visited_ = 0;
    std::string error_;
};

// Lists the files of a level-3 RomFS image that starts at `base` in the image.
std::optional<std::string> ListRomFs(Image& image, uint64_t base, uint64_t limit,
                                     std::map<std::string, RomFsEntry>& files)
{
    using Layout = horizon::RomFsLayout;
    uint8_t header[Layout::kHeaderSize];
    if (!image.Read(base, header, sizeof(header)))
    {
        return "truncated RomFS";
    }

    const uint32_t dir_meta_off = xsflib::LoadLe32(header + Layout::kDirMetaOffset);
    const uint32_t dir_meta_size = xsflib::LoadLe32(header + Layout::kDirMetaLength);
    const uint32_t file_meta_off = xsflib::LoadLe32(header + Layout::kFileMetaOffset);
    const uint32_t file_meta_size = xsflib::LoadLe32(header + Layout::kFileMetaLength);
    const uint32_t data_off = xsflib::LoadLe32(header + Layout::kFileDataOffset);
    if (xsflib::LoadLe32(header) != Layout::kHeaderSize || dir_meta_size > kMaxDirMetaBytes ||
        file_meta_size > kMaxFileMetaBytes)
    {
        return "not a RomFS (bad level-3 header)";
    }

    std::vector<uint8_t> dirs, filemeta;
    if (!image.Read(base + dir_meta_off, dir_meta_size, dirs) ||
        !image.Read(base + file_meta_off, file_meta_size, filemeta))
    {
        return "truncated RomFS metadata";
    }

    RomFsWalker walker(dirs, filemeta, base + data_off, limit, files);
    walker.Walk(0, "", 0);
    if (!walker.Error().empty())
    {
        return walker.Error();
    }

    return std::nullopt;
}

// Reads .code from the ExeFS at `exefs` in the image. The ExeFS takes `size` media units.
std::optional<std::string> ReadExeFsCode(Image& image, uint64_t exefs, uint32_t size, std::vector<uint8_t>& code)
{
    uint8_t exefs_header[kExeFsHeaderSize];
    if (!size || !image.Read(exefs, exefs_header, sizeof(exefs_header)))
    {
        return "the NCCH has no ExeFS";
    }

    bool found = false;
    for (std::size_t i = 0; i < kExeFsEntries; i++)
    {
        const uint8_t* entry = exefs_header + i * kExeFsEntrySize;
        if (std::memcmp(entry, ".code\0\0\0", kExeFsNameSize) != 0)
        {
            continue;
        }

        if (!image.Read(exefs + kExeFsHeaderSize + xsflib::LoadLe32(entry + kExeFsFileOffset),
                        xsflib::LoadLe32(entry + kExeFsFileSize), code))
        {
            return "truncated .code";
        }

        found = true;
    }
    if (!found)
    {
        return "the ExeFS has no .code (is the image really decrypted?)";
    }

    return std::nullopt;
}

// Puts the paths of the RomFS's files in out.romfs_files and a reader of the files in out.read_romfs. The RomFS starts
// at `romfs` in the image and takes `size` bytes. No file may reach past `limit`. The RomFS is an IVFC image. Its
// level-3 file system follows the IVFC header and the master hash.
std::optional<std::string> OpenRomFs(const std::shared_ptr<Image>& image, uint64_t romfs, uint64_t size, uint64_t limit,
                                     GameFiles& out)
{
    uint8_t ivfc[kIvfcHeaderSize];
    if (!size || !image->Read(romfs, ivfc, sizeof(ivfc)))
    {
        return "the NCCH has no RomFS";
    }

    if (std::memcmp(ivfc, "IVFC", 4) != 0 || xsflib::LoadLe32(ivfc + kIvfcVersion) != kIvfcRomFsVersion)
    {
        return "the RomFS has no IVFC header (is the image really decrypted?)";
    }

    const uint32_t block_log2 = xsflib::LoadLe32(ivfc + kIvfcLevel3BlockLog2);
    if (block_log2 > kMaxIvfcBlockLog2)
    {
        return "bad IVFC header";
    }

    const uint64_t level3 =
        romfs + AlignUp(kIvfcHeaderSize + static_cast<uint64_t>(xsflib::LoadLe32(ivfc + kIvfcMasterHashSize)),
                        1ull << block_log2);
    auto entries = std::make_shared<std::map<std::string, RomFsEntry>>();
    if (auto err = ListRomFs(*image, level3, std::min(limit, romfs + size), *entries))
    {
        return err;
    }

    out.romfs_files.clear();
    for (const auto& [path, entry] : *entries)
    {
        out.romfs_files.push_back(path);
    }

    out.read_romfs = [image, entries](const std::string& path, std::vector<uint8_t>& data)
    {
        auto it = entries->find(path);
        return it != entries->end() && image->Read(it->second.offset, it->second.size, data);
    };

    return std::nullopt;
}

// An NCCH (a CXI) at `base`.
std::optional<std::string> LoadNcch(const std::shared_ptr<Image>& image, uint64_t base, uint64_t limit, GameFiles& out)
{
    uint8_t header[kImageHeaderSize];
    if (!image->Read(base, header, sizeof(header)) || std::memcmp(header + kImageMagic, "NCCH", 4) != 0)
    {
        char message[40];
        std::snprintf(message, sizeof(message), "no NCCH at 0x%llx", static_cast<unsigned long long>(base));
        return message;
    }

    const uint8_t* flags = header + kImageFlags;
    if (!(flags[kFlagCrypto] & kNoCrypto))
    {
        return "the image is encrypted: decrypt it first (only decrypted images are supported)";
    }

    if (!(flags[kFlagContentType] & kExecutable))
    {
        return "the NCCH isn't an application (CXI)";
    }

    const std::optional<uint64_t> media_unit = MediaUnit(header);
    if (!media_unit)
    {
        return "the NCCH's media unit size is invalid";
    }

    const uint32_t exheader_size = xsflib::LoadLe32(header + kNcchExheaderSize);
    if (exheader_size < kExheaderSize)
    {
        return "the NCCH has no extended header";
    }

    if (!image->Read(base + kImageHeaderSize, kExheaderAndDescriptorSize, out.exheader))
    {
        return "truncated extended header";
    }

    // Read .code from the ExeFS, and decompress it if the exheader says it's compressed.
    const uint64_t exefs = base + xsflib::LoadLe32(header + kNcchExeFsOffset) * *media_unit;
    if (auto err = ReadExeFsCode(*image, exefs, xsflib::LoadLe32(header + kNcchExeFsSize), out.code))
    {
        return err;
    }

    if (out.exheader[kExheaderFlags] & 1)
    {
        auto code = BlzDecompress(out.code);
        if (!code)
        {
            return "can't decompress .code";
        }

        out.code = std::move(*code);
    }

    return OpenRomFs(image, base + xsflib::LoadLe32(header + kNcchRomFsOffset) * *media_unit,
                     xsflib::LoadLe32(header + kNcchRomFsSize) * *media_unit, limit, out);
}

std::optional<std::string> LoadCia(const std::shared_ptr<Image>& image, GameFiles& out)
{
    uint8_t header[kCiaHeaderFields];
    if (!image->Read(0, header, sizeof(header)))
    {
        return "truncated CIA";
    }

    const uint64_t cert = AlignUp(xsflib::LoadLe32(header + kCiaHeaderSize), kCiaAlignment);
    const uint64_t ticket = AlignUp(cert + xsflib::LoadLe32(header + kCiaCertificatesSize), kCiaAlignment);
    const uint64_t tmd = AlignUp(ticket + xsflib::LoadLe32(header + kCiaTicketSize), kCiaAlignment);
    const uint64_t content = AlignUp(tmd + xsflib::LoadLe32(header + kCiaTmdSize), kCiaAlignment);

    uint8_t signature_type[kSignatureTypeSize];
    if (!image->Read(tmd, signature_type, sizeof(signature_type)))
    {
        return "truncated CIA (TMD)";
    }

    // The types of RSA-4096, RSA-2048 and ECDSA signatures, each over a SHA-1 or a SHA-256 hash.
    uint64_t signature_size;
    switch (xsflib::LoadBe32(signature_type))
    {
    case 0x10000:
    case 0x10003:
        signature_size = kRsa4096SignatureSize + kRsaSignaturePadding;
        break;
    case 0x10001:
    case 0x10004:
        signature_size = kRsa2048SignatureSize + kRsaSignaturePadding;
        break;
    case 0x10002:
    case 0x10005:
        signature_size = kEcdsaSignatureSize + kEcdsaSignaturePadding;
        break;
    default:
        return "bad TMD signature type";
    }

    const uint64_t tmd_header_offset = tmd + kSignatureTypeSize + signature_size;
    uint8_t tmd_header[kTmdHeaderSize];
    if (!image->Read(tmd_header_offset, tmd_header, sizeof(tmd_header)))
    {
        return "truncated TMD";
    }

    if (xsflib::LoadBe16(tmd_header + kTmdContentCount) == 0)
    {
        return "the CIA has no contents";
    }

    // The first content chunk record: the main NCCH, stored first.
    uint8_t chunk[kChunkRecordSize];
    if (!image->Read(tmd_header_offset + kTmdHeaderSize + kTmdContentInfoSize, chunk, sizeof(chunk)))
    {
        return "truncated TMD";
    }

    if (xsflib::LoadBe16(chunk + kChunkType) & kChunkEncrypted)
    {
        return "the CIA's contents are encrypted: decrypt it first (only decrypted images are supported)";
    }

    const uint64_t size = xsflib::LoadBe64(chunk + kChunkSize);
    if (content > image->Size() || size > image->Size() - content)
    {
        return "truncated CIA contents";
    }

    return LoadNcch(image, content, content + size, out);
}

std::optional<std::string> LoadGameImage(const std::string& path, GameFiles& out)
{
    out = GameFiles{};

    auto image = std::make_shared<Image>(path);
    if (!image->Ok())
    {
        return "can't open " + path;
    }

    uint8_t header[kImageHeaderSize];
    if (!image->Read(0, header, sizeof(header)))
    {
        return path + " is too small for a 3DS image";
    }

    if (std::memcmp(header + kImageMagic, "NCSD", 4) == 0)
    {
        // .3ds/.cci: partition 0 is the application.
        const std::optional<uint64_t> media_unit = MediaUnit(header);
        if (!media_unit)
        {
            return "the NCSD image's media unit size is invalid";
        }

        const uint64_t offset = xsflib::LoadLe32(header + kNcsdPartition0Offset) * *media_unit;
        const uint64_t size = xsflib::LoadLe32(header + kNcsdPartition0Size) * *media_unit;
        if (!size)
        {
            return "the NCSD image has no partition 0";
        }

        return LoadNcch(image, offset, offset + size, out);
    }

    if (std::memcmp(header + kImageMagic, "NCCH", 4) == 0)
    {
        return LoadNcch(image, 0, image->Size(), out); // .cxi
    }

    // A CIA starts with the size of its header (0x2020).
    if (xsflib::LoadLe32(header) == kCiaHeaderBytes)
    {
        return LoadCia(image, out);
    }

    return path + " isn't a 3DS image (NCSD, NCCH or CIA)";
}

// Lists the files under `romfs`, an extracted RomFS folder, into `files`: their paths relative to `romfs`, with "/"
// between folders, in sorted order. Returns false if the folder can't be listed. Folders that are symbolic links are
// followed, but not into a folder that holds the link. That would loop. An entry that can't be read, such as a broken
// link, is passed over. AppleDouble files are left out. macOS writes one, named "._" and the file's name, beside each
// file that it copies to a FAT or exFAT drive.
bool ListRomFs(const fs::path& romfs, std::vector<std::string>& files)
{
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
        else if (it->is_regular_file(entry_error) && !it->path().filename().u8string().starts_with(u8"._"))
        {
            files.push_back(it->path().lexically_relative(romfs).generic_string());
        }
    }

    if (ec)
    {
        return false;
    }

    std::sort(files.begin(), files.end());

    return true;
}

} // namespace

uint64_t GameFiles::ProgramId() const
{
    if (exheader.size() < kExheaderProgramId + 8)
    {
        return 0;
    }

    return xsflib::LoadLe64(exheader.data() + kExheaderProgramId);
}

uint32_t GameFiles::CodeCrc32() const
{
    return static_cast<uint32_t>(crc32(0, code.data(), static_cast<uInt>(code.size())));
}

std::vector<std::string> GameFiles::SoundArchives() const
{
    std::vector<std::string> out;
    for (const auto& p : romfs_files)
    {
        const std::string lower = xsflib::AsciiLower(p);
        if (lower.size() > 6 && lower.ends_with(".bcsar"))
        {
            out.push_back(p);
        }
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

    const fs::path exheader = exefs / "exheader.bin";
    if (!ReadFile(exheader, out.exheader))
    {
        return "can't read " + exheader.string();
    }

    if (out.exheader.size() < kExheaderSize)
    {
        return exheader.string() + " is too short for an extended header";
    }

    if (!ReadFile(exefs / "code.bin", out.code))
    {
        return "can't read " + (exefs / "code.bin").string();
    }

    // A compressed code.bin is shorter than the text, rodata and data pages together.
    const uint64_t pages = static_cast<uint64_t>(xsflib::LoadLe32(&out.exheader[kExheaderTextPages])) +
                           xsflib::LoadLe32(&out.exheader[kExheaderRodataPages]) +
                           xsflib::LoadLe32(&out.exheader[kExheaderDataPages]);
    if (out.code.size() < pages * 0x1000)
    {
        auto code = BlzDecompress(out.code);
        if (!code || code->size() < pages * 0x1000)
        {
            return "code.bin is truncated or not valid BLZ";
        }

        out.code = std::move(*code);
    }

    const fs::path romfs = fs::path(dir) / "romfs";
    if (!ListRomFs(romfs, out.romfs_files))
    {
        return "can't list " + romfs.string();
    }

    out.read_romfs = [romfs](const std::string& path, std::vector<uint8_t>& data)
    {
        return ReadFile(romfs / fs::path(path), data);
    };

    return std::nullopt;
}

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
