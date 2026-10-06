// Tick-to-trade client (Linux): market data in, order out, on one pinned core.
//
//   UDP  ITCH 5.0 / MoldUDP64 -> GapFiller -> ITCH decoder -> L3 book -> rule -> OUCH order
//   TCP  SoupBinTCP replay to fill any gap the UDP feed had (connects only while needed)
//   TCP  OUCH 5.0 order entry over SoupBinTCP, or FIX 4.4 (--oe fix)
//
// Rule: when a bid of at least --threshold shares is added to the book, buy
// --qty shares IOC at the current best ask. The order is encoded and sent from
// inside the decode callback of the packet that carried the signal, with no
// queue or thread hop in between. Signals seen while catching up (buffered or
// replayed messages) are not traded: the book is stale then.
//
// Measured on this host (CLOCK_MONOTONIC):
//   tick-to-trade   receive call returned the packet -> order handed to send()
//   order RTT       send() -> order accepted (OUCH Accepted / FIX ExecutionReport ExecType=New)
// The exchange simulator separately measures trigger sendto() -> order recv().
//
// usage: trader [--exchange 127.0.0.1] [--md-port 31007] [--oe-port 31100] [--rec-port 31101]
//               [--rx recvmmsg|recvfrom|xdp] [--ifname eth0] [--zc] [--native] [--group 239.x.x.x]
//               [--threshold 5000] [--qty 100] [--cpu -1] [--user TRADER] [--oe ouch|fix] [--fix-port 31102]

#if !defined(__linux__)
#include <cstdio>
int main() {
    std::puts("trader needs Linux");
    return 0;
}
#else

#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mde/af_xdp.h"
#include "mde/book_hash.h"
#include "mde/fix44.h"
#include "mde/gap_filler.h"
#include "mde/itch50.h"
#include "mde/itch50_writer.h"
#include "mde/l3_book.h"
#include "mde/linux_net.h"
#include "mde/ouch50.h"
#include "mde/soupbintcp.h"

using namespace mde;

namespace {

constexpr std::uint64_t kSecond = 1'000'000'000;
constexpr unsigned kBatch = 64;

struct Config {
    std::string exchange = "127.0.0.1";
    std::uint16_t md_port = 31007, oe_port = 31100, rec_port = 31101, fix_port = 31102;
    std::string oe = "ouch";
    std::string rx = "recvmmsg";
    std::string ifname = "eth0";
    bool zc = false, native = false;
    std::string group;
    std::uint32_t threshold = 5000, qty = 100;
    int cpu = -1;
    std::string user = "TRADER";
    double timeout_s = 120;
};

class Trader {
public:
    explicit Trader(const Config& c)
        : c_(c), book_(std::make_unique<L3Book>(std::size_t{1} << 20)), strat_{this}, dec_(strat_), feed_{this},
          filler_(feed_) {
        sent_ns_.reserve(1 << 20);
    }

    int run() {
        net::pin_cpu(c_.cpu);
        // Market data socket first, so nothing is missed once the exchange starts.
        std::unique_ptr<xdp::Socket> xsk;
        int absorb = -1;
        if (c_.rx == "xdp") {
            xdp::Options o;
            o.ifname = c_.ifname;
            o.udp_port = c_.md_port;
            o.zero_copy = c_.zc;
            o.native = c_.native;
            xsk = std::make_unique<xdp::Socket>(o);
            absorb = net::udp_receiver(c_.md_port);   // keeps the port bound: no ICMP for packets XDP passes up
        } else {
            md_fd_ = net::udp_receiver(c_.md_port, c_.group);
        }
        if (fix_) {
            fix_conn_ = std::make_unique<net::FixConn>(net::tcp_connect(c_.exchange, c_.fix_port));
            fix_session_ = std::make_unique<fix::Session>(c_.user, "MDESIM", false);
            const auto m = fix_session_->logon(net::epoch_ns());
            fix_conn_->send(m.first, m.second);
        } else {
            oe_ = std::make_unique<net::Conn>(net::tcp_connect(c_.exchange, c_.oe_port));
            std::uint8_t login[soup::kLoginRequestLen];
            oe_->send(login, soup::put_login_request(login, c_.user, "pass", "", 1));
        }
        std::printf("trader: %s market data on :%u, %s to %s:%u (rule: bid >= %u shares -> buy %u IOC at best ask)\n",
                    c_.rx.c_str(), c_.md_port, fix_ ? "FIX 4.4" : "OUCH/SoupBinTCP", c_.exchange.c_str(),
                    fix_ ? c_.fix_port : c_.oe_port, c_.threshold, c_.qty);
        std::fflush(stdout);

        std::vector<std::uint8_t> bufs(kBatch * 2048);
        mmsghdr msgs[kBatch];
        iovec iov[kBatch];
        for (unsigned j = 0; j < kBatch; ++j) {
            iov[j] = {bufs.data() + j * 2048, 2048};
            msgs[j] = {};
            msgs[j].msg_hdr.msg_iov = &iov[j];
            msgs[j].msg_hdr.msg_iovlen = 1;
        }
        const bool batch = c_.rx == "recvmmsg";
        const std::uint64_t deadline = net::mono_ns() + std::uint64_t(c_.timeout_s * 1e9);
        std::uint64_t it = 0;

        while (!done()) {
            // 1. market data
            // rx_ns_ is stamped once per batch on every path: a packet behind others
            // in the same batch waits for them, and that wait is part of its latency.
            if (xsk) {
                bool first = true;
                xsk->poll_once([&](const std::uint8_t* p, std::size_t n) {
                    if (first) {
                        rx_ns_ = net::mono_ns();
                        first = false;
                    }
                    filler_.on_packet(p, n);
                });
            } else if (batch) {
                const int got = recvmmsg(md_fd_, msgs, kBatch, MSG_DONTWAIT, nullptr);
                if (got > 0) {
                    rx_ns_ = net::mono_ns();
                    for (int j = 0; j < got; ++j) filler_.on_packet(bufs.data() + j * 2048, msgs[j].msg_len);
                }
            } else {
                const ssize_t n = recv(md_fd_, bufs.data(), 2048, MSG_DONTWAIT);
                if (n > 0) {
                    rx_ns_ = net::mono_ns();
                    filler_.on_packet(bufs.data(), static_cast<std::size_t>(n));
                }
            }
            // 2. everything else, less often: it costs syscalls the hot path does not need
            if ((++it & 15) == 0) {
                const std::uint64_t now = net::mono_ns();
                service_order_entry(now);
                service_recovery(now);
                if (now > deadline) {
                    std::fprintf(stderr, "trader: timed out\n");
                    break;
                }
            }
        }
        // Final OUCH replies, then log out.
        const std::uint64_t drain_end = net::mono_ns() + kSecond / 2;
        while (net::mono_ns() < drain_end) service_order_entry(net::mono_ns());
        if (fix_) {
            fix_session_->set_logout_sent();
            const auto m = fix_session_->logout(net::epoch_ns());
            fix_conn_->send(m.first, m.second);
            fix_conn_->flush();
        } else {
            std::uint8_t bye[soup::kHeaderLen];
            oe_->send(bye, soup::put_empty(bye, soup::kLogoutRequest));
        }
        if (absorb >= 0) close(absorb);
        report();
        return ended_ ? 0 : 1;
    }

    // ---- OUCH outbound handler (ouch::decode_outbound) ----
    void on_system_event(std::uint64_t, char) {}
    void on_accepted(const ouch::Accepted& a) {
        ++accepted_;
        if (a.user_ref < sent_ns_.size() && sent_ns_[a.user_ref]) rtt_.add(net::mono_ns() - sent_ns_[a.user_ref]);
    }
    void on_replaced(const ouch::Replaced&) {}
    void on_canceled(const ouch::Canceled&) { ++canceled_; }
    void on_executed(const ouch::Executed& e) {
        ++fills_;
        filled_shares_ += e.qty;
    }
    void on_rejected(const ouch::Rejected&) { ++rejected_; }

    // ---- FIX application messages (fix::Session) ----
    void on_app(const fix::Message& m) {
        if (m.type() == '9') {
            ++rejected_;
            return;
        }
        if (m.type() != '8') return;
        switch (m.get(fix::tag::ExecType)[0]) {
            case '0': {
                ++accepted_;
                const auto it = fix_sent_.find(fix::parse_uint(m.get(fix::tag::ClOrdID).substr(1)));
                if (it != fix_sent_.end()) {
                    rtt_.add(net::mono_ns() - it->second);
                    fix_sent_.erase(it);
                }
                break;
            }
            case 'F':
                ++fills_;
                filled_shares_ += m.get_uint(fix::tag::LastQty);
                break;
            case '4': ++canceled_; break;
            case '8': ++rejected_; break;
            default: break;
        }
    }

private:
    // ITCH events -> book -> rule
    struct Strategy {
        Trader* t;
        void on_event(const BookEvent& e) {
            t->book_->on_event(e);
            if (e.type == EventType::AddOrder && e.side == Side::Buy && e.qty >= Qty{t->c_.threshold} * kScale) t->signal(e);
        }
    };
    // MoldUDP64 payload -> ITCH decoder, watching for End of Messages
    struct Feed {
        Trader* t;
        void decode(const std::uint8_t* m, std::size_t len) {
            if (m[0] == 'S' && len >= 12 && m[11] == itch::kEndOfMessages) t->ended_ = true;
            t->dec_.decode(m, len);
        }
    };

    void signal(const BookEvent& e) {
        ++signals_;
        if (!filler_.live()) {   // the book is catching up: do not trade on old news
            ++stale_signals_;
            return;
        }
        const L3Book::Instrument* in = book_->instrument(e.symbol);
        if (!in || in->asks.empty() || !oe_logged_in_) return;
        const auto ask4 = static_cast<std::uint64_t>(in->asks.best().price / itch::kPriceMul);
        if (fix_) {
            send_fix_order(e, ask4);
            return;
        }

        // Build the SoupBinTCP + OUCH Enter Order in place and send it.
        ouch::EnterOrder o{};
        o.user_ref = ++user_ref_;
        o.side = 'B';
        o.qty = c_.qty;
        o.symbol = dec_.symbol_name(e.symbol);
        o.price = ask4;
        o.tif = ouch::kTifIoc;
        char clordid[16];
        const int n = std::snprintf(clordid, sizeof clordid, "T%llu", (unsigned long long)filler_.current_sequence());
        o.clordid = std::string_view(clordid, static_cast<std::size_t>(n));
        std::uint8_t pkt[soup::kHeaderLen + ouch::kEnterOrderLen];
        const std::size_t len = soup::seal_data(pkt, soup::kUnsequencedData, ouch::put(pkt + soup::kHeaderLen, o));
        const std::uint64_t t0 = net::mono_ns();
        oe_->send(pkt, len);
        const std::uint64_t t1 = net::mono_ns();
        t2t_.add(t0 - rx_ns_);
        t2t_send_.add(t1 - rx_ns_);
        if (o.user_ref >= sent_ns_.size()) sent_ns_.resize(o.user_ref + 1024, 0);
        sent_ns_[o.user_ref] = t1;
        ++orders_;
    }

    // NewOrderSingle: limit IOC buy at the best ask; ClOrdID carries the trigger's
    // ITCH sequence number, as the OUCH path does.
    void send_fix_order(const BookEvent& e, std::uint64_t ask4) {
        const std::uint64_t trigger = filler_.current_sequence();
        char clordid[24];
        const int n = std::snprintf(clordid, sizeof clordid, "T%llu", (unsigned long long)trigger);
        const std::string_view symbol = dec_.symbol_name(e.symbol);
        const std::uint64_t now = net::epoch_ns();
        const auto m = fix_session_->build('D', now, [&](fix::Encoder& enc) {
            enc.field(fix::tag::ClOrdID, std::string_view(clordid, static_cast<std::size_t>(n)));
            enc.field(fix::tag::Symbol, symbol.substr(0, symbol.find_last_not_of(' ') + 1));
            enc.field_char(fix::tag::Side, '1');
            enc.field_uint(fix::tag::OrderQty, c_.qty);
            enc.field_char(fix::tag::OrdType, '2');
            enc.field_price(fix::tag::Price, ask4);
            enc.field_char(fix::tag::TimeInForce, '3');
            enc.field_time(fix::tag::TransactTime, now);
        });
        const std::uint64_t t0 = net::mono_ns();
        fix_conn_->send(m.first, m.second);
        const std::uint64_t t1 = net::mono_ns();
        t2t_.add(t0 - rx_ns_);
        t2t_send_.add(t1 - rx_ns_);
        fix_sent_[trigger] = t1;
        ++orders_;
    }

    void service_order_entry(std::uint64_t now) {
        if (fix_) {
            service_fix(now);
            return;
        }
        if (!oe_->read()) {
            if (oe_->open()) std::fprintf(stderr, "trader: order entry connection closed\n");
            oe_->close();
            return;
        }
        oe_->in().drain([&](char type, const std::uint8_t* p, std::size_t n) {
            if (type == soup::kSequencedData) ouch::decode_outbound(p, n, *this);
            else if (type == soup::kLoginAccepted) oe_logged_in_ = true;
            else if (type == soup::kLoginRejected) std::fprintf(stderr, "trader: login rejected\n");
            return true;
        });
        oe_->flush();
        if (oe_logged_in_ && now > oe_->last_tx_ns + kSecond) {
            std::uint8_t hb[soup::kHeaderLen];
            oe_->send(hb, soup::put_empty(hb, soup::kClientHeartbeat));
        }
    }

    void service_fix(std::uint64_t now) {
        auto out = [&](const std::uint8_t* p, std::size_t n) { fix_conn_->send(p, n); };
        if (!fix_conn_->read()) {
            if (fix_conn_->open()) std::fprintf(stderr, "trader: FIX connection closed\n");
            fix_conn_->close();
            return;
        }
        fix_conn_->in().drain([&](const std::uint8_t* p, std::size_t n) {
            fix_session_->on_message(p, n, net::epoch_ns(), *this, out);
            return true;
        });
        oe_logged_in_ = fix_session_->logged_on();
        fix_conn_->flush();
        fix_session_->tick(net::epoch_ns(), net::epoch_ns() - (now - fix_conn_->last_tx_ns), out);
    }

    // Opens a SoupBinTCP replay from the first missing message while the
    // filler has a gap, feeds it, and logs out as soon as the gap is closed.
    void service_recovery(std::uint64_t) {
        if (filler_.recovering() && !rec_) {
            rec_ = std::make_unique<net::Conn>(net::tcp_connect(c_.exchange, c_.rec_port));
            std::uint8_t login[soup::kLoginRequestLen];
            rec_->send(login, soup::put_login_request(login, c_.user, "pass", "", filler_.next_sequence()));
            ++recoveries_;
        }
        if (!rec_) return;
        const bool alive = rec_->read();
        rec_->in().drain([&](char type, const std::uint8_t* p, std::size_t n) {
            if (type == soup::kLoginAccepted) {
                rec_seq_ = soup::parse_login_accepted(p).next_seq;
            } else if (type == soup::kSequencedData) {
                filler_.on_recovered(rec_seq_++, p, n);
            }
            return filler_.recovering();
        });
        if (!filler_.recovering() || !alive) {
            std::uint8_t bye[soup::kHeaderLen];
            rec_->send(bye, soup::put_empty(bye, soup::kLogoutRequest));
            rec_.reset();
        }
    }

    bool done() const { return ended_ && !filler_.recovering(); }

    void report() {
        const auto& g = filler_.stats();
        std::printf("trader: %llu packets (%llu duplicate, %llu malformed), messages: %llu live + %llu buffered + %llu recovered over TCP "
                    "(%llu gaps, %llu recovery sessions)\n",
                    (unsigned long long)g.packets, (unsigned long long)g.duplicate_packets, (unsigned long long)g.malformed,
                    (unsigned long long)g.live_messages, (unsigned long long)g.buffered_messages,
                    (unsigned long long)g.recovered_messages, (unsigned long long)g.gaps, (unsigned long long)recoveries_);
        std::printf("  signals %llu (%llu skipped while catching up), orders %llu, accepted %llu, fills %llu (%llu shares), "
                    "IOC cancels %llu, rejects %llu\n",
                    (unsigned long long)signals_, (unsigned long long)stale_signals_, (unsigned long long)orders_,
                    (unsigned long long)accepted_, (unsigned long long)fills_, (unsigned long long)filled_shares_,
                    (unsigned long long)canceled_, (unsigned long long)rejected_);
        std::printf("  latency on this host:\n");
        t2t_.print("tick-to-trade (rx -> send call)");
        t2t_send_.print("tick-to-trade (rx -> send returned)");
        rtt_.print("order RTT (send -> Accepted)");
        std::printf("book hash %016llx (L3 book rebuilt from ITCH, %s)\n", (unsigned long long)book_hash(*book_),
                    ended_ ? "complete" : "INCOMPLETE");
    }

    Config c_;
    std::unique_ptr<L3Book> book_;
    Strategy strat_;
    itch::Decoder<Strategy> dec_;
    Feed feed_;
    mold::GapFiller<Feed> filler_;
    int md_fd_ = -1;
    std::unique_ptr<net::Conn> oe_, rec_;
    bool fix_ = c_.oe == "fix";
    std::unique_ptr<net::FixConn> fix_conn_;
    std::unique_ptr<fix::Session> fix_session_;
    std::unordered_map<std::uint64_t, std::uint64_t> fix_sent_;   // trigger seq -> send time
    bool oe_logged_in_ = false, ended_ = false;
    std::uint64_t rec_seq_ = 0, rx_ns_ = 0;
    std::uint32_t user_ref_ = 0;
    std::vector<std::uint64_t> sent_ns_;
    std::uint64_t signals_ = 0, stale_signals_ = 0, orders_ = 0, accepted_ = 0, fills_ = 0, filled_shares_ = 0,
                  canceled_ = 0, rejected_ = 0, recoveries_ = 0;
    net::Samples t2t_, t2t_send_, rtt_;
};

}  // namespace

int main(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--exchange") c.exchange = next();
        else if (a == "--md-port") c.md_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--oe-port") c.oe_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--rec-port") c.rec_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--rx") c.rx = next();
        else if (a == "--ifname") c.ifname = next();
        else if (a == "--zc") c.zc = true;
        else if (a == "--native") c.native = true;
        else if (a == "--group") c.group = next();
        else if (a == "--threshold") c.threshold = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--qty") c.qty = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--cpu") c.cpu = std::atoi(next().c_str());
        else if (a == "--user") c.user = next();
        else if (a == "--oe") c.oe = next();
        else if (a == "--fix-port") c.fix_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--timeout") c.timeout_s = std::atof(next().c_str());
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    try {
        auto t = std::make_unique<Trader>(c);
        return t->run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "trader: %s\n", e.what());
        return 1;
    }
}
#endif
