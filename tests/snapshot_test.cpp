// SPDX-License-Identifier: MIT

// Unit tests for the pieces of snapshots (save states) that need no game data: memory images (src/common/
// memory_image.h).

#include "common/memory_image.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "check.h"

using namespace threesf;

namespace
{

constexpr std::size_t kPage = MemoryImage::kPageSize;

std::vector<uint8_t> Restored(const MemoryImage& image)
{
    std::vector<uint8_t> bytes(image.Size(), 0xAA);
    image.Restore(bytes.data());
    return bytes;
}

void TestMemoryImages()
{
    std::mt19937 rng(1);
    // Ten and a half pages: random ones, zero ones and a partial one at the end.
    std::vector<uint8_t> memory(10 * kPage + kPage / 2);
    for (std::size_t i = 0; i < memory.size(); i++)
    {
        const std::size_t page = i / kPage;
        memory[i] = static_cast<uint8_t>(page == 2 || page == 7 ? 0 : rng());
    }

    const MemoryImage first(memory.data(), memory.size());
    THREESF_CHECK(first.Size() == memory.size());
    THREESF_CHECK(Restored(first) == memory);
    // Every page but the two zero ones takes memory, the partial one a whole page.
    THREESF_CHECK(first.BytesNotIn(nullptr) == 9 * kPage);

    // Change a byte in page 4, the last byte of the partial page, and turn page 5 into zeros and page 7 into data.
    std::vector<uint8_t> changed = memory;
    changed[4 * kPage + 100] ^= 1;
    changed.back() ^= 0x80;
    std::fill(changed.begin() + static_cast<std::ptrdiff_t>(5 * kPage),
              changed.begin() + static_cast<std::ptrdiff_t>(6 * kPage), uint8_t{0});
    changed[7 * kPage + 7] = 1;

    const MemoryImage second(changed.data(), changed.size(), &first);
    THREESF_CHECK(Restored(second) == changed);
    THREESF_CHECK(Restored(first) == memory); // the first image doesn't change
    // Pages 4, 7 and 10 are new; page 5 is zeros, which cost nothing; the rest are shared.
    THREESF_CHECK(second.BytesNotIn(&first) == 3 * kPage);
    THREESF_CHECK(second.BytesNotIn(nullptr) == 9 * kPage);

    // An image of unchanged memory shares everything.
    const MemoryImage third(changed.data(), changed.size(), &second);
    THREESF_CHECK(third.BytesNotIn(&second) == 0);
    THREESF_CHECK(Restored(third) == changed);

    // Restoring writes every byte, zero pages included.
    std::vector<uint8_t> target(changed.size(), 0x55);
    first.Restore(target.data());
    THREESF_CHECK(target == memory);

    // A previous image of a different size shares what it can.
    std::vector<uint8_t> longer = memory;
    longer.resize(12 * kPage, 3);
    const MemoryImage grown(longer.data(), longer.size(), &first);
    THREESF_CHECK(Restored(grown) == longer);
    THREESF_CHECK(grown.BytesNotIn(&first) == 2 * kPage); // pages 10 (now whole) and 11

    // An empty block.
    const MemoryImage empty(nullptr, 0);
    THREESF_CHECK(empty.Size() == 0);
    THREESF_CHECK(empty.BytesNotIn(nullptr) == 0);
}

} // namespace

int main()
{
    TestMemoryImages();

    if (failures)
    {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }

    std::printf("snapshot tests passed\n");
    return 0;
}
