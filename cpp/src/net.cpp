#include "lob/net.hpp"

#include "lob/book.hpp"
#include "lob/engine.hpp"
#include "lob/latency.hpp"
#include "lob/sequencer.hpp"
#include "lob/timing.hpp"
#include "lob/wire.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)

namespace lob {
int run_serve(const ServeConfig&) {
    std::cerr << "serve: network ingestion uses POSIX sockets and is not built on Windows\n";
    return 1;
}
int run_send(const SendConfig&) {
    std::cerr << "send: network ingestion uses POSIX sockets and is not built on Windows\n";
    return 1;
}
}  // namespace lob

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace lob {
namespace {

[[noreturn]] void fail_errno(const std::string& what) { throw std::runtime_error(what + ": " + std::strerror(errno)); }

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    int get() const { return fd_; }

private:
    int fd_ = -1;
};

sockaddr_in make_addr(const std::string& host, int port) {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const std::string literal = host == "localhost" ? "127.0.0.1" : host;
    if (inet_pton(AF_INET, literal.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("not an IPv4 address: " + host);
    }
    return addr;
}

void parse_target(const std::string& target, std::string* host, int* port) {
    const std::size_t colon = target.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == target.size()) {
        throw std::runtime_error("expected host:port, got '" + target + "'");
    }
    *host = target.substr(0, colon);
    *port = std::stoi(target.substr(colon + 1));
    if (*port <= 0 || *port > 65535) {
        throw std::runtime_error("bad port in '" + target + "'");
    }
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fail_errno("fcntl O_NONBLOCK");
    }
}

int bound_port(int fd) {
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        fail_errno("getsockname");
    }
    return ntohs(addr.sin_port);
}

void write_ready_file(const std::string& path, const std::string& text) {
    if (path.empty()) {
        return;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            throw std::runtime_error("cannot write " + tmp);
        }
        out << text << '\n';
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        fail_errno("rename " + tmp);
    }
}

std::string hex16(uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

// ---------------------------------------------------------------- receiving

struct ServeCounters {
    uint64_t line_packets[2] = {0, 0};
    uint64_t bad_packets = 0;
};

// Owns the book and turns in-order events into fingerprint lines. The receive
// loops only decode packets and call push().
class Receiver {
public:
    explicit Receiver(const ServeConfig& config) : config_(config) {
        if (config.checkpoint == 0) {
            throw std::runtime_error("checkpoint must be >= 1");
        }
        if (!config.fingerprints_path.empty()) {
            fp_.open(config.fingerprints_path);
            if (!fp_) {
                throw std::runtime_error("cannot write " + config.fingerprints_path);
            }
        }
        latencies_.reserve(1u << 16);
    }

    wire::Packet packet;  // decode target
    Sequencer sequencer;
    Book book;

    void push() {
        sequencer.push(packet, [this](const Event* events, std::size_t n, uint64_t send_ns) {
            on_events(events, n, send_ns);
        });
    }

    void finish() {
        if (applied_ != last_written_ && applied_ > 0) {
            checkpoint();
        }
    }

    uint64_t applied() const { return applied_; }
    std::vector<double>& latencies() { return latencies_; }

private:
    void on_events(const Event* events, std::size_t n, uint64_t send_ns) {
        for (std::size_t i = 0; i < n; ++i) {
            book.apply(events[i]);
            ++applied_;
            if (applied_ % config_.checkpoint == 0) {
                checkpoint();
            }
        }
        if (send_ns != 0) {
            const uint64_t now = monotonic_ns();
            latencies_.push_back(now > send_ns ? static_cast<double>(now - send_ns) : 0.0);
        }
    }

    void checkpoint() {
        if (config_.verify) {
            std::string error;
            if (!book.consistent(error)) {
                throw std::runtime_error("inconsistent at event " + std::to_string(applied_) + ": " + error);
            }
        }
        if (fp_.is_open()) {
            fp_ << fingerprint_line(applied_, book) << '\n';
        }
        last_written_ = applied_;
    }

    ServeConfig config_;
    std::ofstream fp_;
    uint64_t applied_ = 0;
    uint64_t last_written_ = 0;
    std::vector<double> latencies_;
};

constexpr int kExitGap = 4;
constexpr int kExitIdle = 5;
constexpr int kExitProtocol = 6;
constexpr int kExitClosed = 7;

int serve_udp(const ServeConfig& config, Receiver& rx, ServeCounters& counters) {
    std::vector<Fd> sockets;
    std::vector<int> ports;
    auto open_line = [&](int port) {
        Fd fd(::socket(AF_INET, SOCK_DGRAM, 0));
        if (fd.get() < 0) {
            fail_errno("socket");
        }
        const int one = 1;
        setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        const int rcvbuf = 8 << 20;  // the kernel caps this at net.core.rmem_max
        setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        const sockaddr_in addr = make_addr(config.bind_addr, port);
        if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            fail_errno("bind");
        }
        set_nonblocking(fd.get());
        ports.push_back(bound_port(fd.get()));
        sockets.push_back(std::move(fd));
    };
    open_line(config.port);
    if (config.port_b >= 0) {
        open_line(config.port_b);
    }
    std::string ready = "udp_a=" + std::to_string(ports[0]);
    if (ports.size() > 1) {
        ready += " udp_b=" + std::to_string(ports[1]);
    }
    write_ready_file(config.ready_file, ready);
    std::cerr << "listening " << ready << '\n';

    const uint64_t idle_ns = static_cast<uint64_t>(config.idle_timeout_ms) * 1000000ull;
    uint64_t last_progress = monotonic_ns();
    uint8_t buf[2048];
    while (!rx.sequencer.finished()) {
        if (rx.sequencer.gap()) {
            return kExitGap;
        }
        if (!config.busy_poll) {
            pollfd pfds[2];
            for (std::size_t i = 0; i < sockets.size(); ++i) {
                pfds[i] = {sockets[i].get(), POLLIN, 0};
            }
            if (::poll(pfds, static_cast<nfds_t>(sockets.size()), 50) < 0 && errno != EINTR) {
                fail_errno("poll");
            }
        }
        bool got_valid = false;
        for (std::size_t line = 0; line < sockets.size(); ++line) {
            for (int burst = 0; burst < 64; ++burst) {
                const ssize_t len = ::recv(sockets[line].get(), buf, sizeof(buf), MSG_DONTWAIT);
                if (len < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    fail_errno("recv");
                }
                if (wire::decode_packet(buf, static_cast<std::size_t>(len), &rx.packet) != wire::Status::Ok) {
                    ++counters.bad_packets;
                    continue;
                }
                ++counters.line_packets[line];
                got_valid = true;
                rx.push();
                if (rx.sequencer.finished() || rx.sequencer.gap()) {
                    break;
                }
            }
        }
        const uint64_t now = monotonic_ns();
        if (got_valid) {
            last_progress = now;
        } else if (now - last_progress > idle_ns) {
            return kExitIdle;
        }
    }
    return 0;
}

int serve_tcp(const ServeConfig& config, Receiver& rx, ServeCounters& counters) {
    Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0) {
        fail_errno("socket");
    }
    const int one = 1;
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const sockaddr_in addr = make_addr(config.bind_addr, config.port);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        fail_errno("bind");
    }
    if (::listen(listener.get(), 1) < 0) {
        fail_errno("listen");
    }
    const std::string ready = "tcp=" + std::to_string(bound_port(listener.get()));
    write_ready_file(config.ready_file, ready);
    std::cerr << "listening " << ready << '\n';

    pollfd lp = {listener.get(), POLLIN, 0};
    if (::poll(&lp, 1, config.idle_timeout_ms) <= 0) {
        return kExitIdle;
    }
    Fd conn(::accept(listener.get(), nullptr, nullptr));
    if (conn.get() < 0) {
        fail_errno("accept");
    }
    setsockopt(conn.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    set_nonblocking(conn.get());

    const uint64_t idle_ns = static_cast<uint64_t>(config.idle_timeout_ms) * 1000000ull;
    uint64_t last_progress = monotonic_ns();
    uint8_t stream[65536];
    std::size_t have = 0;
    while (!rx.sequencer.finished()) {
        if (rx.sequencer.gap()) {
            return kExitGap;
        }
        if (!config.busy_poll) {
            pollfd pfd = {conn.get(), POLLIN, 0};
            if (::poll(&pfd, 1, 50) < 0 && errno != EINTR) {
                fail_errno("poll");
            }
        }
        const ssize_t n = ::recv(conn.get(), stream + have, sizeof(stream) - have, MSG_DONTWAIT);
        if (n == 0) {
            return kExitClosed;
        }
        const uint64_t now = monotonic_ns();
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (now - last_progress > idle_ns) {
                    return kExitIdle;
                }
                continue;
            }
            fail_errno("recv");
        }
        last_progress = now;
        have += static_cast<std::size_t>(n);
        std::size_t off = 0;
        while (have - off >= wire::kHeaderSize) {
            wire::Header header;
            if (wire::decode_header(stream + off, have - off, &header) != wire::Status::Ok) {
                ++counters.bad_packets;
                return kExitProtocol;
            }
            const std::size_t total = wire::packet_size(header.count);
            if (have - off < total) {
                break;
            }
            if (wire::decode_packet(stream + off, total, &rx.packet) != wire::Status::Ok) {
                ++counters.bad_packets;
                return kExitProtocol;
            }
            ++counters.line_packets[0];
            rx.push();
            off += total;
            if (rx.sequencer.finished() || rx.sequencer.gap()) {
                break;
            }
        }
        std::memmove(stream, stream + off, have - off);
        have -= off;
    }
    return 0;
}

const char* exit_name(int rc) {
    switch (rc) {
        case 0: return "ok";
        case kExitGap: return "gap";
        case kExitIdle: return "idle_timeout";
        case kExitProtocol: return "protocol_error";
        case kExitClosed: return "closed_before_end";
        default: return "error";
    }
}

// --------------------------------------------------------------------- send

void sleep_ns(uint64_t ns) {
    timespec ts;
    ts.tv_sec = static_cast<time_t>(ns / 1000000000ull);
    ts.tv_nsec = static_cast<long>(ns % 1000000000ull);
    nanosleep(&ts, nullptr);
}

// Holds packet slot t until start + t * ns_per_slot. Sleeps while the wait is
// long and spins for the last 50 microseconds.
class Pacer {
public:
    Pacer(double ns_per_slot) : ns_per_slot_(ns_per_slot), start_(monotonic_ns()) {}
    void wait(uint64_t slot) const {
        if (ns_per_slot_ <= 0) {
            return;
        }
        const uint64_t deadline = start_ + static_cast<uint64_t>(static_cast<double>(slot) * ns_per_slot_);
        for (;;) {
            const uint64_t now = monotonic_ns();
            if (now >= deadline) {
                return;
            }
            if (deadline - now > 100000) {
                sleep_ns(deadline - now - 50000);
            }
        }
    }

private:
    double ns_per_slot_;
    uint64_t start_;
};

Fd connect_socket(int type, const std::string& target) {
    std::string host;
    int port = 0;
    parse_target(target, &host, &port);
    const sockaddr_in addr = make_addr(host, port);
    for (int attempt = 0;; ++attempt) {
        Fd fd(::socket(AF_INET, type, 0));
        if (fd.get() < 0) {
            fail_errno("socket");
        }
        if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        if (type == SOCK_STREAM && errno == ECONNREFUSED && attempt < 100) {
            sleep_ns(50000000ull);  // the server may still be starting
            continue;
        }
        fail_errno("connect " + target);
    }
}

void send_all(int fd, const uint8_t* data, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        const ssize_t r = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail_errno("send");
        }
        sent += static_cast<std::size_t>(r);
    }
}

}  // namespace

int run_serve(const ServeConfig& config) {
    if (config.proto != "udp" && config.proto != "tcp") {
        throw std::runtime_error("proto must be udp or tcp");
    }
    Receiver rx(config);
    ServeCounters counters;
    const int rc = config.proto == "tcp" ? serve_tcp(config, rx, counters) : serve_udp(config, rx, counters);
    if (rc == 0) {
        rx.finish();
    }

    std::vector<double>& lat = rx.latencies();
    std::sort(lat.begin(), lat.end());
    const Sequencer::Stats& st = rx.sequencer.stats();
    const Book::Depth depth = rx.book.depth();
    std::cout << "SERVE proto=" << config.proto << " status=" << exit_name(rc) << " events=" << rx.applied()
              << " end_seq=" << (rx.sequencer.end_known() ? std::to_string(rx.sequencer.end_seq()) : "unknown")
              << " packets_a=" << counters.line_packets[0] << " packets_b=" << counters.line_packets[1]
              << " bad_packets=" << counters.bad_packets << " duplicates=" << st.duplicates
              << " reordered=" << st.reordered << " partial_overlaps=" << st.partial_overlaps
              << " conflicting_end=" << st.conflicting_end << " gap=" << (rx.sequencer.gap() ? 1 : 0);
    if (rx.sequencer.gap()) {
        std::cout << " gap_at=" << rx.sequencer.gap_at();
    }
    std::cout << " busy_poll=" << (config.busy_poll ? 1 : 0) << " timed_packets=" << lat.size();
    if (!lat.empty()) {
        std::cout << " wire_to_book_ns_p50=" << static_cast<uint64_t>(nearest_rank_percentile(lat, 0.50))
                  << " p95=" << static_cast<uint64_t>(nearest_rank_percentile(lat, 0.95))
                  << " p99=" << static_cast<uint64_t>(nearest_rank_percentile(lat, 0.99))
                  << " p999=" << static_cast<uint64_t>(nearest_rank_percentile(lat, 0.999))
                  << " max=" << static_cast<uint64_t>(lat.back());
    }
    std::cout << " trades=" << rx.book.trade_count() << " rejects=" << rx.book.reject_count()
              << " orders=" << rx.book.order_count() << " bid_levels=" << depth.bid_levels
              << " ask_levels=" << depth.ask_levels << " book=" << hex16(rx.book.book_hash())
              << " trade_hash=" << hex16(rx.book.trade_hash()) << '\n';
    if (rc != 0) {
        std::cerr << "serve failed: " << exit_name(rc) << '\n';
    }
    return rc;
}

int run_send(const SendConfig& config) {
    if (config.proto != "udp" && config.proto != "tcp") {
        throw std::runtime_error("proto must be udp or tcp");
    }
    if (config.target_a.empty()) {
        throw std::runtime_error("--target is required");
    }
    if (config.batch < 1 || config.batch > static_cast<int>(wire::kMaxEvents)) {
        throw std::runtime_error("batch must be 1.." + std::to_string(wire::kMaxEvents));
    }
    const bool udp = config.proto == "udp";
    if (!udp && (!config.target_b.empty() || config.drop_a > 0 || config.drop_b > 0 || config.dup > 0 ||
                 config.reorder > 1)) {
        throw std::runtime_error("loss, duplication, reordering, and a second line apply to udp only");
    }

    std::vector<Event> events;
    if (!config.input.empty()) {
        uint64_t seed = 0;
        events = read_any(config.input, &seed);
    } else {
        events = generate(config.seed, config.count);
    }
    const uint64_t total = events.size();
    const std::size_t batch = static_cast<std::size_t>(config.batch);
    const uint64_t packets = (total + batch - 1) / batch;

    std::vector<Fd> lines;
    lines.push_back(connect_socket(udp ? SOCK_DGRAM : SOCK_STREAM, config.target_a));
    if (!config.target_b.empty()) {
        lines.push_back(connect_socket(SOCK_DGRAM, config.target_b));
    }
    if (!udp) {
        const int one = 1;
        setsockopt(lines[0].get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    // Per-line send order and drop plan, fixed by impair_seed.
    SplitMix64 plan(config.impair_seed);
    auto unit = [&plan]() { return static_cast<double>(plan.next() >> 11) / 9007199254740992.0; };
    std::vector<bool> drop_a(packets, false);
    std::vector<bool> drop_b(packets, false);
    for (uint64_t i = 0; i < packets; ++i) {
        const bool a = unit() < config.drop_a;
        bool b = unit() < config.drop_b;
        if (a && b) {
            b = false;  // never lose a packet on both lines
        }
        drop_a[i] = a;
        drop_b[i] = lines.size() > 1 ? b : false;
    }
    auto make_order = [&](uint64_t salt) {
        std::vector<uint64_t> order(packets);
        for (uint64_t i = 0; i < packets; ++i) {
            order[i] = i;
        }
        SplitMix64 rng(config.impair_seed ^ salt);
        const uint64_t window = static_cast<uint64_t>(std::max(config.reorder, 1));
        for (uint64_t base = 0; base < packets; base += window) {
            const uint64_t end = std::min(packets, base + window);
            for (uint64_t i = end - 1; i > base; --i) {
                std::swap(order[i], order[base + rng.next() % (i - base + 1)]);
            }
        }
        return order;
    };
    const std::vector<uint64_t> order_a = make_order(0xA);
    const std::vector<uint64_t> order_b = make_order(0xB);

    uint8_t buf[wire::kMaxPacketSize];
    uint64_t sent[2] = {0, 0};
    uint64_t dropped[2] = {0, 0};
    uint64_t dups = 0;
    uint64_t bytes = 0;
    uint64_t refused = 0;

    auto send_packet = [&](std::size_t line, uint64_t index, uint8_t flags) {
        wire::Header header;
        header.flags = flags;
        const uint64_t first = index * batch;
        header.seq = first;
        header.count = static_cast<uint16_t>(first < total ? std::min<uint64_t>(batch, total - first) : 0);
        const std::size_t n = wire::encode_packet(header, events.data() + std::min(first, total), buf);
        wire::set_send_time(buf, monotonic_ns());
        if (udp) {
            if (::send(lines[line].get(), buf, n, 0) < 0) {
                if (errno != ECONNREFUSED) {
                    fail_errno("send");
                }
                ++refused;  // an earlier datagram drew an ICMP port-unreachable
                return;
            }
        } else {
            send_all(lines[line].get(), buf, n);
        }
        ++sent[line];
        bytes += n;
    };

    const double ns_per_slot = config.rate > 0 ? static_cast<double>(batch) * 1e9 / config.rate : 0.0;
    Pacer pacer(ns_per_slot);
    const uint64_t t_start = monotonic_ns();
    for (uint64_t t = 0; t < packets; ++t) {
        pacer.wait(t);
        const uint64_t ia = order_a[t];
        if (drop_a[ia]) {
            ++dropped[0];
        } else {
            send_packet(0, ia, 0);
            if (config.dup > 0 && unit() < config.dup) {
                send_packet(0, ia, 0);
                ++dups;
            }
        }
        if (lines.size() > 1) {
            const uint64_t ib = order_b[t];
            if (drop_b[ib]) {
                ++dropped[1];
            } else {
                send_packet(1, ib, 0);
                if (config.dup > 0 && unit() < config.dup) {
                    send_packet(1, ib, 0);
                    ++dups;
                }
            }
        }
    }
    // END is repeated on UDP because a lost END would stall the receiver; the
    // simulated loss above never applies to it.
    const int end_copies = udp ? 5 : 1;
    for (int k = 0; k < end_copies; ++k) {
        for (std::size_t line = 0; line < lines.size(); ++line) {
            send_packet(line, packets, wire::kFlagEnd);
        }
        if (udp) {
            sleep_ns(1000000ull);
        }
    }
    const double elapsed = static_cast<double>(monotonic_ns() - t_start) / 1e9;

    std::cout << "SEND proto=" << config.proto << " lines=" << lines.size() << " events=" << total
              << " packets=" << packets << " batch=" << batch << " bytes=" << bytes << " sent_a=" << sent[0]
              << " sent_b=" << sent[1] << " dropped_a=" << dropped[0] << " dropped_b=" << dropped[1]
              << " extra_copies=" << dups << " refused=" << refused << " reorder_window=" << config.reorder
              << " rate=" << config.rate << " elapsed_s=" << std::fixed << std::setprecision(3) << elapsed
              << " events_per_s=" << (elapsed > 0 ? static_cast<double>(total) / elapsed : 0.0) << '\n';
    return 0;
}

}  // namespace lob

#endif
