// SPDX-License-Identifier: MIT

// The level-3 RomFS image (see romfs.h). The image is a header, a hash table and a table of entries for the
// directories, the same two for the files, and then the files' data. Each directory lists its subdirectories and its
// files, and each entry links to the next one in its hash table's bucket. A game's file system code follows those
// links. The entries can therefore come in any order. This builder puts them in the order of the sorted paths.

#include "horizon/romfs.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/utf8.h"

namespace threesf::horizon
{
namespace
{

using Layout = RomFsLayout;

// A name's hash starts from the entry offset of its directory XORed with this value.
constexpr uint32_t kHashSeed = 123456789;

// The character that stands for a UTF-8 sequence that doesn't decode.
constexpr char16_t kReplacement = 0xfffd;

// The fixed part of each kind of entry, before its name.
constexpr uint32_t kDirEntrySize = 0x18;
constexpr uint32_t kFileEntrySize = 0x20;

// File data starts at a multiple of 16 bytes, from the start of the image and from the start of the data.
constexpr uint64_t kDataAlignment = 16;

// A directory of the tree.
struct DirNode
{
    std::u16string name;
    uint32_t parent = 0;                 // the index of the parent; the root's parent is the root
    std::vector<uint32_t> dirs;          // the subdirectories, in the order they were made
    std::vector<uint32_t> files;         // the files, in sorted order
    uint32_t offset = 0;                 // the entry's offset in the directory table
    uint32_t sibling = Layout::kNoEntry; // the offset of the next entry in the parent's list of subdirectories
};

// A file of the tree.
struct FileNode
{
    std::u16string name;
    uint32_t dir = 0;
    uint64_t data_offset = 0; // from the start of the file data
    uint64_t size = 0;
    uint32_t offset = 0;                 // the entry's offset in the file table
    uint32_t sibling = Layout::kNoEntry; // the offset of the next entry in the directory's list of files
};

// A part of a path in UTF-16. A UTF-8 sequence that doesn't decode gives U+FFFD and moves on by one byte.
std::u16string Utf16(std::string_view part)
{
    std::u16string name;
    while (!part.empty())
    {
        const auto [c, length] = xsflib::Utf8Character(part);
        if (length == 0)
        {
            name.push_back(kReplacement);
            part.remove_prefix(1);
            continue;
        }

        if (c > 0xffff)
        {
            name.push_back(static_cast<char16_t>(0xd800 + ((c - 0x10000) >> 10)));
            name.push_back(static_cast<char16_t>(0xdc00 + ((c - 0x10000) & 0x3ff)));
        }
        else
        {
            name.push_back(static_cast<char16_t>(c));
        }

        part.remove_prefix(length);
    }

    return name;
}

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// The bytes that a name takes in its entry: two for each UTF-16 unit, padded to a multiple of 4.
uint32_t NameField(const std::u16string& name)
{
    return static_cast<uint32_t>(AlignUp(2 * uint64_t{name.size()}, 4));
}

// The bucket count of a hash table for `entries` entries.
uint32_t BucketCount(uint32_t entries)
{
    if (entries < 3)
    {
        return 3;
    }

    if (entries < 19)
    {
        return entries | 1;
    }

    const auto has_small_factor = [](uint32_t n)
    {
        for (const uint32_t prime : {2u, 3u, 5u, 7u, 11u, 13u, 17u})
        {
            if (n % prime == 0)
            {
                return true;
            }
        }

        return false;
    };

    uint32_t count = entries;
    while (has_small_factor(count))
    {
        count++;
    }

    return count;
}

// The hash of a name in the directory whose entry is at `dir_offset`.
uint32_t NameHash(uint32_t dir_offset, const std::u16string& name)
{
    uint32_t hash = dir_offset ^ kHashSeed;
    for (const char16_t c : name)
    {
        hash = std::rotr(hash, 5) ^ static_cast<uint32_t>(c);
    }

    return hash;
}

// The index of the subdirectory of `parent` called `name`. A missing one is made, at the end of the list of all
// directories and of its parent's list.
uint32_t Subdirectory(std::vector<DirNode>& dirs, uint32_t parent, std::u16string name)
{
    for (const uint32_t dir : dirs[parent].dirs)
    {
        if (dirs[dir].name == name)
        {
            return dir;
        }
    }

    const auto index = static_cast<uint32_t>(dirs.size());
    DirNode dir;
    dir.name = std::move(name);
    dir.parent = parent;
    dirs.push_back(std::move(dir));
    dirs[parent].dirs.push_back(index);

    return index;
}

// The entry offset of the first item in `list`, or kNoEntry for an empty list.
template <typename Node>
uint32_t FirstOffset(const std::vector<uint32_t>& list, const std::vector<Node>& nodes)
{
    return list.empty() ? Layout::kNoEntry : nodes[list.front()].offset;
}

// Writes a name in UTF-16LE at `out`.
void StoreName(uint8_t* out, const std::u16string& name)
{
    for (std::size_t i = 0; i < name.size(); i++)
    {
        xsflib::StoreLe16(out + 2 * i, static_cast<uint16_t>(name[i]));
    }
}

// The directories and files of an image, in the order of their entries. The root comes first among the directories.
// The others come in the order that the sorted paths first name them. The files come in sorted order.
struct Tree
{
    std::vector<DirNode> dirs;
    std::vector<FileNode> files;
};

// The tree of files sorted by path. Each part of a path before the last names a directory inside the one before it.
// The last part names the file. Empty parts are names too.
Tree BuildTree(const std::vector<RomFs::File>& files)
{
    Tree tree;
    tree.dirs.resize(1);
    tree.files.reserve(files.size());
    for (const RomFs::File& file : files)
    {
        std::string_view path = file.path;
        uint32_t dir = 0;
        for (std::size_t slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/'))
        {
            dir = Subdirectory(tree.dirs, dir, Utf16(path.substr(0, slash)));
            path.remove_prefix(slash + 1);
        }

        FileNode node;
        node.name = Utf16(path);
        node.dir = dir;
        node.size = file.data ? file.data->size() : 0;
        tree.dirs[dir].files.push_back(static_cast<uint32_t>(tree.files.size()));
        tree.files.push_back(std::move(node));
    }

    return tree;
}

// The lengths of the directory table, of the file table and of the file data.
struct Lengths
{
    uint32_t dir_table = 0;
    uint32_t file_table = 0;
    uint64_t data = 0;
};

// Gives each entry its offset in its table and each file its place in the data. Each entry then links to the next
// entry in its directory's list. The last of each list and the root link to none.
Lengths PlaceEntries(Tree& tree)
{
    Lengths lengths;
    for (DirNode& dir : tree.dirs)
    {
        dir.offset = lengths.dir_table;
        lengths.dir_table += kDirEntrySize + NameField(dir.name);
    }

    for (FileNode& node : tree.files)
    {
        node.offset = lengths.file_table;
        lengths.file_table += kFileEntrySize + NameField(node.name);
        lengths.data = AlignUp(lengths.data, kDataAlignment);
        node.data_offset = lengths.data;
        lengths.data += node.size;
    }

    for (const DirNode& dir : tree.dirs)
    {
        for (std::size_t i = 0; i + 1 < dir.dirs.size(); i++)
        {
            tree.dirs[dir.dirs[i]].sibling = tree.dirs[dir.dirs[i + 1]].offset;
        }

        for (std::size_t i = 0; i + 1 < dir.files.size(); i++)
        {
            tree.files[dir.files[i]].sibling = tree.files[dir.files[i + 1]].offset;
        }
    }

    return lengths;
}

// The image up to the file data: the header, the directories' hash table and entries, the files' hash table and
// entries, and zeros up to the data. The data starts at a multiple of 16 bytes.
std::vector<uint8_t> BuildHead(const Tree& tree, const Lengths& lengths)
{
    const uint32_t dir_buckets = BucketCount(static_cast<uint32_t>(tree.dirs.size()));
    const uint32_t file_buckets = BucketCount(static_cast<uint32_t>(tree.files.size()));
    const uint32_t dir_hash_offset = Layout::kHeaderSize;
    const uint32_t dir_table_offset = dir_hash_offset + 4 * dir_buckets;
    const uint32_t file_hash_offset = dir_table_offset + lengths.dir_table;
    const uint32_t file_table_offset = file_hash_offset + 4 * file_buckets;
    const auto data_offset =
        static_cast<uint32_t>(AlignUp(uint64_t{file_table_offset} + lengths.file_table, kDataAlignment));

    std::vector<uint8_t> bytes(data_offset, 0);
    uint8_t* const head = bytes.data();

    // The header starts with its size. The offset and the length of each table follow. The data's offset comes last.
    xsflib::StoreLe32(head, Layout::kHeaderSize);
    xsflib::StoreLe32(head + Layout::kDirHashOffset, dir_hash_offset);
    xsflib::StoreLe32(head + Layout::kDirHashLength, 4 * dir_buckets);
    xsflib::StoreLe32(head + Layout::kDirMetaOffset, dir_table_offset);
    xsflib::StoreLe32(head + Layout::kDirMetaLength, lengths.dir_table);
    xsflib::StoreLe32(head + Layout::kFileHashOffset, file_hash_offset);
    xsflib::StoreLe32(head + Layout::kFileHashLength, 4 * file_buckets);
    xsflib::StoreLe32(head + Layout::kFileMetaOffset, file_table_offset);
    xsflib::StoreLe32(head + Layout::kFileMetaLength, lengths.file_table);
    xsflib::StoreLe32(head + Layout::kFileDataOffset, data_offset);

    // Each entry links to the entry that its bucket held before it. The bucket then holds the entry.
    std::vector<uint32_t> dir_hash(dir_buckets, Layout::kNoEntry);
    for (const DirNode& dir : tree.dirs)
    {
        uint8_t* entry = head + dir_table_offset + dir.offset;
        const uint32_t parent = tree.dirs[dir.parent].offset;
        uint32_t& bucket = dir_hash[NameHash(parent, dir.name) % dir_buckets];
        xsflib::StoreLe32(entry + Layout::kDirParent, parent);
        xsflib::StoreLe32(entry + Layout::kDirSibling, dir.sibling);
        xsflib::StoreLe32(entry + Layout::kDirFirstDir, FirstOffset(dir.dirs, tree.dirs));
        xsflib::StoreLe32(entry + Layout::kDirFirstFile, FirstOffset(dir.files, tree.files));
        xsflib::StoreLe32(entry + Layout::kDirHashNext, bucket);
        xsflib::StoreLe32(entry + Layout::kDirNameLength, static_cast<uint32_t>(2 * dir.name.size()));
        StoreName(entry + Layout::kDirName, dir.name);
        bucket = dir.offset;
    }

    std::vector<uint32_t> file_hash(file_buckets, Layout::kNoEntry);
    for (const FileNode& node : tree.files)
    {
        uint8_t* entry = head + file_table_offset + node.offset;
        const uint32_t dir = tree.dirs[node.dir].offset;
        uint32_t& bucket = file_hash[NameHash(dir, node.name) % file_buckets];
        xsflib::StoreLe32(entry + Layout::kFileDir, dir);
        xsflib::StoreLe32(entry + Layout::kFileSibling, node.sibling);
        xsflib::StoreLe64(entry + Layout::kFileData, node.data_offset);
        xsflib::StoreLe64(entry + Layout::kFileSize, node.size);
        xsflib::StoreLe32(entry + Layout::kFileHashNext, bucket);
        xsflib::StoreLe32(entry + Layout::kFileNameLength, static_cast<uint32_t>(2 * node.name.size()));
        StoreName(entry + Layout::kFileName, node.name);
        bucket = node.offset;
    }

    for (std::size_t i = 0; i < dir_buckets; i++)
    {
        xsflib::StoreLe32(head + dir_hash_offset + 4 * i, dir_hash[i]);
    }

    for (std::size_t i = 0; i < file_buckets; i++)
    {
        xsflib::StoreLe32(head + file_hash_offset + 4 * i, file_hash[i]);
    }

    return bytes;
}

} // namespace

RomFs::RomFs(std::vector<File> files)
{
    std::stable_sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.path < b.path; });
    Tree tree = BuildTree(files);
    const Lengths lengths = PlaceEntries(tree);
    head_ = BuildHead(tree, lengths);

    // The files' data stays in their buffers. Empty files take no place in it.
    const uint64_t data_offset = head_.size();
    for (std::size_t i = 0; i < files.size(); i++)
    {
        const FileNode& node = tree.files[i];
        if (node.size > 0)
        {
            data_.push_back({data_offset + node.data_offset, node.size, std::move(files[i].data)});
        }
    }

    size_ = data_offset + lengths.data;
}

uint64_t RomFs::Size() const
{
    return size_;
}

uint64_t RomFs::Read(uint64_t offset, uint64_t length, uint8_t* dst) const
{
    if (offset >= size_ || length == 0)
    {
        return 0;
    }

    const uint64_t count = std::min(length, size_ - offset);
    const uint64_t end = offset + count;
    uint64_t at = offset;

    // The header, the tables and the zeros after them.
    if (at < head_.size())
    {
        const auto n = static_cast<std::size_t>(std::min<uint64_t>(end, head_.size()) - at);
        std::memcpy(dst, head_.data() + at, n);
        dst += n;
        at += n;
    }

    // The files' data, with zeros in the gaps between files.
    const auto past = [](const Data& data, uint64_t position)
    {
        return data.offset + data.size <= position;
    };
    auto data = std::lower_bound(data_.begin(), data_.end(), at, past);
    while (at < end)
    {
        const uint64_t start = data == data_.end() ? end : std::min(end, data->offset);
        if (at < start)
        {
            const auto n = static_cast<std::size_t>(start - at);
            std::memset(dst, 0, n);
            dst += n;
            at = start;
            continue;
        }

        const auto n = static_cast<std::size_t>(std::min(end, data->offset + data->size) - at);
        std::memcpy(dst, data->bytes->data() + static_cast<std::size_t>(at - data->offset), n);
        dst += n;
        at += n;
        ++data;
    }

    return count;
}

} // namespace threesf::horizon
