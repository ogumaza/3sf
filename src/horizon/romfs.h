// SPDX-License-Identifier: MIT

// The level-3 RomFS image (3dbrew, "RomFS") that fs:USER serves to a game in game mode, built from a list of files.
// RomFsLayout gives the layout of an image's header and entries. The ripper also reads real images with it.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace threesf::horizon
{

// A level-3 RomFS image of a list of files. The image refers to the files' buffers and doesn't copy them.
class RomFs
{
public:
    // A file: its UTF-8 path, with '/' between its parts and no leading slash, and its contents.
    struct File
    {
        std::string path;
        std::shared_ptr<const std::vector<uint8_t>> data;
    };

    // Builds the image's tables. The files may come in any order. No two may have the same path.
    explicit RomFs(std::vector<File> files);

    // The image's size in bytes.
    uint64_t Size() const;

    // Copies bytes [offset, offset + length) of the image to `dst`, cut at the image's end, and returns their count.
    // Leaves `dst` alone and returns 0 when the offset is at or past the end, or the length is 0.
    uint64_t Read(uint64_t offset, uint64_t length, uint8_t* dst) const;

private:
    // A file's contents and the offset of its data in the image.
    struct Data
    {
        uint64_t offset;
        uint64_t size;
        std::shared_ptr<const std::vector<uint8_t>> bytes;
    };

    std::vector<uint8_t> head_; // the header, the tables and the zeros up to the file data
    std::vector<Data> data_;    // in the order of their offsets
    uint64_t size_ = 0;
};

// The offsets of a level-3 image's header fields, and of the fields of its directory and file entries.
struct RomFsLayout
{
    static constexpr uint32_t kHeaderSize = 0x28;
    static constexpr uint32_t kNoEntry = 0xffffffff;

    static constexpr uint32_t kDirHashOffset = 0x04;
    static constexpr uint32_t kDirHashLength = 0x08;
    static constexpr uint32_t kDirMetaOffset = 0x0c;
    static constexpr uint32_t kDirMetaLength = 0x10;
    static constexpr uint32_t kFileHashOffset = 0x14;
    static constexpr uint32_t kFileHashLength = 0x18;
    static constexpr uint32_t kFileMetaOffset = 0x1c;
    static constexpr uint32_t kFileMetaLength = 0x20;
    static constexpr uint32_t kFileDataOffset = 0x24;

    static constexpr uint32_t kDirParent = 0x00;
    static constexpr uint32_t kDirSibling = 0x04;
    static constexpr uint32_t kDirFirstDir = 0x08;
    static constexpr uint32_t kDirFirstFile = 0x0c;
    static constexpr uint32_t kDirHashNext = 0x10;
    static constexpr uint32_t kDirNameLength = 0x14;
    static constexpr uint32_t kDirName = 0x18;

    static constexpr uint32_t kFileDir = 0x00;
    static constexpr uint32_t kFileSibling = 0x04;
    static constexpr uint32_t kFileData = 0x08;
    static constexpr uint32_t kFileSize = 0x10;
    static constexpr uint32_t kFileHashNext = 0x18;
    static constexpr uint32_t kFileNameLength = 0x1c;
    static constexpr uint32_t kFileName = 0x20;
};

} // namespace threesf::horizon
