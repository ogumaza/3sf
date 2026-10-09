// SPDX-License-Identifier: MIT

// Bounds-checked little-endian reads for the CTR sound formats.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#include "common/bytes.h"

namespace threesf::csar
{

// Reads little-endian values from a byte span. A read past the end throws std::runtime_error.
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data)
    {
    }

    std::size_t Size() const
    {
        return data_.size();
    }

    std::span<const uint8_t> Sub(std::size_t off, std::size_t len) const
    {
        Check(off, len);

        return data_.subspan(off, len);
    }

    uint8_t U8(std::size_t off) const
    {
        Check(off, 1);

        return data_[off];
    }

    uint16_t U16(std::size_t off) const
    {
        return Load<uint16_t>(off);
    }

    int16_t S16(std::size_t off) const
    {
        return Load<int16_t>(off);
    }

    uint32_t U32(std::size_t off) const
    {
        return Load<uint32_t>(off);
    }

    int32_t S32(std::size_t off) const
    {
        return Load<int32_t>(off);
    }

    float F32(std::size_t off) const
    {
        return Load<float>(off);
    }

    std::string CString(std::size_t off, std::size_t max_len) const
    {
        std::string s;
        while (off < data_.size() && data_[off] && s.size() < max_len)
        {
            s.push_back(static_cast<char>(data_[off++]));
        }

        return s;
    }

    bool Magic(std::size_t off, const char* m) const
    {
        Check(off, 4);

        return std::memcmp(data_.data() + off, m, 4) == 0;
    }

private:
    template <typename T>
    T Load(std::size_t off) const
    {
        Check(off, sizeof(T));

        const uint8_t* p = data_.data() + off;
        if constexpr (sizeof(T) == 2)
        {
            return std::bit_cast<T>(LoadLe16(p));
        }
        else
        {
            static_assert(sizeof(T) == 4);
            return std::bit_cast<T>(LoadLe32(p));
        }
    }

    void Check(std::size_t off, std::size_t len) const
    {
        if (off > data_.size() || len > data_.size() - off)
        {
            throw std::runtime_error("the data ends before offset " + std::to_string(off) +
                                     " (the file is cut short or damaged)");
        }
    }

    std::span<const uint8_t> data_;
};

// A CTR "reference": uint16_t type id, uint16_t padding, int32_t offset (relative to a base).
struct Reference
{
    uint16_t type = 0;
    int32_t offset = -1;
};

inline Reference ReadRef(const Reader& r, std::size_t off)
{
    return Reference{r.U16(off), r.S32(off + 4)};
}

} // namespace threesf::csar
