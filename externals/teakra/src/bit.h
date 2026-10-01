#pragma once

#include <limits>
#include <type_traits>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace std20 {

// A simple (and not very correct) implementation of C++20's std::log2p1
template <class T>
constexpr T log2p1(T x) noexcept {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    if (x == 0)
        return 0;
#ifdef _MSC_VER
    // _BitScanReverse64 exists only on 64-bit targets, so a 64-bit value is scanned a half at a
    // time.
    unsigned long index = 0;
    const unsigned long long v = x;
    if (v >> 32) {
        _BitScanReverse(&index, static_cast<unsigned long>(v >> 32));
        return static_cast<T>(index + 33);
    }
    _BitScanReverse(&index, static_cast<unsigned long>(v));
    return static_cast<T>(index + 1);
#else
    return static_cast<T>(std::numeric_limits<unsigned long long>::digits - __builtin_clzll(x));
#endif
}

} // namespace std20
