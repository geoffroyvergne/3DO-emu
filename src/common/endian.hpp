#pragma once

#include <bit>
#include <cstring>

#include "common/types.hpp"

// The 3DO runs big-endian: guest memory is stored as raw big-endian bytes and
// converted at the access boundary. (std::byteswap is C++23, hence the helper.)

constexpr u32 bswap32(u32 value) {
    return (value >> 24) | ((value >> 8) & 0x0000FF00) | ((value << 8) & 0x00FF0000) | (value << 24);
}

inline u32 load_be32(const u8* src) {
    u32 value;
    std::memcpy(&value, src, sizeof(value));
    if constexpr (std::endian::native == std::endian::little) value = bswap32(value);
    return value;
}

inline void store_be32(u8* dst, u32 value) {
    if constexpr (std::endian::native == std::endian::little) value = bswap32(value);
    std::memcpy(dst, &value, sizeof(value));
}
