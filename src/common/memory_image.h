// SPDX-License-Identifier: MIT

// A memory snapshot stored in 4 KiB pages. Unchanged pages are shared with the previous image of the same block; zero
// pages need no storage. Images are immutable and can be shared between threads.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace threesf
{

class MemoryImage
{
public:
    static constexpr std::size_t kPageSize = 4096;

    MemoryImage() = default;

    // Copies the `size` bytes at `data`. A page that equals the one at the same offset in `previous` is shared with it.
    MemoryImage(const uint8_t* data, std::size_t size, const MemoryImage* previous = nullptr) : size_(size)
    {
        pages_.resize((size + kPageSize - 1) / kPageSize);
        for (std::size_t i = 0; i < pages_.size(); i++)
        {
            const uint8_t* bytes = data + i * kPageSize;
            const std::size_t n = std::min(kPageSize, size - i * kPageSize);
            if (previous && i < previous->pages_.size())
            {
                const std::shared_ptr<const Page>& before = previous->pages_[i];
                if (std::memcmp(bytes, before ? before->data() : Zeros().data(), n) == 0)
                {
                    pages_[i] = before;
                    continue;
                }
            }

            if (std::memcmp(bytes, Zeros().data(), n) == 0)
            {
                continue;
            }

            auto page = std::make_shared<Page>();
            std::memcpy(page->data(), bytes, n);
            pages_[i] = std::move(page);
        }
    }

    // Copies the image back to `data`, which holds Size() bytes.
    void Restore(uint8_t* data) const
    {
        for (std::size_t i = 0; i < pages_.size(); i++)
        {
            const std::size_t n = std::min(kPageSize, size_ - i * kPageSize);
            if (pages_[i])
            {
                std::memcpy(data + i * kPageSize, pages_[i]->data(), n);
            }
            else
            {
                std::memset(data + i * kPageSize, 0, n);
            }
        }
    }

    std::size_t Size() const
    {
        return size_;
    }

    // Bytes stored in pages not shared with `other`, or in all stored pages if `other` is null.
    std::size_t BytesNotIn(const MemoryImage* other) const
    {
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < pages_.size(); i++)
        {
            if (pages_[i] && !(other && i < other->pages_.size() && other->pages_[i] == pages_[i]))
            {
                bytes += kPageSize;
            }
        }

        return bytes;
    }

private:
    using Page = std::array<uint8_t, kPageSize>;

    static const Page& Zeros()
    {
        static const Page zeros{};
        return zeros;
    }

    std::size_t size_ = 0;
    std::vector<std::shared_ptr<const Page>> pages_; // nullptr for a page of zeros
};

} // namespace threesf
