// SPDX-License-Identifier: MIT

// RomFS level-3 image builder (see romfs.h).

#include "horizon/romfs.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/bytes.h"

namespace threesf::horizon
{
namespace
{

constexpr uint32_t kEmpty = 0xffffffff;

// Decodes the UTF-8 sequence that starts at s[i] and moves i past it. A malformed sequence decodes to U+FFFD, and i
// moves past its first byte only.
uint32_t DecodeUtf8(const std::string& s, std::size_t& i)
{
    constexpr uint32_t kReplacement = 0xfffd;

    const uint8_t lead = static_cast<uint8_t>(s[i]);
    if (lead < 0x80)
    {
        i++;
        return lead;
    }

    std::size_t length;
    uint32_t c;
    uint32_t min;
    if (lead >= 0xc0 && lead < 0xe0)
    {
        length = 2;
        c = lead & 0x1f;
        min = 0x80;
    }
    else if (lead >= 0xe0 && lead < 0xf0)
    {
        length = 3;
        c = lead & 0x0f;
        min = 0x800;
    }
    else if (lead >= 0xf0 && lead < 0xf8)
    {
        length = 4;
        c = lead & 0x07;
        min = 0x10000;
    }
    else
    {
        i++;
        return kReplacement;
    }

    for (std::size_t k = 1; k < length; k++)
    {
        const uint8_t next = i + k < s.size() ? static_cast<uint8_t>(s[i + k]) : 0;
        if ((next & 0xc0) != 0x80)
        {
            i++;
            return kReplacement;
        }

        c = (c << 6) | (next & 0x3f);
    }

    // Overlong forms, surrogates and values past U+10FFFF aren't valid UTF-8.
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c < 0xe000))
    {
        i++;
        return kReplacement;
    }

    i += length;

    return c;
}

// Converts a FILE chunk's UTF-8 path component to RomFS UTF-16.
std::u16string Utf16(const std::string& s)
{
    std::u16string out;
    for (std::size_t i = 0; i < s.size();)
    {
        const uint32_t c = DecodeUtf8(s, i);
        if (c < 0x10000)
        {
            out.push_back(static_cast<char16_t>(c));
        }
        else
        {
            out.push_back(static_cast<char16_t>(0xd800 + ((c - 0x10000) >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + ((c - 0x10000) & 0x3ff)));
        }
    }

    return out;
}

uint32_t HashTableCount(uint32_t entries)
{
    if (entries < 3)
    {
        return 3;
    }
    if (entries < 19)
    {
        return entries | 1;
    }

    uint32_t count = entries;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 || count % 11 == 0 || count % 13 == 0 ||
           count % 17 == 0)
    {
        count++;
    }

    return count;
}

uint32_t PathHash(uint32_t parent, const std::u16string& name)
{
    uint32_t hash = parent ^ 123456789;
    for (char16_t c : name)
    {
        hash = (hash >> 5) | (hash << 27);
        hash ^= c;
    }

    return hash;
}

uint32_t Align(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

struct Dir
{
    std::u16string name;
    int parent = -1;
    std::vector<int> children; // directory indices
    std::vector<int> files;    // file indices
    uint32_t offset = 0;       // offset in the directory metadata table
};

struct FileEntry
{
    std::u16string name;
    int dir = 0;
    uint64_t data_offset = 0; // relative to the file data section
    uint64_t data_size = 0;
    uint32_t offset = 0; // offset in the file metadata table
    std::shared_ptr<const std::vector<uint8_t>> data;
};

void Put32(std::vector<uint8_t>& v, std::size_t off, uint32_t x)
{
    StoreLe32(v.data() + off, x);
}

void Put64(std::vector<uint8_t>& v, std::size_t off, uint64_t x)
{
    StoreLe64(v.data() + off, x);
}

void PutUtf16(std::vector<uint8_t>& v, std::size_t off, const std::u16string& s)
{
    for (std::size_t i = 0; i < s.size(); i++)
    {
        StoreLe16(v.data() + off + 2 * i, s[i]);
    }
}

} // namespace

RomFs::RomFs(std::vector<File> files)
{
    std::vector<Dir> dirs(1); // root
    std::vector<FileEntry> entries;
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.path < b.path; });
    for (auto& f : files)
    {
        // Find or create each directory on the file's path.
        int dir = 0;
        std::size_t start = 0;
        while (true)
        {
            const std::size_t slash = f.path.find('/', start);
            if (slash == std::string::npos)
            {
                break;
            }

            const std::u16string part = Utf16(f.path.substr(start, slash - start));
            int found = -1;
            for (int c : dirs[dir].children)
            {
                if (dirs[c].name == part)
                {
                    found = c;
                }
            }
            if (found < 0)
            {
                Dir d;
                d.name = part;
                d.parent = dir;
                dirs.push_back(d);
                found = static_cast<int>(dirs.size()) - 1;
                dirs[dir].children.push_back(found);
            }

            dir = found;
            start = slash + 1;
        }

        FileEntry e;
        e.name = Utf16(f.path.substr(start));
        e.dir = dir;
        e.data = f.data;
        e.data_size = f.data->size();
        entries.push_back(e);
        dirs[dir].files.push_back(static_cast<int>(entries.size()) - 1);
    }

    // Lay out metadata tables (directories in creation order, files in insertion order).
    uint32_t dir_table = 0;
    for (auto& d : dirs)
    {
        d.offset = dir_table;
        dir_table += 0x18 + Align(static_cast<uint32_t>(d.name.size() * 2), 4);
    }

    uint32_t file_table = 0;
    uint64_t data = 0;
    for (auto& e : entries)
    {
        e.offset = file_table;
        file_table += 0x20 + Align(static_cast<uint32_t>(e.name.size() * 2), 4);
        data = (data + 0xf) & ~uint64_t{0xf};
        e.data_offset = data;
        data += e.data_size;
    }

    const uint32_t dir_buckets = HashTableCount(static_cast<uint32_t>(dirs.size()));
    const uint32_t file_buckets = HashTableCount(static_cast<uint32_t>(entries.size()));
    constexpr uint32_t kHeaderSize = 0x28;
    constexpr uint32_t kDirHashOffset = kHeaderSize;
    const uint32_t dir_hash_len = dir_buckets * 4;
    const uint32_t dir_meta_off = kDirHashOffset + dir_hash_len;
    const uint32_t file_hash_off = dir_meta_off + dir_table;
    const uint32_t file_hash_len = file_buckets * 4;
    const uint32_t file_meta_off = file_hash_off + file_hash_len;
    data_offset_ = Align(file_meta_off + file_table, 0x10);

    meta_.assign(static_cast<std::size_t>(data_offset_), 0);
    Put32(meta_, 0x00, kHeaderSize);
    Put32(meta_, 0x04, kDirHashOffset);
    Put32(meta_, 0x08, dir_hash_len);
    Put32(meta_, 0x0c, dir_meta_off);
    Put32(meta_, 0x10, dir_table);
    Put32(meta_, 0x14, file_hash_off);
    Put32(meta_, 0x18, file_hash_len);
    Put32(meta_, 0x1c, file_meta_off);
    Put32(meta_, 0x20, file_table);
    Put32(meta_, 0x24, static_cast<uint32_t>(data_offset_));

    std::vector<uint32_t> dir_hash(dir_buckets, kEmpty);
    std::vector<uint32_t> file_hash(file_buckets, kEmpty);

    // Directory entries
    for (std::size_t i = 0; i < dirs.size(); i++)
    {
        const Dir& d = dirs[i];
        const std::size_t off = dir_meta_off + d.offset;
        const uint32_t parent_off = d.parent < 0 ? 0 : dirs[d.parent].offset;
        uint32_t sibling = kEmpty;
        if (d.parent >= 0)
        {
            const auto& sibs = dirs[d.parent].children;
            auto it = std::find(sibs.begin(), sibs.end(), static_cast<int>(i));
            if (it + 1 != sibs.end())
            {
                sibling = dirs[*(it + 1)].offset;
            }
        }

        Put32(meta_, off + 0x00, parent_off);
        Put32(meta_, off + 0x04, sibling);
        Put32(meta_, off + 0x08, d.children.empty() ? kEmpty : dirs[d.children[0]].offset);
        Put32(meta_, off + 0x0c, d.files.empty() ? kEmpty : entries[d.files[0]].offset);
        const uint32_t bucket = PathHash(parent_off, d.name) % dir_buckets;
        Put32(meta_, off + 0x10, dir_hash[bucket]);
        dir_hash[bucket] = d.offset;
        Put32(meta_, off + 0x14, static_cast<uint32_t>(d.name.size() * 2));
        PutUtf16(meta_, off + 0x18, d.name);
    }

    // File entries
    for (std::size_t i = 0; i < entries.size(); i++)
    {
        const FileEntry& e = entries[i];
        const std::size_t off = file_meta_off + e.offset;
        const auto& sibs = dirs[e.dir].files;
        auto it = std::find(sibs.begin(), sibs.end(), static_cast<int>(i));
        const uint32_t sibling = (it + 1 != sibs.end()) ? entries[*(it + 1)].offset : kEmpty;
        const uint32_t dir_off = dirs[e.dir].offset;
        Put32(meta_, off + 0x00, dir_off);
        Put32(meta_, off + 0x04, sibling);
        Put64(meta_, off + 0x08, e.data_offset);
        Put64(meta_, off + 0x10, e.data_size);
        const uint32_t bucket = PathHash(dir_off, e.name) % file_buckets;
        Put32(meta_, off + 0x18, file_hash[bucket]);
        file_hash[bucket] = e.offset;
        Put32(meta_, off + 0x1c, static_cast<uint32_t>(e.name.size() * 2));
        PutUtf16(meta_, off + 0x20, e.name);
    }

    for (uint32_t b = 0; b < dir_buckets; b++)
    {
        Put32(meta_, kDirHashOffset + 4 * b, dir_hash[b]);
    }

    for (uint32_t b = 0; b < file_buckets; b++)
    {
        Put32(meta_, file_hash_off + 4 * b, file_hash[b]);
    }

    for (auto& e : entries)
    {
        placements_.push_back({data_offset_ + e.data_offset, e.data});
    }

    size_ = data_offset_ + data;
}

uint64_t RomFs::Read(uint64_t offset, uint64_t length, uint8_t* dst) const
{
    // A read of nothing may come without a buffer, and memset and memcpy mustn't be given a null pointer.
    if (offset >= size_ || length == 0)
    {
        return 0;
    }

    length = std::min(length, size_ - offset);
    std::memset(dst, 0, static_cast<std::size_t>(length));

    // Metadata
    if (offset < meta_.size())
    {
        const uint64_t n = std::min<uint64_t>(length, meta_.size() - offset);
        std::memcpy(dst, meta_.data() + offset, static_cast<std::size_t>(n));
    }

    // File data
    for (const auto& p : placements_)
    {
        const uint64_t begin = p.offset, end = p.offset + p.data->size();
        const uint64_t lo = std::max(begin, offset), hi = std::min(end, offset + length);
        if (lo < hi)
        {
            std::memcpy(dst + (lo - offset), p.data->data() + (lo - begin), static_cast<std::size_t>(hi - lo));
        }
    }

    return length;
}

} // namespace threesf::horizon
