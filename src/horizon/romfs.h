// SPDX-License-Identifier: MIT

// Builds a RomFS level-3 image (the view an application gets of its RomFS through fs:USER OpenFileDirectly on archive
// 3) from a list of files. The game's nn::fs code parses the image, so the directory and file hash tables follow the
// real layout (see 3dbrew "RomFS"). File contents aren't copied: reads are served from the caller's buffers.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace threesf::horizon
{

class RomFs
{
public:
    struct File
    {
        std::string path; // e.g. "sound/xy_sound.bcsar" (no leading slash)
        std::shared_ptr<const std::vector<uint8_t>> data;
    };

    explicit RomFs(std::vector<File> files);

    uint64_t Size() const
    {
        return size_;
    }

    // Copies bytes [offset, offset + length) of the image; returns the number copied.
    uint64_t Read(uint64_t offset, uint64_t length, uint8_t* dst) const;

private:
    struct Placement
    {
        uint64_t offset; // offset of the file data within the image
        std::shared_ptr<const std::vector<uint8_t>> data;
    };

    std::vector<uint8_t> meta_; // header + tables, padded; file data follows at data_offset_
    std::vector<Placement> placements_;
    uint64_t data_offset_ = 0;
    uint64_t size_ = 0;
};

} // namespace threesf::horizon
