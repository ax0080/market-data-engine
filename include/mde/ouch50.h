#pragma once
// NASDAQ OUCH 5.0 order entry messages (carried in SoupBinTCP data packets).
//
// Field offsets follow the NASDAQ OUCH 5.0 specification. Numbers are
// big-endian binary; prices are 8 bytes with 4 implied decimals; alpha fields
// are space-padded on the right. Every message ends in an Appendage Length
// field; this implementation sends no optional appendage (length 0) and skips
// any it receives.
//
// Client -> exchange (inbound): Enter Order 'O', Replace Order 'U', Cancel Order 'X'.
// Exchange -> client (outbound): System Event 'S', Accepted 'A', Replaced 'U',
//   Canceled 'C', Executed 'E', Rejected 'J'.
//
// Orders are named by the client's UserRefNum, which must strictly increase
// within a trading day; the exchange answers with its own Order Reference Number.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "mde/wire.h"

namespace mde::ouch {

// Message lengths without optional appendage
inline constexpr std::size_t kEnterOrderLen = 47;
inline constexpr std::size_t kReplaceOrderLen = 40;
inline constexpr std::size_t kCancelOrderLen = 11;
inline constexpr std::size_t kSystemEventLen = 10;
inline constexpr std::size_t kAcceptedLen = 64;
inline constexpr std::size_t kReplacedLen = 68;
inline constexpr std::size_t kCanceledLen = 20;
inline constexpr std::size_t kExecutedLen = 36;
inline constexpr std::size_t kRejectedLen = 31;
inline constexpr std::size_t kMaxMessageLen = kReplacedLen;

// Time In Force
inline constexpr char kTifDay = '0';
inline constexpr char kTifIoc = '3';
// Order State
inline constexpr char kStateLive = 'L';
inline constexpr char kStateDead = 'D';
// Cancel reasons (subset)
inline constexpr char kCancelUser = 'U';
inline constexpr char kCancelIoc = 'I';
inline constexpr char kCancelSelfMatch = 'Q';
// Reject reasons (subset)
inline constexpr std::uint16_t kRejectDestinationClosed = 0x0002;
inline constexpr std::uint16_t kRejectOther = 0x000F;
inline constexpr std::uint16_t kRejectInvalidQuantity = 0x0013;
inline constexpr std::uint16_t kRejectInvalidSymbol = 0x0017;
inline constexpr std::uint16_t kRejectInvalidPrice = 0x001D;
// Liquidity flags (subset)
inline constexpr char kLiquidityAdded = 'A';
inline constexpr char kLiquidityRemoved = 'R';

using ClOrdId = char[14];

// ------------------------------------------------------------------- inbound

struct EnterOrder {
    std::uint32_t user_ref;
    char side;                 // B, S, T (short), E (short exempt)
    std::uint32_t qty;
    std::string_view symbol;   // up to 8 chars
    std::uint64_t price;       // 4 implied decimals
    char tif = kTifDay;
    char display = 'Y';
    char capacity = 'P';
    char ise = 'N';
    char cross = 'N';
    std::string_view clordid;  // up to 14 chars
};

struct ReplaceOrder {
    std::uint32_t orig_user_ref;
    std::uint32_t user_ref;
    std::uint32_t qty;         // total liable for the chain, including executed shares
    std::uint64_t price;
    char tif = kTifDay;
    char display = 'Y';
    char ise = 'N';
    std::string_view clordid;
};

struct CancelOrder {
    std::uint32_t user_ref;
    std::uint32_t qty;         // new intended open size; 0 cancels the order
};

inline std::size_t put(std::uint8_t* m, const EnterOrder& o) {
    m[0] = 'O';
    wire::put32(m + 1, o.user_ref);
    m[5] = static_cast<std::uint8_t>(o.side);
    wire::put32(m + 6, o.qty);
    wire::put_alpha(m + 10, 8, o.symbol);
    wire::put64(m + 18, o.price);
    m[26] = static_cast<std::uint8_t>(o.tif);
    m[27] = static_cast<std::uint8_t>(o.display);
    m[28] = static_cast<std::uint8_t>(o.capacity);
    m[29] = static_cast<std::uint8_t>(o.ise);
    m[30] = static_cast<std::uint8_t>(o.cross);
    wire::put_alpha(m + 31, 14, o.clordid);
    wire::put16(m + 45, 0);
    return kEnterOrderLen;
}

inline std::size_t put(std::uint8_t* m, const ReplaceOrder& o) {
    m[0] = 'U';
    wire::put32(m + 1, o.orig_user_ref);
    wire::put32(m + 5, o.user_ref);
    wire::put32(m + 9, o.qty);
    wire::put64(m + 13, o.price);
    m[21] = static_cast<std::uint8_t>(o.tif);
    m[22] = static_cast<std::uint8_t>(o.display);
    m[23] = static_cast<std::uint8_t>(o.ise);
    wire::put_alpha(m + 24, 14, o.clordid);
    wire::put16(m + 38, 0);
    return kReplaceOrderLen;
}

inline std::size_t put(std::uint8_t* m, const CancelOrder& o) {
    m[0] = 'X';
    wire::put32(m + 1, o.user_ref);
    wire::put32(m + 5, o.qty);
    wire::put16(m + 9, 0);
    return kCancelOrderLen;
}

// Decodes one inbound message; calls h.on_enter / on_replace / on_cancel.
// Returns false for an unknown type or a short message.
template <class H>
bool decode_inbound(const std::uint8_t* m, std::size_t len, H& h) {
    if (len == 0) return false;
    switch (m[0]) {
        case 'O': {
            if (len < kEnterOrderLen) return false;
            EnterOrder o;
            o.user_ref = wire::get32(m + 1);
            o.side = static_cast<char>(m[5]);
            o.qty = wire::get32(m + 6);
            o.symbol = wire::get_alpha(m + 10, 8);
            o.price = wire::get64(m + 18);
            o.tif = static_cast<char>(m[26]);
            o.display = static_cast<char>(m[27]);
            o.capacity = static_cast<char>(m[28]);
            o.ise = static_cast<char>(m[29]);
            o.cross = static_cast<char>(m[30]);
            o.clordid = wire::get_alpha(m + 31, 14);
            h.on_enter(o);
            return true;
        }
        case 'U': {
            if (len < kReplaceOrderLen) return false;
            ReplaceOrder o;
            o.orig_user_ref = wire::get32(m + 1);
            o.user_ref = wire::get32(m + 5);
            o.qty = wire::get32(m + 9);
            o.price = wire::get64(m + 13);
            o.tif = static_cast<char>(m[21]);
            o.display = static_cast<char>(m[22]);
            o.ise = static_cast<char>(m[23]);
            o.clordid = wire::get_alpha(m + 24, 14);
            h.on_replace(o);
            return true;
        }
        case 'X': {
            if (len < 9) return false;   // Appendage Length is optional on Cancel
            h.on_cancel(CancelOrder{wire::get32(m + 1), wire::get32(m + 5)});
            return true;
        }
        default:
            return false;
    }
}

// ------------------------------------------------------------------ outbound

struct Accepted {
    std::uint64_t ts_ns;
    std::uint32_t user_ref;
    char side;
    std::uint32_t qty;
    std::string_view symbol;
    std::uint64_t price;
    char tif;
    char display;
    std::uint64_t order_ref;   // exchange-assigned Order Reference Number
    char capacity;
    char ise;
    char cross;
    char state;                // L live, D dead (accepted and already cancelled)
    std::string_view clordid;
};

struct Replaced {
    std::uint64_t ts_ns;
    std::uint32_t orig_user_ref;
    std::uint32_t user_ref;
    char side;
    std::uint32_t qty;         // shares outstanding after the replace
    std::string_view symbol;
    std::uint64_t price;
    char tif;
    char display;
    std::uint64_t order_ref;
    char capacity;
    char ise;
    char cross;
    char state;
    std::string_view clordid;
};

struct Canceled {
    std::uint64_t ts_ns;
    std::uint32_t user_ref;
    std::uint32_t qty;         // shares removed by this message (incremental)
    char reason;
};

struct Executed {
    std::uint64_t ts_ns;
    std::uint32_t user_ref;
    std::uint32_t qty;
    std::uint64_t price;
    char liquidity;
    std::uint64_t match;
};

struct Rejected {
    std::uint64_t ts_ns;
    std::uint32_t user_ref;
    std::uint16_t reason;
    std::string_view clordid;
};

inline std::size_t put_system_event(std::uint8_t* m, std::uint64_t ts_ns, char code) {
    m[0] = 'S';
    wire::put64(m + 1, ts_ns);
    m[9] = static_cast<std::uint8_t>(code);
    return kSystemEventLen;
}

inline std::size_t put(std::uint8_t* m, const Accepted& a) {
    m[0] = 'A';
    wire::put64(m + 1, a.ts_ns);
    wire::put32(m + 9, a.user_ref);
    m[13] = static_cast<std::uint8_t>(a.side);
    wire::put32(m + 14, a.qty);
    wire::put_alpha(m + 18, 8, a.symbol);
    wire::put64(m + 26, a.price);
    m[34] = static_cast<std::uint8_t>(a.tif);
    m[35] = static_cast<std::uint8_t>(a.display);
    wire::put64(m + 36, a.order_ref);
    m[44] = static_cast<std::uint8_t>(a.capacity);
    m[45] = static_cast<std::uint8_t>(a.ise);
    m[46] = static_cast<std::uint8_t>(a.cross);
    m[47] = static_cast<std::uint8_t>(a.state);
    wire::put_alpha(m + 48, 14, a.clordid);
    wire::put16(m + 62, 0);
    return kAcceptedLen;
}

inline std::size_t put(std::uint8_t* m, const Replaced& r) {
    m[0] = 'U';
    wire::put64(m + 1, r.ts_ns);
    wire::put32(m + 9, r.orig_user_ref);
    wire::put32(m + 13, r.user_ref);
    m[17] = static_cast<std::uint8_t>(r.side);
    wire::put32(m + 18, r.qty);
    wire::put_alpha(m + 22, 8, r.symbol);
    wire::put64(m + 30, r.price);
    m[38] = static_cast<std::uint8_t>(r.tif);
    m[39] = static_cast<std::uint8_t>(r.display);
    wire::put64(m + 40, r.order_ref);
    m[48] = static_cast<std::uint8_t>(r.capacity);
    m[49] = static_cast<std::uint8_t>(r.ise);
    m[50] = static_cast<std::uint8_t>(r.cross);
    m[51] = static_cast<std::uint8_t>(r.state);
    wire::put_alpha(m + 52, 14, r.clordid);
    wire::put16(m + 66, 0);
    return kReplacedLen;
}

inline std::size_t put(std::uint8_t* m, const Canceled& c) {
    m[0] = 'C';
    wire::put64(m + 1, c.ts_ns);
    wire::put32(m + 9, c.user_ref);
    wire::put32(m + 13, c.qty);
    m[17] = static_cast<std::uint8_t>(c.reason);
    wire::put16(m + 18, 0);
    return kCanceledLen;
}

inline std::size_t put(std::uint8_t* m, const Executed& e) {
    m[0] = 'E';
    wire::put64(m + 1, e.ts_ns);
    wire::put32(m + 9, e.user_ref);
    wire::put32(m + 13, e.qty);
    wire::put64(m + 17, e.price);
    m[25] = static_cast<std::uint8_t>(e.liquidity);
    wire::put64(m + 26, e.match);
    wire::put16(m + 34, 0);
    return kExecutedLen;
}

inline std::size_t put(std::uint8_t* m, const Rejected& r) {
    m[0] = 'J';
    wire::put64(m + 1, r.ts_ns);
    wire::put32(m + 9, r.user_ref);
    wire::put16(m + 13, r.reason);
    wire::put_alpha(m + 15, 14, r.clordid);
    wire::put16(m + 29, 0);
    return kRejectedLen;
}

// Decodes one outbound message; calls h.on_system_event / on_accepted /
// on_replaced / on_canceled / on_executed / on_rejected. Returns false for an
// unknown type or a short message. (Canceled and Rejected may omit their
// Appendage Length field, so it is not required.)
template <class H>
bool decode_outbound(const std::uint8_t* m, std::size_t len, H& h) {
    if (len == 0) return false;
    switch (m[0]) {
        case 'S':
            if (len < kSystemEventLen) return false;
            h.on_system_event(wire::get64(m + 1), static_cast<char>(m[9]));
            return true;
        case 'A': {
            if (len < kAcceptedLen) return false;
            h.on_accepted(Accepted{wire::get64(m + 1), wire::get32(m + 9), static_cast<char>(m[13]),
                                   wire::get32(m + 14), wire::get_alpha(m + 18, 8), wire::get64(m + 26),
                                   static_cast<char>(m[34]), static_cast<char>(m[35]), wire::get64(m + 36),
                                   static_cast<char>(m[44]), static_cast<char>(m[45]), static_cast<char>(m[46]),
                                   static_cast<char>(m[47]), wire::get_alpha(m + 48, 14)});
            return true;
        }
        case 'U': {
            if (len < kReplacedLen) return false;
            h.on_replaced(Replaced{wire::get64(m + 1), wire::get32(m + 9), wire::get32(m + 13),
                                   static_cast<char>(m[17]), wire::get32(m + 18), wire::get_alpha(m + 22, 8),
                                   wire::get64(m + 30), static_cast<char>(m[38]), static_cast<char>(m[39]),
                                   wire::get64(m + 40), static_cast<char>(m[48]), static_cast<char>(m[49]),
                                   static_cast<char>(m[50]), static_cast<char>(m[51]), wire::get_alpha(m + 52, 14)});
            return true;
        }
        case 'C':
            if (len < 18) return false;
            h.on_canceled(Canceled{wire::get64(m + 1), wire::get32(m + 9), wire::get32(m + 13), static_cast<char>(m[17])});
            return true;
        case 'E':
            if (len < 34) return false;
            h.on_executed(Executed{wire::get64(m + 1), wire::get32(m + 9), wire::get32(m + 13), wire::get64(m + 17),
                                   static_cast<char>(m[25]), wire::get64(m + 26)});
            return true;
        case 'J':
            if (len < 29) return false;
            h.on_rejected(Rejected{wire::get64(m + 1), wire::get32(m + 9), wire::get16(m + 13), wire::get_alpha(m + 15, 14)});
            return true;
        default:
            return false;
    }
}

}  // namespace mde::ouch
