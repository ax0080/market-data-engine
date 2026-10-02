#pragma once
// Order-independent fingerprint of an L3 book (every instrument, every level:
// price, quantity, order count). Two processes that rebuilt the same book from
// the same feed print the same value.

#include <cstdint>

#include "mde/l3_book.h"

namespace mde {

inline std::uint64_t book_hash(const L3Book& b) {
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a
    auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    for (std::uint32_t s = 0; s < 65536; ++s) {
        const L3Book::Instrument* in = b.instrument(static_cast<std::uint16_t>(s));
        if (!in || (in->bids.empty() && in->asks.empty())) continue;
        mix(s);
        for (const PriceLevels* side : {&in->bids, &in->asks}) {
            mix(side->depth());
            for (std::size_t k = 0; k < side->depth(); ++k) {
                mix(static_cast<std::uint64_t>(side->at(k).price));
                mix(static_cast<std::uint64_t>(side->at(k).qty));
                mix(side->at(k).orders);
            }
        }
    }
    return h;
}

}  // namespace mde
