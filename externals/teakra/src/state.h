#pragma once

// Saving and loading the DSP's state (Teakra::SaveState and LoadState). Each component lists its
// fields once, in a Serialize function that it runs with a StateWriter to save them or with a
// StateReader to load them, so that saving and loading always agree.
//
// State data is only valid within the process that saved it. Values use the host's byte order, and
// structs include their padding.

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstring>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include "common_types.h"

namespace Teakra {

namespace StateDetail {
template <typename T>
struct IsBitset : std::false_type {};
template <std::size_t N>
struct IsBitset<std::bitset<N>> : std::true_type {};

template <typename T>
struct IsQueue : std::false_type {};
template <typename T>
struct IsQueue<std::queue<T>> : std::true_type {};

template <typename T>
struct IsAtomic : std::false_type {};
template <typename T>
struct IsAtomic<std::atomic<T>> : std::true_type {};

template <typename T>
struct IsArray : std::false_type {};
template <typename T, std::size_t N>
struct IsArray<std::array<T, N>> : std::true_type {};
} // namespace StateDetail

class StateWriter {
public:
    explicit StateWriter(std::vector<u8>& out) : out(out) {}

    template <typename... T>
    void operator()(T&... values) {
        (Value(values), ...);
    }

private:
    template <typename T>
    void Value(T& value) {
        if constexpr (StateDetail::IsBitset<T>::value) {
            static_assert(T().size() <= 64);
            Bytes(static_cast<u64>(value.to_ullong()));
        } else if constexpr (StateDetail::IsQueue<T>::value) {
            T copy = value;
            Bytes(static_cast<u64>(copy.size()));
            for (; !copy.empty(); copy.pop()) {
                Value(copy.front());
            }
        } else if constexpr (StateDetail::IsAtomic<T>::value) {
            Bytes(value.load());
        } else if constexpr (StateDetail::IsArray<T>::value) {
            for (auto& element : value) {
                Value(element);
            }
        } else {
            Bytes(value);
        }
    }

    template <typename T>
    void Bytes(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::size_t at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &value, sizeof(T));
    }

    std::vector<u8>& out;
};

class StateReader {
public:
    StateReader(const u8* data, std::size_t size) : data(data), size(size) {}

    template <typename... T>
    void operator()(T&... values) {
        (Value(values), ...);
    }

    // True when every byte has been read.
    bool AtEnd() const {
        return position == size;
    }

private:
    template <typename T>
    void Value(T& value) {
        if constexpr (StateDetail::IsBitset<T>::value) {
            value = T(Bytes<u64>());
        } else if constexpr (StateDetail::IsQueue<T>::value) {
            const u64 count = Bytes<u64>();
            value = T();
            for (u64 i = 0; i < count; ++i) {
                typename T::value_type element;
                Value(element);
                value.push(element);
            }
        } else if constexpr (StateDetail::IsAtomic<T>::value) {
            value.store(Bytes<typename T::value_type>());
        } else if constexpr (StateDetail::IsArray<T>::value) {
            for (auto& element : value) {
                Value(element);
            }
        } else {
            value = Bytes<T>();
        }
    }

    template <typename T>
    T Bytes() {
        static_assert(std::is_trivially_copyable_v<T>);
        if (size - position < sizeof(T)) {
            throw std::invalid_argument("Teakra: the saved state is cut short");
        }
        T value;
        std::memcpy(&value, data + position, sizeof(T));
        position += sizeof(T);
        return value;
    }

    const u8* data;
    std::size_t size;
    std::size_t position = 0;
};

} // namespace Teakra
