#pragma once
// Big-endian loads and stores for the NASDAQ wire formats (ITCH, OUCH,
// MoldUDP64, SoupBinTCP), plus the fixed-width ASCII fields they use.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mde::wire {

inline std::uint16_t get16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}
inline std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
inline std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}
inline void put16(std::uint8_t* p, std::uint16_t v) {
    v = __builtin_bswap16(v);
    std::memcpy(p, &v, 2);
}
inline void put32(std::uint8_t* p, std::uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}
inline void put64(std::uint8_t* p, std::uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(p, &v, 8);
}
inline void put48(std::uint8_t* p, std::uint64_t v) {   // ITCH timestamps
    put16(p, static_cast<std::uint16_t>(v >> 32));
    put32(p + 2, static_cast<std::uint32_t>(v));
}

// Alpha field: left-justified, padded on the right with spaces.
inline void put_alpha(std::uint8_t* p, std::size_t width, std::string_view s) {
    const std::size_t n = s.size() < width ? s.size() : width;
    if (n) std::memcpy(p, s.data(), n);   // an empty view may hold a null pointer
    std::memset(p + n, ' ', width - n);
}
// Alpha field without its trailing spaces.
inline std::string_view get_alpha(const std::uint8_t* p, std::size_t width) {
    while (width > 0 && p[width - 1] == ' ') --width;
    return {reinterpret_cast<const char*>(p), width};
}

// SoupBinTCP numeric-in-ASCII field: right-justified, padded on the left with spaces.
inline void put_ascii_num(std::uint8_t* p, std::size_t width, std::uint64_t v) {
    std::memset(p, ' ', width);
    std::size_t i = width;
    do {
        p[--i] = static_cast<std::uint8_t>('0' + v % 10);
        v /= 10;
    } while (v != 0 && i > 0);
}
inline std::uint64_t get_ascii_num(const std::uint8_t* p, std::size_t width) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i)
        if (p[i] >= '0' && p[i] <= '9') v = v * 10 + (p[i] - '0');
    return v;
}

}  // namespace mde::wire
