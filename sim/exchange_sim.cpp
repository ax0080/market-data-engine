// Exchange simulator (Linux): the orderbook-engine matching engine behind
// NASDAQ-style interfaces.
//
//   market data   ITCH 5.0 in MoldUDP64 packets over UDP (unicast or multicast)
//   recovery      SoupBinTCP replay of the same ITCH stream from any sequence number
//   order entry   OUCH 5.0 over SoupBinTCP, one account per login username
//                 FIX 4.4 over TCP (sim/fix_order_entry.h), one account per SenderCompID
//
// Simulated market flow ("house" orders) keeps the books moving: passive limit
// orders, cancels, and marketable IOC orders around a drifting mid price.
// Every --trigger-us a large passive bid is added; the trader's rule reacts to
// it, and the order it sends carries the trigger's ITCH sequence number in its
// ClOrdID. The simulator stamps the trigger packet just before sendto() and the
// order just after recv(), both on this host's clock, so it measures the full
// exchange -> trader -> exchange reaction time without clock sync.
//
// --drop N withholds every Nth market-data packet (it stays in the replay log),
// forcing the trader through gap recovery over TCP.
//
// usage: exchange_sim [--md-dest 127.0.0.1] [--md-port 31007] [--oe-port 31100]
//                     [--rec-port 31101] [--fix-port 31102] [--rate 100000] [--trigger-us 1000]
//                     [--duration 10] [--drop 0] [--flush-us 50] [--cpu -1]
//                     [--symbols AAPL,MSFT,...] [--no-wait]

#if !defined(__linux__)
#include <cstdio>
int main() {
    std::puts("exchange_sim needs Linux");
    return 0;
}
#else

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "fix_order_entry.h"
#include "gateway.h"
#include "mde/book_hash.h"
#include "mde/fix44.h"
#include "mde/itch50.h"
#include "mde/itch50_writer.h"
#include "mde/l3_book.h"
#include "mde/linux_net.h"
#include "mde/ouch50.h"
#include "mde/soupbintcp.h"

using namespace mde;

namespace {

constexpr const char* kSession = "MDESIM0001";
constexpr std::uint64_t kSecond = 1'000'000'000;

struct Config {
    std::string md_dest = "127.0.0.1";
    std::uint16_t md_port = 31007, oe_port = 31100, rec_port = 31101, fix_port = 31102;
    double rate = 100'000;          // house events per second, all symbols
    std::uint64_t trigger_us = 1000;
    std::uint32_t trigger_qty = 5000;
    double duration = 10;
    std::uint64_t drop = 0;
    std::uint64_t flush_us = 50;
    int cpu = -1;
    bool wait_client = true;
    std::vector<std::string> symbols = {"AAPL", "MSFT", "AMZN", "NVDA", "GOOG", "META", "TSLA", "AVGO"};
};

// Sequenced message store: the ITCH replay log and each OUCH account's output.
struct MsgLog {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint64_t> off{0};
    std::uint64_t count() const { return off.size() - 1; }
    void append(const std::uint8_t* m, std::size_t n) {
        bytes.insert(bytes.end(), m, m + n);
        off.push_back(bytes.size());
    }
    const std::uint8_t* at(std::uint64_t i) const { return bytes.data() + off[i]; }   // 0-based
    std::size_t len(std::uint64_t i) const { return off[i + 1] - off[i]; }
};

struct Client {
    enum Kind { OrderEntry, Recovery } kind;
    net::Conn conn;
    bool logged_in = false;
    int account = -1;           // OrderEntry: gateway session
    std::uint64_t pos = 0;      // next log index to send
};

struct FixClient {
    net::FixConn conn;
    int account = -1;           // set by the first Logon
};

class Exchange {
public:
    explicit Exchange(const Config& c)
        : c_(c), pb_(kSession), shadow_(std::make_unique<L3Book>(std::size_t{1} << 20)), shadow_dec_(*shadow_), gw_(*this) {
        md_fd_ = net::udp_socket();
        md_dst_ = net::addr(c.md_dest, c.md_port);
        const std::uint32_t first = ntohl(md_dst_.sin_addr.s_addr) >> 24;
        if (first >= 224 && first <= 239) {
            unsigned char loop = 1;
            setsockopt(md_fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
        }
        oe_listen_ = net::tcp_listen(c.oe_port);
        rec_listen_ = net::tcp_listen(c.rec_port);
        fix_listen_ = net::tcp_listen(c.fix_port);
    }

    // ---- Gateway output ----
    void md(const std::uint8_t* m, std::size_t len) {
        log_.append(m, len);
        shadow_dec_.decode(m, len);
        if (!pb_.fits(len)) flush_md();
        if (pb_.empty()) pkt_start_ns_ = net::mono_ns();
        pb_.append(m, len);
    }
    void oe(int s, const std::uint8_t* m, std::size_t len) {
        Account& acct = accounts_[static_cast<std::size_t>(s)];
        if (acct.fix) {   // FIX account: the OUCH reply becomes an ExecutionReport
            acct.fix->on_ouch(m, len, net::epoch_ns(), fix_sender(s));
            return;
        }
        acct.log.append(m, len);
        for (auto& cl : clients_)
            if (cl->kind == Client::OrderEntry && cl->account == s) pump(*cl);
    }
    std::uint64_t now_ns() { return net::ns_since_midnight(); }

    int run() {
        std::printf("exchange_sim: ITCH/MoldUDP64 -> %s:%u, OUCH/SoupBinTCP :%u, FIX 4.4 :%u, recovery :%u, %zu symbols\n",
                    c_.md_dest.c_str(), c_.md_port, c_.oe_port, c_.fix_port, c_.rec_port, c_.symbols.size());
        std::fflush(stdout);
        net::pin_cpu(c_.cpu);
        if (c_.wait_client) {
            const std::uint64_t give_up = net::mono_ns() + 60 * kSecond;
            while (logged_in_oe() == 0) {
                if (net::mono_ns() > give_up) {
                    std::fprintf(stderr, "exchange_sim: no client logged in within 60 s\n");
                    return 1;
                }
                service(net::mono_ns());
            }
        }
        for (const auto& s : c_.symbols) {
            books_.emplace_back();
            books_.back().locate = gw_.add_symbol(s);
        }
        gw_.system_event(itch::kStartOfMessages);
        flush_md();

        const std::uint64_t t0 = net::mono_ns();
        const std::uint64_t end = t0 + std::uint64_t(c_.duration * 1e9);
        const std::uint64_t period = c_.rate > 0 ? std::uint64_t(1e9 / c_.rate) : end;
        std::uint64_t next_event = t0, next_trigger = t0 + c_.trigger_us * 1000;
        std::mt19937_64 rng(42);
        std::uint64_t now = t0;
        while ((now = net::mono_ns()) < end) {
            for (int k = 0; k < 64 && now >= next_event; ++k, next_event += period) house_event(rng);
            if (now > next_event + 10'000'000) next_event = now;   // fell behind by 10 ms: skip ahead
            if (now >= next_trigger) {
                trigger(rng);
                next_trigger += c_.trigger_us * 1000;
            }
            service(now);
        }
        gw_.system_event(itch::kEndOfMessages);
        flush_md();
        // End of Session carries the final sequence number, so a trader that lost
        // the last packets still sees the gap. Keep serving recovery and order entry
        // until every client has disconnected (up to 60 s): a slow trader (e.g. under
        // sanitizers) may still be filling gaps when the feed ends.
        std::uint8_t ctl[mold::kHeader];
        for (int i = 0; i < 3; ++i) {
            sendto(md_fd_, ctl, pb_.control(ctl, log_.count() + 1, mold::kEndOfSession), 0,
                   reinterpret_cast<const sockaddr*>(&md_dst_), sizeof md_dst_);
        }
        const std::uint64_t linger_end = net::mono_ns() + 60 * kSecond;
        while (net::mono_ns() < linger_end && (!clients_.empty() || !fix_clients_.empty())) service(net::mono_ns());
        report(double(now - t0) / 1e9);
        return 0;
    }

private:
    struct Book {
        std::uint16_t locate = 0;
        std::uint32_t mid = 1'000'000;   // $100.0000
        std::vector<exchange::OrderId> live;
        exchange::OrderId trigger = 0;
        std::uint64_t events = 0;
    };
    struct Account {
        std::string user;
        MsgLog log;                                   // OUCH: sequenced output for SoupBinTCP
        std::unique_ptr<sim::FixOrderEntry> fix;      // FIX: order state and ExecutionReports
        std::unique_ptr<fix::Session> session;        // FIX: sequence numbers and resend
    };

    static constexpr std::uint32_t kTick = 100;   // $0.01

    // ---- simulated market ----
    void house_event(std::mt19937_64& rng) {
        Book& b = books_[rng() % books_.size()];
        if (++b.events % 2000 == 0) b.mid += (rng() % 2 ? kTick : -kTick);
        const unsigned r = rng() % 100;
        if (r < 35 && !b.live.empty()) {   // cancel
            const std::size_t k = rng() % b.live.size();
            gw_.house_cancel(b.live[k]);
            b.live[k] = b.live.back();
            b.live.pop_back();
            return;
        }
        if (r < 45) {   // marketable IOC through a few levels
            const bool buy = rng() % 2;
            gw_.house_limit(b.locate, buy ? 'B' : 'S', buy ? b.mid + 5 * kTick : b.mid - 5 * kTick,
                            100 * static_cast<std::uint32_t>(1 + rng() % 5), true);
            return;
        }
        if (b.live.size() > 2000) return;
        const bool buy = rng() % 2;
        const std::uint32_t away = kTick * static_cast<std::uint32_t>(1 + rng() % 10);
        const auto id = gw_.house_limit(b.locate, buy ? 'B' : 'S', buy ? b.mid - away : b.mid + away,
                                        100 * static_cast<std::uint32_t>(1 + rng() % 10));
        if (id && gw_.book(b.locate)->find(id)) b.live.push_back(id);
    }

    // A large passive bid at the best bid: the trader's signal.
    void trigger(std::mt19937_64& rng) {
        Book& b = books_[rng() % books_.size()];
        if (b.trigger) gw_.house_cancel(b.trigger);
        const auto bid = gw_.book(b.locate)->best_bid();
        const std::uint32_t px = bid ? static_cast<std::uint32_t>(bid->raw) : b.mid - kTick;
        flush_md();   // the trigger goes out in its own packet, stamped precisely
        const std::uint64_t seq = log_.count() + 1;
        b.trigger = gw_.house_limit(b.locate, 'B', px, c_.trigger_qty);
        if (!pb_.empty()) {
            trigger_seq_ = seq;
            flush_md();
        }
        ++triggers_;
    }

    // ---- I/O ----
    void flush_md() {
        if (pb_.empty()) return;
        const std::uint8_t* p = pb_.data();
        const std::size_t n = pb_.size();
        const std::uint64_t next = pb_.first_sequence() + pb_.count();
        ++md_packets_;
        if (c_.drop && md_packets_ % c_.drop == 0) {
            ++md_dropped_;
        } else {
            const std::uint64_t t = net::mono_ns();
            if (trigger_seq_) trigger_sent_[trigger_seq_] = t;
            sendto(md_fd_, p, n, 0, reinterpret_cast<const sockaddr*>(&md_dst_), sizeof md_dst_);
        }
        trigger_seq_ = 0;
        last_md_ns_ = net::mono_ns();
        pb_.reset(next);
        for (auto& cl : clients_)
            if (cl->kind == Client::Recovery) pump(*cl);
    }

    void service(std::uint64_t now) {
        accept_new(oe_listen_, Client::OrderEntry);
        accept_new(rec_listen_, Client::Recovery);
        if (const int fd = accept(fix_listen_, nullptr, nullptr); fd >= 0) {
            net::tune_tcp(fd);
            fix_clients_.push_back(std::make_unique<FixClient>(FixClient{net::FixConn(fd)}));
        }
        for (std::size_t i = 0; i < fix_clients_.size(); ++i) {
            FixClient& cl = *fix_clients_[i];
            const bool alive = cl.conn.read();
            const std::uint64_t t_rx = cl.conn.last_rx_ns;
            cl.conn.in().drain([&](const std::uint8_t* p, std::size_t n) {
                fix_message(cl, p, n, t_rx);
                return cl.conn.open();
            });
            if (!alive || !cl.conn.open() || !cl.conn.flush()) {
                fix_clients_.erase(fix_clients_.begin() + static_cast<std::ptrdiff_t>(i--));
                continue;
            }
            if (cl.account >= 0) {
                fix::Session& ss = *accounts_[static_cast<std::size_t>(cl.account)].session;
                ss.tick(net::epoch_ns(), net::epoch_ns() - (now - cl.conn.last_tx_ns),
                        [&](const std::uint8_t* p, std::size_t n) { cl.conn.send(p, n); });
            }
        }
        for (std::size_t i = 0; i < clients_.size(); ++i) {
            Client& cl = *clients_[i];
            const bool alive = cl.conn.read();
            handle_packets(cl);
            if (!alive || !cl.conn.open() || !cl.conn.flush()) {
                clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i--));
                continue;
            }
            pump(cl);
            if (cl.logged_in && now > cl.conn.last_tx_ns + kSecond) {
                std::uint8_t hb[soup::kHeaderLen];
                cl.conn.send(hb, soup::put_empty(hb, soup::kServerHeartbeat));
            }
        }
        if (!pb_.empty() && now > pkt_start_ns_ + c_.flush_us * 1000) flush_md();
        if (pb_.empty() && now > last_md_ns_ + kSecond && log_.count() > 0) {   // MoldUDP64 heartbeat
            std::uint8_t hb[mold::kHeader];
            sendto(md_fd_, hb, pb_.control(hb, log_.count() + 1, mold::kHeartbeat), 0,
                   reinterpret_cast<const sockaddr*>(&md_dst_), sizeof md_dst_);
            last_md_ns_ = now;
        }
    }

    void accept_new(int listen_fd, Client::Kind kind) {
        const int fd = accept(listen_fd, nullptr, nullptr);
        if (fd < 0) return;
        net::tune_tcp(fd);
        clients_.push_back(std::make_unique<Client>(Client{kind, net::Conn(fd)}));
    }

    void handle_packets(Client& cl) {
        const std::uint64_t t_rx = cl.conn.last_rx_ns;
        cl.conn.in().drain([&](char type, const std::uint8_t* p, std::size_t n) {
            if (type == soup::kLoginRequest && n >= soup::kLoginRequestLen - soup::kHeaderLen && !cl.logged_in) {
                login(cl, soup::parse_login_request(p));
            } else if (type == soup::kUnsequencedData && cl.logged_in && cl.kind == Client::OrderEntry) {
                inbound_ = &cl;
                rx_ns_ = t_rx;
                ouch::decode_inbound(p, n, *this);
            } else if (type == soup::kLogoutRequest) {
                cl.conn.close();
                return false;
            }
            return true;
        });
    }

    void login(Client& cl, const soup::LoginRequest& r) {
        std::uint8_t out[soup::kLoginAcceptedLen];
        if (!r.session.empty() && r.session != kSession) {
            cl.conn.send(out, soup::put_login_rejected(out, soup::kRejectSessionUnavailable));
            cl.conn.close();
            return;
        }
        const MsgLog* src = &log_;
        if (cl.kind == Client::OrderEntry) {
            int a = -1;
            for (std::size_t i = 0; i < accounts_.size(); ++i)
                if (accounts_[i].user == r.user && !accounts_[i].fix) a = static_cast<int>(i);
            if (a < 0) {
                accounts_.push_back(Account{std::string(r.user), {}, nullptr, nullptr});
                a = static_cast<int>(accounts_.size() - 1);
            }
            cl.account = a;
            src = &accounts_[static_cast<std::size_t>(a)].log;
        }
        // Requested sequence 0 means "from the newest message on".
        const std::uint64_t next = r.next_seq == 0 ? src->count() + 1 : std::min(r.next_seq, src->count() + 1);
        cl.pos = next - 1;
        cl.logged_in = true;
        cl.conn.send(out, soup::put_login_accepted(out, kSession, next));
        pump(cl);
    }

    // Sends sequenced messages the client has not seen, keeping the socket
    // queue bounded so a slow replay cannot stall the exchange.
    void pump(Client& cl) {
        if (!cl.logged_in || !cl.conn.open()) return;
        const MsgLog& src = cl.kind == Client::Recovery ? log_ : accounts_[static_cast<std::size_t>(cl.account)].log;
        std::uint8_t buf[8192];
        std::size_t used = 0;
        while (cl.pos < src.count() && cl.conn.queued() < (1u << 20)) {
            const std::size_t len = src.len(cl.pos);
            if (used + soup::kHeaderLen + len > sizeof buf) {
                cl.conn.send(buf, used);
                used = 0;
            }
            std::memcpy(buf + used + soup::kHeaderLen, src.at(cl.pos), len);
            used += soup::seal_data(buf + used, soup::kSequencedData, len);
            ++cl.pos;
            if (cl.kind == Client::Recovery) ++replayed_;
        }
        if (used) cl.conn.send(buf, used);
    }

    std::size_t logged_in_oe() const {
        std::size_t n = 0;
        for (const auto& cl : clients_) n += cl->kind == Client::OrderEntry && cl->logged_in;
        for (const auto& cl : fix_clients_)
            n += cl->account >= 0 && accounts_[static_cast<std::size_t>(cl->account)].session->logged_on();
        return n;
    }

    // ---- FIX ----
    // Replies for FIX account a: built by its session (sequence number, kept for
    // resend) and sent to its connection, if one is up.
    struct FixSender {
        Exchange* ex;
        int a;
        template <class Fill>
        void operator()(char type, Fill&& fill) const {
            Account& acct = ex->accounts_[static_cast<std::size_t>(a)];
            const auto m = acct.session->build(type, net::epoch_ns(), fill);
            for (auto& cl : ex->fix_clients_)
                if (cl->account == a && m.first) cl->conn.send(m.first, m.second);
        }
    };
    FixSender fix_sender(int a) { return FixSender{this, a}; }

    struct FixApp {
        Exchange& ex;
        int account;
        std::uint64_t t_rx;
        void on_app(const fix::Message& m) { ex.fix_request(account, m, t_rx); }
    };

    void fix_message(FixClient& cl, const std::uint8_t* p, std::size_t n, std::uint64_t t_rx) {
        if (cl.account < 0) {   // first message must be a Logon; SenderCompID names the account
            fix::Message m;
            if (!m.parse(p, n) || m.type() != 'A') {
                cl.conn.close();
                return;
            }
            const std::string comp(m.get(fix::tag::SenderCompID));
            int a = -1;
            for (std::size_t i = 0; i < accounts_.size(); ++i)
                if (accounts_[i].fix && accounts_[i].user == comp) a = static_cast<int>(i);
            if (a < 0) {
                accounts_.push_back(Account{comp, {}, nullptr, nullptr});
                a = static_cast<int>(accounts_.size() - 1);
                accounts_.back().fix = std::make_unique<sim::FixOrderEntry>(a);
                accounts_.back().session = std::make_unique<fix::Session>("MDESIM", comp, true);
            }
            cl.account = a;
        }
        FixApp app{*this, cl.account, t_rx};
        accounts_[static_cast<std::size_t>(cl.account)].session->on_message(
            p, n, net::epoch_ns(), app, [&](const std::uint8_t* q, std::size_t k) { cl.conn.send(q, k); });
    }

    void fix_request(int account, const fix::Message& m, std::uint64_t t_rx) {
        if (m.type() == 'D') {
            const std::string_view id = m.get(fix::tag::ClOrdID);
            if (id.size() > 1 && id[0] == 'T') {   // reaction to a trigger
                const std::uint64_t seq = fix::parse_uint(id.substr(1));
                const auto it = trigger_sent_.find(seq);
                if (it != trigger_sent_.end() && t_rx >= it->second) reaction_.add(t_rx - it->second);
            }
            ++orders_in_;
            ++fix_orders_in_;
        }
        accounts_[static_cast<std::size_t>(account)].fix->on_request(m, gw_, net::epoch_ns(), fix_sender(account));
    }

public:
    // ---- OUCH inbound (ouch::decode_inbound handler) ----
    void on_enter(const ouch::EnterOrder& o) {
        if (o.clordid.size() > 1 && o.clordid[0] == 'T') {   // reaction to a trigger
            const std::uint64_t seq = std::strtoull(std::string(o.clordid.substr(1)).c_str(), nullptr, 10);
            const auto it = trigger_sent_.find(seq);
            if (it != trigger_sent_.end() && rx_ns_ >= it->second) reaction_.add(rx_ns_ - it->second);
        }
        ++orders_in_;
        gw_.enter(inbound_->account, o);
    }
    void on_replace(const ouch::ReplaceOrder& o) { gw_.replace(inbound_->account, o); }
    void on_cancel(const ouch::CancelOrder& o) { gw_.cancel(inbound_->account, o); }

private:
    void report(double secs) {
        const auto& g = gw_.stats();
        std::printf("exchange_sim: %.1f s, %llu ITCH messages in %llu packets (%llu withheld by --drop), %llu replayed over TCP\n",
                    secs, (unsigned long long)log_.count(), (unsigned long long)md_packets_,
                    (unsigned long long)md_dropped_, (unsigned long long)replayed_);
        std::printf("  orders: %llu in (%llu OUCH, %llu FIX), %llu accepted (incl. house), %llu rejected, %llu executions, %llu triggers sent\n",
                    (unsigned long long)orders_in_, (unsigned long long)(orders_in_ - fix_orders_in_),
                    (unsigned long long)fix_orders_in_, (unsigned long long)g.accepted, (unsigned long long)g.rejected,
                    (unsigned long long)g.executions, (unsigned long long)triggers_);
        std::printf("  exchange-observed reaction (trigger packet sendto -> order recv, both on this host's clock):\n");
        reaction_.print("trigger -> order at exchange");
        std::printf("book hash %016llx (L3 book rebuilt from the published ITCH)\n", (unsigned long long)book_hash(*shadow_));
    }

    Config c_;
    int md_fd_ = -1, oe_listen_ = -1, rec_listen_ = -1, fix_listen_ = -1;
    sockaddr_in md_dst_{};
    mold::PacketBuilder pb_;
    MsgLog log_;
    std::unique_ptr<L3Book> shadow_;
    itch::Decoder<L3Book> shadow_dec_;
    sim::Gateway<Exchange> gw_;
    std::vector<Book> books_;
    std::vector<Account> accounts_;
    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<std::unique_ptr<FixClient>> fix_clients_;
    Client* inbound_ = nullptr;
    std::uint64_t rx_ns_ = 0;
    std::uint64_t pkt_start_ns_ = 0, last_md_ns_ = 0;
    std::uint64_t trigger_seq_ = 0, triggers_ = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> trigger_sent_;   // ITCH sequence -> send time
    std::uint64_t md_packets_ = 0, md_dropped_ = 0, replayed_ = 0, orders_in_ = 0, fix_orders_in_ = 0;
    net::Samples reaction_;
};

}  // namespace

int main(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--md-dest") c.md_dest = next();
        else if (a == "--md-port") c.md_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--oe-port") c.oe_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--rec-port") c.rec_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--fix-port") c.fix_port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        else if (a == "--rate") c.rate = std::atof(next().c_str());
        else if (a == "--trigger-us") c.trigger_us = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--trigger-qty") c.trigger_qty = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--duration") c.duration = std::atof(next().c_str());
        else if (a == "--drop") c.drop = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--flush-us") c.flush_us = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--cpu") c.cpu = std::atoi(next().c_str());
        else if (a == "--no-wait") c.wait_client = false;
        else if (a == "--symbols") {
            c.symbols.clear();
            std::string s = next(), cur;
            for (char ch : s + ",") {
                if (ch == ',') {
                    if (!cur.empty()) c.symbols.push_back(cur);
                    cur.clear();
                } else {
                    cur += ch;
                }
            }
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    try {
        auto ex = std::make_unique<Exchange>(c);
        return ex->run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "exchange_sim: %s\n", e.what());
        return 1;
    }
}
#endif
