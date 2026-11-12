// SPDX-License-Identifier: MIT

// BLZ decompression (see blz.h).

#include "rip/blz.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "xsflib/bytes.h"

namespace threesf::rip
{
namespace
{

// The largest output: 1 GiB, the size of the largest file that the game loader reads into memory (game.cpp). A damaged
// footer can't make the decompression allocate more.
constexpr uint64_t kMaxOutputSize = 1ull << 30;

} // namespace

std::optional<std::vector<uint8_t>> BlzDecompress(const std::vector<uint8_t>& data)
{
    if (data.size() < 8)
    {
        return std::nullopt;
    }

    const uint32_t top_bottom = xsflib::LoadLe32(&data[data.size() - 8]);
    const uint32_t extra = xsflib::LoadLe32(&data[data.size() - 4]);
    const std::size_t top = top_bottom & 0xffffff, bottom = top_bottom >> 24;
    if (bottom < 8 || bottom > 11 || top < bottom || top > data.size())
    {
        return std::nullopt;
    }

    const uint64_t out_size = data.size() + static_cast<uint64_t>(extra);
    if (out_size > kMaxOutputSize)
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

} // namespace threesf::rip
