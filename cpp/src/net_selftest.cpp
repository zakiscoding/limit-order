#include "lob/book.hpp"
#include "lob/engine.hpp"
#include "lob/net.hpp"
#include "lob/sequencer.hpp"
#include "lob/types.hpp"
#include "lob/wire.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace lob {
namespace {

int g_failures = 0;

#define CHECK(cond, what)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::cerr << "FAIL " << what << " (" << #cond << ") line " << __LINE__ << '\n'; \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

wire::Packet make_packet(uint64_t seq, uint16_t count, uint8_t flags = 0, uint64_t send_ns = 0) {
    wire::Packet p;
    p.header.flags = flags;
    p.header.count = count;
    p.header.seq = seq;
    p.header.send_ns = send_ns;
    for (uint16_t i = 0; i < count; ++i) {
        Event e;
        e.type = EventType::Add;
        e.qty = 1;
        e.price = 100;
        e.id = seq + i;
        e.ts = seq + i;
        p.events[i] = e;
    }
    return p;
}

struct Recorder {
    std::vector<uint64_t> ids;
    std::size_t calls = 0;
    void operator()(const Event* events, std::size_t n, uint64_t) {
        ++calls;
        for (std::size_t i = 0; i < n; ++i) {
            ids.push_back(events[i].id);
        }
    }
};

bool in_order(const std::vector<uint64_t>& ids, uint64_t n) {
    if (ids.size() != n) {
        return false;
    }
    for (uint64_t i = 0; i < n; ++i) {
        if (ids[i] != i) {
            return false;
        }
    }
    return true;
}

void test_layout() {
    Event e;
    e.type = EventType::Add;
    e.side = Side::Sell;
    e.tif = Tif::Ioc;
    e.qty = 0x01020304u;
    e.id = 0x1112131415161718ull;
    e.price = -2;
    e.ts = 0x2122232425262728ull;
    uint8_t got[32];
    wire::encode_event(e, got);
    const uint8_t want[32] = {1,    1,    1,    0,    0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16,
                              0x15, 0x14, 0x13, 0x12, 0x11, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21};
    CHECK(std::memcmp(got, want, 32) == 0, "event byte layout");
    Event back;
    wire::decode_event(got, &back);
    CHECK(back.type == e.type && back.side == e.side && back.tif == e.tif && back.qty == e.qty && back.id == e.id &&
              back.price == e.price && back.ts == e.ts,
          "event roundtrip");

    wire::Header h;
    h.flags = wire::kFlagEnd;
    h.count = 1;
    h.seq = 0x0102030405060708ull;
    h.send_ns = 0x1112131415161718ull;
    uint8_t packet[wire::kMaxPacketSize];
    const std::size_t n = wire::encode_packet(h, &e, packet);
    CHECK(n == 56, "one-event packet is 56 bytes");
    const uint8_t head[24] = {'L',  'O',  'B',  'M',  1,    1,    1,    0,    0x08, 0x07, 0x06, 0x05,
                              0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11};
    CHECK(std::memcmp(packet, head, 24) == 0, "header byte layout");
    wire::Packet decoded;
    CHECK(wire::decode_packet(packet, n, &decoded) == wire::Status::Ok, "decode own packet");
    CHECK(decoded.header.seq == h.seq && decoded.header.send_ns == h.send_ns && decoded.header.count == 1 &&
              decoded.header.flags == wire::kFlagEnd,
          "header roundtrip");
    wire::set_send_time(packet, 7);
    CHECK(wire::decode_packet(packet, n, &decoded) == wire::Status::Ok && decoded.header.send_ns == 7, "set_send_time");
}

void test_rejections() {
    uint8_t buf[wire::kMaxPacketSize + 8];
    Event e;
    wire::Header h;
    h.count = 2;
    Event two[2] = {e, e};
    const std::size_t n = wire::encode_packet(h, two, buf);
    wire::Packet out;
    CHECK(wire::decode_packet(buf, n, &out) == wire::Status::Ok, "valid packet decodes");
    CHECK(wire::decode_packet(buf, 23, &out) == wire::Status::Short, "short header");
    CHECK(wire::decode_packet(buf, n - 1, &out) == wire::Status::BadLength, "truncated body");
    CHECK(wire::decode_packet(buf, n + 1, &out) == wire::Status::BadLength, "trailing byte");
    uint8_t bad[wire::kMaxPacketSize];
    std::memcpy(bad, buf, n);
    bad[0] = 'X';
    CHECK(wire::decode_packet(bad, n, &out) == wire::Status::BadMagic, "bad magic");
    std::memcpy(bad, buf, n);
    bad[4] = 2;
    CHECK(wire::decode_packet(bad, n, &out) == wire::Status::BadVersion, "bad version");
    std::memcpy(bad, buf, n);
    bad[5] = 2;
    CHECK(wire::decode_packet(bad, n, &out) == wire::Status::BadFlags, "unknown flag");
    std::memcpy(bad, buf, n);
    bad[6] = 41;
    CHECK(wire::decode_packet(bad, n, &out) == wire::Status::BadCount, "count above the maximum");
    bad[6] = 0xFF;
    bad[7] = 0xFF;
    CHECK(wire::decode_packet(bad, n, &out) == wire::Status::BadCount, "huge count");
}

void test_in_order_and_reverse() {
    Sequencer seq;
    Recorder rec;
    for (uint64_t s = 0; s < 50; s += 5) {
        CHECK(seq.push(make_packet(s, 5), rec) == Sequencer::Result::Delivered, "in-order delivered");
    }
    CHECK(in_order(rec.ids, 50) && rec.calls == 10 && seq.buffered() == 0, "in-order stream");

    Sequencer rev;
    Recorder r2;
    for (int k = 9; k >= 0; --k) {
        rev.push(make_packet(static_cast<uint64_t>(k) * 5, 5), r2);
    }
    CHECK(in_order(r2.ids, 50), "reverse order is restored");
    CHECK(rev.stats().reordered == 9 && rev.buffered() == 0 && !rev.gap(), "reverse order stats");
}

void test_duplicates_and_overlap() {
    Sequencer seq;
    Recorder rec;
    seq.push(make_packet(0, 5), rec);
    CHECK(seq.push(make_packet(0, 5), rec) == Sequencer::Result::Duplicate, "delivered duplicate");
    CHECK(seq.push(make_packet(10, 5), rec) == Sequencer::Result::Buffered, "future packet buffers");
    CHECK(seq.push(make_packet(10, 5), rec) == Sequencer::Result::Duplicate, "buffered duplicate");
    seq.push(make_packet(5, 5), rec);
    CHECK(in_order(rec.ids, 15), "ids after duplicates");
    CHECK(seq.stats().duplicates == 2, "duplicate count");

    Sequencer lap;
    Recorder r2;
    lap.push(make_packet(0, 10), r2);
    lap.push(make_packet(5, 10), r2);  // 5..14: only 10..14 are new
    CHECK(in_order(r2.ids, 15) && lap.stats().partial_overlaps == 1, "overlapping retransmission delivers the new tail once");
}

void test_gap() {
    Sequencer seq;
    Recorder rec;
    for (std::size_t k = 0; k < Sequencer::kWindow; ++k) {
        CHECK(seq.push(make_packet(5 + 5 * k, 5), rec) == Sequencer::Result::Buffered, "fills the window");
    }
    CHECK(!seq.gap(), "no gap while the window has room");
    CHECK(seq.push(make_packet(5 + 5 * Sequencer::kWindow, 5), rec) == Sequencer::Result::Gap, "window overflow is a gap");
    CHECK(seq.gap() && seq.gap_at() == 0 && rec.ids.empty(), "nothing is delivered past a gap");
    seq.push(make_packet(0, 5), rec);
    CHECK(seq.gap(), "gap flag stays set");
}

void test_end() {
    Sequencer a;
    Recorder ra;
    a.push(make_packet(0, 10), ra);
    CHECK(!a.finished(), "not finished without END");
    a.push(make_packet(10, 0, wire::kFlagEnd), ra);
    CHECK(a.finished() && a.end_seq() == 10, "END after the data");

    Sequencer b;
    Recorder rb;
    b.push(make_packet(10, 0, wire::kFlagEnd), rb);
    CHECK(!b.finished(), "END first does not finish");
    b.push(make_packet(0, 10), rb);
    CHECK(b.finished() && in_order(rb.ids, 10), "finishes when the data catches up");
    b.push(make_packet(11, 0, wire::kFlagEnd), rb);
    CHECK(b.stats().conflicting_end == 1 && b.end_seq() == 10, "a conflicting END is counted and ignored");

    Sequencer c;
    Recorder rc;
    c.push(make_packet(5, 5, wire::kFlagEnd), rc);  // END carried by the last data packet
    CHECK(!c.finished(), "END on a future packet waits");
    c.push(make_packet(0, 5), rc);
    CHECK(c.finished() && in_order(rc.ids, 10), "END on the last data packet");

    Sequencer d;
    Recorder rd;
    CHECK(d.push(make_packet(UINT64_MAX - 1, 5), rd) == Sequencer::Result::Invalid, "sequence overflow is rejected");
}

void test_stress() {
    for (uint64_t seed = 1; seed <= 25; ++seed) {
        SplitMix64 rng(seed);
        const uint64_t total = 3000 + rng.next() % 4000;
        std::vector<wire::Packet> stream;
        for (uint64_t s = 0; s < total;) {
            const uint16_t c = static_cast<uint16_t>(std::min<uint64_t>(1 + rng.next() % 40, total - s));
            stream.push_back(make_packet(s, c));
            s += c;
        }
        // Shuffle within windows of 16 packets, then add duplicates and
        // retransmissions cut on different boundaries next to their original.
        std::vector<wire::Packet> arrivals;
        for (std::size_t base = 0; base < stream.size(); base += 16) {
            const std::size_t end = std::min(stream.size(), base + 16);
            for (std::size_t i = end - 1; i > base; --i) {
                std::swap(stream[i], stream[base + rng.next() % (i - base + 1)]);
            }
            for (std::size_t i = base; i < end; ++i) {
                arrivals.push_back(stream[i]);
                if (rng.next() % 5 == 0) {
                    arrivals.push_back(stream[i]);
                }
                if (rng.next() % 20 == 0) {
                    const uint64_t start = stream[i].header.seq + rng.next() % stream[i].header.count;
                    const uint16_t c = static_cast<uint16_t>(std::min<uint64_t>(1 + rng.next() % 40, total - start));
                    arrivals.push_back(make_packet(start, c));
                }
            }
        }
        arrivals.push_back(make_packet(total, 0, wire::kFlagEnd));
        Sequencer seq;
        Recorder rec;
        for (const wire::Packet& p : arrivals) {
            seq.push(p, rec);
        }
        CHECK(in_order(rec.ids, total), "stress: every event exactly once, in order");
        CHECK(seq.finished() && !seq.gap() && seq.buffered() == 0, "stress: finished with nothing buffered");
    }
}

void test_decoder_fuzz() {
    SplitMix64 rng(0xF00D);
    uint8_t buf[2048];
    wire::Packet out;
    uint64_t ok = 0;
    for (int iter = 0; iter < 300000; ++iter) {
        std::size_t len = static_cast<std::size_t>(rng.next() % 1400);
        for (std::size_t i = 0; i < len; ++i) {
            buf[i] = static_cast<uint8_t>(rng.next());
        }
        if (len >= wire::kHeaderSize && rng.next() % 2 == 0) {
            std::memcpy(buf, "LOBM", 4);
            buf[4] = 1;
            buf[5] &= 1;
            if (rng.next() % 2 == 0) {
                const uint16_t count = static_cast<uint16_t>(rng.next() % 41);
                buf[6] = static_cast<uint8_t>(count);
                buf[7] = 0;
                len = wire::packet_size(count);
                for (std::size_t i = wire::kHeaderSize; i < len; ++i) {
                    buf[i] = static_cast<uint8_t>(rng.next());
                }
            }
        }
        const wire::Status s = wire::decode_packet(buf, len, &out);
        if (s == wire::Status::Ok) {
            ++ok;
            if (out.header.count > wire::kMaxEvents || len != wire::packet_size(out.header.count)) {
                CHECK(false, "fuzz: Ok result violates its own invariants");
                return;
            }
        }
    }
    // Mutate a valid packet one bit at a time.
    uint8_t good[wire::kMaxPacketSize];
    wire::Packet source = make_packet(100, 40);
    const std::size_t n = wire::encode_packet(source.header, source.events, good);
    for (std::size_t bit = 0; bit < n * 8; ++bit) {
        uint8_t copy[wire::kMaxPacketSize];
        std::memcpy(copy, good, n);
        copy[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        wire::decode_packet(copy, n, &out);
    }
    CHECK(ok > 1000, "fuzz: the Ok path was exercised");
}

void test_wire_preserves_book_result() {
    const std::vector<Event> events = generate(42, 20000);
    Book direct;
    Book via_wire;
    uint8_t buf[wire::kEventSize];
    for (const Event& e : events) {
        direct.apply(e);
        wire::encode_event(e, buf);
        Event copy;
        wire::decode_event(buf, &copy);
        via_wire.apply(copy);
    }
    CHECK(direct.book_hash() == via_wire.book_hash() && direct.trade_hash() == via_wire.trade_hash() &&
              direct.trade_count() == via_wire.trade_count() && direct.reject_count() == via_wire.reject_count(),
          "events that cross the wire give the same book and trades");
}

}  // namespace

int net_selftest() {
    g_failures = 0;
    test_layout();
    test_rejections();
    test_in_order_and_reverse();
    test_duplicates_and_overlap();
    test_gap();
    test_end();
    test_stress();
    test_decoder_fuzz();
    test_wire_preserves_book_result();
    if (g_failures == 0) {
        std::cout << "net selftest OK\n";
        return 0;
    }
    std::cerr << g_failures << " net selftest check(s) failed\n";
    return 1;
}

}  // namespace lob
