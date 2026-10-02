#pragma once
// Schema-specific, zero-allocation JSON scanning for exchange feeds.
//
// A general JSON library builds a DOM (allocations, type dispatch, number
// conversion to double). Exchange messages have a fixed, known shape, so we scan
// the raw bytes for the few keys we need and convert decimal strings straight to
// fixed-point integers. Nothing is allocated and no floating point is involved.
// Inputs are trusted exchange payloads; on malformed input the functions return
// empty views or 0 rather than reading out of bounds.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mde/types.h"

namespace mde::json {

// Position just after `"key":` at or after `from`, or npos.
inline std::size_t after_key(std::string_view s, std::string_view key, std::size_t from = 0) {
    for (std::size_t p = s.find(key, from); p != std::string_view::npos; p = s.find(key, p + 1)) {
        if (p == 0 || s[p - 1] != '"') continue;
        const std::size_t q = p + key.size();
        if (q + 1 < s.size() && s[q] == '"' && s[q + 1] == ':') return q + 2;
    }
    return std::string_view::npos;
}

// Unsigned integer value of `"key":123`.
inline std::uint64_t get_uint(std::string_view s, std::string_view key, std::size_t from = 0) {
    std::size_t p = after_key(s, key, from);
    if (p == std::string_view::npos) return 0;
    std::uint64_t v = 0;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') v = v * 10 + static_cast<std::uint64_t>(s[p++] - '0');
    return v;
}

// Contents of a quoted string value `"key":"..."` (without quotes).
inline std::string_view get_str(std::string_view s, std::string_view key, std::size_t from = 0,
                                std::size_t* end = nullptr) {
    const std::size_t p = after_key(s, key, from);
    if (p == std::string_view::npos || p >= s.size() || s[p] != '"') return {};
    const std::size_t q = s.find('"', p + 1);
    if (q == std::string_view::npos) return {};
    if (end) *end = q + 1;
    return s.substr(p + 1, q - p - 1);
}

// "86324.95000000" -> 8632495000000 (8 decimal places). Extra digits are truncated.
inline std::int64_t parse_fixed(std::string_view d) {
    std::int64_t ip = 0, fp = 0;
    std::size_t i = 0;
    bool neg = false;
    if (i < d.size() && d[i] == '-') {
        neg = true;
        ++i;
    }
    while (i < d.size() && d[i] >= '0' && d[i] <= '9') ip = ip * 10 + (d[i++] - '0');
    int frac_digits = 0;
    if (i < d.size() && d[i] == '.') {
        ++i;
        while (i < d.size() && d[i] >= '0' && d[i] <= '9') {
            if (frac_digits < 8) {
                fp = fp * 10 + (d[i] - '0');
                ++frac_digits;
            }
            ++i;
        }
    }
    static constexpr std::int64_t pow10[9] = {100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
    const std::int64_t v = ip * kScale + fp * pow10[frac_digits];
    return neg ? -v : v;
}

// Iterates `[["price","qty"],["price","qty"],...]` starting at the array's '['.
// Calls f(price, qty) for each pair; returns the position after the closing ']'.
template <class F>
std::size_t for_each_pair(std::string_view s, std::size_t open, F&& f) {
    if (open >= s.size() || s[open] != '[') return std::string_view::npos;
    std::size_t p = open + 1;
    while (p < s.size()) {
        while (p < s.size() && (s[p] == ',' || s[p] == ' ')) ++p;
        if (p >= s.size()) break;
        if (s[p] == ']') return p + 1;
        if (s[p] != '[') return std::string_view::npos;
        // ["price","qty"]  (Coinbase REST adds a third element: order count)
        const std::size_t a1 = s.find('"', p);
        const std::size_t a2 = s.find('"', a1 + 1);
        const std::size_t b1 = s.find('"', a2 + 1);
        const std::size_t b2 = s.find('"', b1 + 1);
        if (b2 == std::string_view::npos) return std::string_view::npos;
        f(parse_fixed(s.substr(a1 + 1, a2 - a1 - 1)), parse_fixed(s.substr(b1 + 1, b2 - b1 - 1)));
        p = s.find(']', b2);
        if (p == std::string_view::npos) return p;
        ++p;
    }
    return std::string_view::npos;
}

}  // namespace mde::json
