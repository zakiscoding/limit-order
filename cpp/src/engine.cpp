#include "lob/engine.hpp"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace lob {
namespace {

constexpr char kMagic[8] = {'L', 'O', 'B', 'R', 'E', 'P', 'V', '1'};

int64_t clamp_i64(int64_t value, int64_t lo, int64_t hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

void write_pod(std::ostream& out, const void* data, std::streamsize size) {
    out.write(static_cast<const char*>(data), size);
    if (!out) {
        throw std::runtime_error("short write");
    }
}

void write_u32(std::ostream& out, uint32_t value) {
    unsigned char bytes[4];
    for (int i = 0; i < 4; ++i) {
        bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);
    }
    write_pod(out, bytes, 4);
}

void write_u64(std::ostream& out, uint64_t value) {
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);
    }
    write_pod(out, bytes, 8);
}

void read_exact(std::istream& in, void* data, std::streamsize size, const std::string& path) {
    in.read(static_cast<char*>(data), size);
    if (in.gcount() != size) {
        throw std::runtime_error(path + ": truncated read");
    }
}

uint32_t read_u32(std::istream& in, const std::string& path) {
    unsigned char bytes[4];
    read_exact(in, bytes, 4, path);
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read_u64(std::istream& in, const std::string& path) {
    unsigned char bytes[8];
    read_exact(in, bytes, 8, path);
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

uint64_t parse_u64(const std::string& token, const std::string& what) {
    if (token.empty()) {
        throw std::runtime_error("missing " + what);
    }
    size_t used = 0;
    unsigned long long value = std::stoull(token, &used, 10);
    if (used != token.size()) {
        throw std::runtime_error("bad " + what + " " + token);
    }
    return static_cast<uint64_t>(value);
}

int64_t parse_i64(const std::string& token, const std::string& what) {
    if (token.empty()) {
        throw std::runtime_error("missing " + what);
    }
    size_t used = 0;
    long long value = std::stoll(token, &used, 10);
    if (used != token.size()) {
        throw std::runtime_error("bad " + what + " " + token);
    }
    return static_cast<int64_t>(value);
}

Side parse_side(const std::string& token) {
    if (token == "B" || token == "BUY") {
        return Side::Buy;
    }
    if (token == "S" || token == "SELL") {
        return Side::Sell;
    }
    throw std::runtime_error("bad side " + token);
}

Tif parse_tif(const std::string& token) {
    if (token == "GTC") {
        return Tif::Gtc;
    }
    if (token == "IOC") {
        return Tif::Ioc;
    }
    throw std::runtime_error("bad tif " + token);
}

std::vector<std::string> split_ws(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> parts;
    std::string token;
    while (in >> token) {
        parts.push_back(token);
    }
    return parts;
}

Event parse_event_line(const std::string& line) {
    const std::vector<std::string> parts = split_ws(line);
    if (parts.empty()) {
        throw std::runtime_error("empty event");
    }
    const std::string& op = parts[0];
    Event event;
    if (op == "RESET") {
        if (parts.size() != 1) {
            throw std::runtime_error(line);
        }
        event.type = EventType::Reset;
        return event;
    }
    if (op == "ADD") {
        if (parts.size() != 7) {
            throw std::runtime_error(line);
        }
        uint64_t qty = parse_u64(parts[4], "qty");
        if (qty > 0xffffffffull) {
            throw std::runtime_error("qty overflow");
        }
        event.type = EventType::Add;
        event.id = parse_u64(parts[1], "id");
        event.side = parse_side(parts[2]);
        event.price = parse_i64(parts[3], "price");
        event.qty = static_cast<uint32_t>(qty);
        event.tif = parse_tif(parts[5]);
        event.ts = parse_u64(parts[6], "ts");
        return event;
    }
    if (op == "CANCEL") {
        if (parts.size() != 3) {
            throw std::runtime_error(line);
        }
        event.type = EventType::Cancel;
        event.id = parse_u64(parts[1], "id");
        event.ts = parse_u64(parts[2], "ts");
        return event;
    }
    if (op == "MODIFY") {
        if (parts.size() != 5) {
            throw std::runtime_error(line);
        }
        uint64_t qty = parse_u64(parts[3], "qty");
        if (qty > 0xffffffffull) {
            throw std::runtime_error("qty overflow");
        }
        event.type = EventType::Modify;
        event.id = parse_u64(parts[1], "id");
        event.price = parse_i64(parts[2], "price");
        event.qty = static_cast<uint32_t>(qty);
        event.ts = parse_u64(parts[4], "ts");
        return event;
    }
    throw std::runtime_error("bad event " + line);
}

struct ScenarioBlock {
    std::string name;
    std::vector<Event> events;
};

std::vector<ScenarioBlock> load_blocks(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    std::vector<ScenarioBlock> blocks;
    ScenarioBlock current;
    std::string raw;
    int lineno = 0;
    auto flush = [&]() {
        if (!current.name.empty() || !current.events.empty()) {
            blocks.push_back(std::move(current));
        }
        current = ScenarioBlock{};
    };
    while (std::getline(in, raw)) {
        ++lineno;
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
            trimmed.pop_back();
        }
        std::size_t start = 0;
        while (start < trimmed.size() && (trimmed[start] == ' ' || trimmed[start] == '\t')) {
            ++start;
        }
        trimmed = trimmed.substr(start);
        if (trimmed.empty()) {
            continue;
        }
        try {
            if (trimmed.rfind("# name:", 0) == 0) {
                current.name = trimmed.substr(7);
                std::size_t pos = 0;
                while (pos < current.name.size() && current.name[pos] == ' ') {
                    ++pos;
                }
                current.name = current.name.substr(pos);
                continue;
            }
            if (trimmed[0] == '#') {
                continue;
            }
            if (trimmed == "RESET") {
                flush();
                continue;
            }
            current.events.push_back(parse_event_line(trimmed));
        } catch (const std::exception& ex) {
            throw std::runtime_error(path + ":" + std::to_string(lineno) + ": " + ex.what());
        }
    }
    flush();
    return blocks;
}

struct ExpectOrder {
    uint64_t id = 0;
    int64_t price = 0;
    uint32_t qty = 0;
};

struct ExpectTrade {
    uint64_t taker = 0;
    uint64_t maker = 0;
    int64_t price = 0;
    uint32_t qty = 0;
};

struct ExpectedBlock {
    std::string name;
    std::vector<ExpectTrade> trades;
    std::vector<ExpectOrder> bids;
    std::vector<ExpectOrder> asks;
    uint64_t rejects = 0;
};

std::vector<ExpectedBlock> load_expected(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    std::vector<ExpectedBlock> blocks;
    ExpectedBlock current;
    std::string raw;
    while (std::getline(in, raw)) {
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        std::string line = raw;
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        line = line.substr(start);
        if (line.empty()) {
            continue;
        }
        if (line.rfind("# name:", 0) == 0) {
            current.name = line.substr(7);
            std::size_t pos = 0;
            while (pos < current.name.size() && current.name[pos] == ' ') {
                ++pos;
            }
            current.name = current.name.substr(pos);
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        if (line == "END") {
            blocks.push_back(std::move(current));
            current = ExpectedBlock{};
            continue;
        }
        std::istringstream row(line);
        std::string tag;
        row >> tag;
        if (tag == "T") {
            ExpectTrade trade;
            row >> trade.taker >> trade.maker >> trade.price >> trade.qty;
            if (!row) {
                throw std::runtime_error("bad trade row " + line);
            }
            current.trades.push_back(trade);
        } else if (tag == "B" || tag == "A") {
            ExpectOrder order;
            row >> order.id >> order.price >> order.qty;
            if (!row) {
                throw std::runtime_error("bad order row " + line);
            }
            if (tag == "B") {
                current.bids.push_back(order);
            } else {
                current.asks.push_back(order);
            }
        } else if (tag == "R") {
            row >> current.rejects;
            if (!row) {
                throw std::runtime_error("bad reject row " + line);
            }
        } else {
            throw std::runtime_error("bad expected row " + line);
        }
    }
    if (!current.name.empty() || !current.trades.empty() || !current.bids.empty() || !current.asks.empty() ||
        current.rejects != 0) {
        throw std::runtime_error(path + ": missing final END");
    }
    return blocks;
}

}  // namespace

std::vector<Event> generate(uint64_t seed, uint64_t count) {
    // Lockstep with python/lob/generate.py. Do not reorder draws.
    SplitMix64 rng(seed);
    int64_t fair = 10000;
    std::vector<uint64_t> live;
    live.reserve(static_cast<std::size_t>(count / 4) + 1);
    std::unordered_map<uint64_t, std::pair<int64_t, uint32_t>> meta;
    uint64_t next_id = 1;
    std::vector<Event> events;
    events.reserve(static_cast<std::size_t>(count));

    for (uint64_t i = 1; i <= count; ++i) {
        if (!live.empty()) {
            uint64_t roll = rng.next() % 100;
            if (roll < 35) {
                uint64_t idx = rng.next() % live.size();
                uint64_t oid = live[idx];
                live[idx] = live.back();
                live.pop_back();
                meta.erase(oid);
                Event event;
                event.type = EventType::Cancel;
                event.id = oid;
                event.ts = i;
                events.push_back(event);
                continue;
            }
            if (roll < 45) {
                uint64_t idx = rng.next() % live.size();
                uint64_t oid = live[idx];
                auto found = meta.find(oid);
                int64_t old_price = found->second.first;
                uint32_t old_qty = found->second.second;
                uint64_t mode = rng.next() % 2;
                uint64_t rprice = rng.next();
                uint64_t rqty = rng.next();
                int64_t new_price = 0;
                uint32_t new_qty = 0;
                if (mode == 0 && old_qty > 1) {
                    new_price = old_price;
                    new_qty = static_cast<uint32_t>(1 + (rqty % (static_cast<uint64_t>(old_qty) - 1)));
                } else {
                    int64_t off = static_cast<int64_t>(rprice % 41) - 20;
                    new_price = clamp_i64(fair + off, 1, 20000);
                    new_qty = static_cast<uint32_t>(1 + (rqty % 200));
                }
                found->second = {new_price, new_qty};
                Event event;
                event.type = EventType::Modify;
                event.id = oid;
                event.price = new_price;
                event.qty = new_qty;
                event.ts = i;
                events.push_back(event);
                continue;
            }
        }

        int64_t step = static_cast<int64_t>(rng.next() % 3);
        fair = clamp_i64(fair + step - 1, 8000, 12000);
        Side side = (rng.next() & 1ull) == 0 ? Side::Buy : Side::Sell;
        bool aggressive = (rng.next() % 10) < 3;
        int64_t off = static_cast<int64_t>(1 + (rng.next() % 20));
        int64_t price = 0;
        if (side == Side::Buy) {
            price = aggressive ? fair + off : fair - off;
        } else {
            price = aggressive ? fair - off : fair + off;
        }
        price = clamp_i64(price, 1, 20000);
        uint32_t qty = static_cast<uint32_t>(1 + (rng.next() % 200));
        uint64_t oid = next_id++;
        live.push_back(oid);
        meta[oid] = {price, qty};
        Event event;
        event.type = EventType::Add;
        event.id = oid;
        event.side = side;
        event.price = price;
        event.qty = qty;
        event.tif = Tif::Gtc;
        event.ts = i;
        events.push_back(event);
    }
    return events;
}

void write_binary(const std::string& path, uint64_t seed, const std::vector<Event>& events) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + path);
    }
    write_pod(out, kMagic, 8);
    write_u32(out, 1);
    write_u32(out, 32);
    write_u64(out, events.size());
    write_u64(out, seed);
    for (const Event& event : events) {
        unsigned char head[4] = {static_cast<unsigned char>(event.type), static_cast<unsigned char>(event.side),
                                 static_cast<unsigned char>(event.tif), 0};
        write_pod(out, head, 4);
        write_u32(out, event.qty);
        write_u64(out, event.id);
        write_u64(out, static_cast<uint64_t>(event.price));
        write_u64(out, event.ts);
    }
}

std::vector<Event> read_binary(const std::string& path, uint64_t* seed) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    char magic[8];
    read_exact(in, magic, 8, path);
    for (int i = 0; i < 8; ++i) {
        if (magic[i] != kMagic[i]) {
            throw std::runtime_error(path + ": bad magic");
        }
    }
    uint32_t version = read_u32(in, path);
    uint32_t header = read_u32(in, path);
    uint64_t count = read_u64(in, path);
    uint64_t file_seed = read_u64(in, path);
    if (version != 1 || header != 32) {
        throw std::runtime_error(path + ": unsupported header");
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size != static_cast<std::streamoff>(32 + count * 32)) {
        throw std::runtime_error(path + ": unexpected file size");
    }
    in.seekg(32, std::ios::beg);
    std::vector<Event> events;
    events.reserve(static_cast<std::size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        unsigned char head[4];
        read_exact(in, head, 4, path);
        Event event;
        event.type = static_cast<EventType>(head[0]);
        event.side = static_cast<Side>(head[1]);
        event.tif = static_cast<Tif>(head[2]);
        event.qty = read_u32(in, path);
        event.id = read_u64(in, path);
        event.price = static_cast<int64_t>(read_u64(in, path));
        event.ts = read_u64(in, path);
        events.push_back(event);
    }
    if (seed != nullptr) {
        *seed = file_seed;
    }
    return events;
}

std::vector<Event> read_text(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    std::vector<Event> events;
    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        std::size_t start = 0;
        while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t')) {
            ++start;
        }
        if (start == raw.size() || raw[start] == '#') {
            continue;
        }
        try {
            events.push_back(parse_event_line(raw.substr(start)));
        } catch (const std::exception& ex) {
            throw std::runtime_error(path + ":" + std::to_string(lineno) + ": " + ex.what());
        }
    }
    return events;
}

std::vector<Event> read_any(const std::string& path, uint64_t* seed) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    char magic[8] = {};
    in.read(magic, 8);
    bool binary = in.gcount() == 8;
    for (int i = 0; binary && i < 8; ++i) {
        if (magic[i] != kMagic[i]) {
            binary = false;
        }
    }
    in.close();
    if (binary) {
        return read_binary(path, seed);
    }
    if (seed != nullptr) {
        *seed = 0;
    }
    return read_text(path);
}

std::string fingerprint_line(uint64_t index, const Book& book) {
    Book::Top top = book.top();
    std::ostringstream out;
    out << index << ' ' << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << book.book_hash()
        << ' ' << std::setw(16) << book.trade_hash() << std::dec << std::setfill(' ') << ' ' << book.trade_count()
        << ' ' << top.bid_px << ' ' << top.bid_qty << ' ' << top.ask_px << ' ' << top.ask_qty << ' '
        << book.order_count() << ' ' << book.reject_count();
    return out.str();
}

TypeCounts count_types(const std::vector<Event>& events) {
    TypeCounts counts;
    for (const Event& event : events) {
        switch (event.type) {
            case EventType::Add:
                ++counts.add;
                break;
            case EventType::Cancel:
                ++counts.cancel;
                break;
            case EventType::Modify:
                ++counts.modify;
                break;
            case EventType::Reset:
                ++counts.reset;
                break;
            default:
                ++counts.other;
                break;
        }
    }
    return counts;
}

void replay_to_file(const std::string& input, const std::string& output, uint64_t checkpoint, bool verify) {
    if (checkpoint == 0) {
        throw std::runtime_error("checkpoint must be >= 1");
    }
    uint64_t seed = 0;
    std::vector<Event> events = read_any(input, &seed);
    Book book;
    std::ofstream out(output);
    if (!out) {
        throw std::runtime_error("cannot write " + output);
    }
    const uint64_t last = events.size();
    for (uint64_t index = 1; index <= last; ++index) {
        book.apply(events[static_cast<std::size_t>(index - 1)]);
        if (index % checkpoint == 0 || index == last) {
            if (verify) {
                std::string error;
                if (!book.consistent(error)) {
                    throw std::runtime_error("inconsistent at event " + std::to_string(index) + ": " + error);
                }
            }
            out << fingerprint_line(index, book) << '\n';
        }
    }
    TypeCounts counts = count_types(events);
    Book::Depth depth = book.depth();
    std::cout << "summary events=" << events.size() << " adds=" << counts.add << " cancels=" << counts.cancel
              << " modifies=" << counts.modify << " resets=" << counts.reset << " trades=" << book.trade_count()
              << " rejects=" << book.reject_count() << " orders=" << book.order_count()
              << " bid_levels=" << depth.bid_levels << " ask_levels=" << depth.ask_levels
              << " bid_orders=" << depth.bid_orders << " ask_orders=" << depth.ask_orders
              << " bid_qty=" << depth.bid_qty << " ask_qty=" << depth.ask_qty << std::hex << " book=" << std::setfill('0')
              << std::setw(16) << book.book_hash() << " trade_hash=" << std::setw(16) << book.trade_hash() << std::dec
              << '\n';
    std::cout << "wrote " << output << " lines=" << ((last + checkpoint - 1) / checkpoint) << '\n';
}

int check_spec(const std::string& scenarios_path, const std::string& expected_path) {
    SplitMix64 rng(0);
    if (rng.next() != 0xE220A8397B1DCDAFull) {
        std::cerr << "splitmix vector mismatch\n";
        return 1;
    }
    if (Book{}.book_hash() != kFnvOffset) {
        std::cerr << "empty book hash mismatch\n";
        return 1;
    }
    std::vector<Event> once = generate(42, 1000);
    std::vector<Event> twice = generate(42, 1000);
    if (once.size() != twice.size()) {
        std::cerr << "generator length drift\n";
        return 1;
    }
    for (std::size_t i = 0; i < once.size(); ++i) {
        const Event& a = once[i];
        const Event& b = twice[i];
        if (a.type != b.type || a.side != b.side || a.tif != b.tif || a.qty != b.qty || a.id != b.id ||
            a.price != b.price || a.ts != b.ts) {
            std::cerr << "generator is not deterministic\n";
            return 1;
        }
    }

    std::vector<ScenarioBlock> blocks = load_blocks(scenarios_path);
    std::vector<ExpectedBlock> spec = load_expected(expected_path);
    if (blocks.size() != spec.size()) {
        std::cerr << blocks.size() << " scenario blocks vs " << spec.size() << " expected blocks\n";
        return 1;
    }
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].name != spec[i].name) {
            std::cerr << "name " << blocks[i].name << " != " << spec[i].name << '\n';
            return 1;
        }
        Book book;
        book.set_record(true);
        for (const Event& event : blocks[i].events) {
            book.apply(event);
        }
        std::string error;
        if (!book.consistent(error)) {
            std::cerr << spec[i].name << " inconsistent: " << error << '\n';
            return 1;
        }
        const std::vector<Trade>& trades = book.recorded();
        if (trades.size() != spec[i].trades.size()) {
            std::cerr << spec[i].name << " trade count " << trades.size() << " != " << spec[i].trades.size() << '\n';
            return 1;
        }
        for (std::size_t t = 0; t < trades.size(); ++t) {
            const Trade& got = trades[t];
            const ExpectTrade& want = spec[i].trades[t];
            if (got.taker_id != want.taker || got.maker_id != want.maker || got.price != want.price ||
                got.qty != want.qty) {
                std::cerr << spec[i].name << " trade " << t << " mismatch\n";
                return 1;
            }
        }
        std::vector<Book::LevelView> bids;
        std::vector<Book::LevelView> asks;
        book.snapshot(bids, asks);
        auto same_orders = [](const std::vector<Book::LevelView>& got, const std::vector<ExpectOrder>& want) {
            if (got.size() != want.size()) {
                return false;
            }
            for (std::size_t n = 0; n < got.size(); ++n) {
                if (got[n].id != want[n].id || got[n].price != want[n].price || got[n].qty != want[n].qty) {
                    return false;
                }
            }
            return true;
        };
        if (!same_orders(bids, spec[i].bids) || !same_orders(asks, spec[i].asks)) {
            std::cerr << spec[i].name << " book snapshot mismatch\n";
            return 1;
        }
        if (book.reject_count() != spec[i].rejects) {
            std::cerr << spec[i].name << " rejects " << book.reject_count() << " != " << spec[i].rejects << '\n';
            return 1;
        }
    }

    Book replay_a;
    Book replay_b;
    for (const Event& event : once) {
        replay_a.apply(event);
        replay_b.apply(event);
    }
    if (replay_a.book_hash() != replay_b.book_hash() || replay_a.trade_hash() != replay_b.trade_hash()) {
        std::cerr << "replay hash drifted\n";
        return 1;
    }
    std::cout << "cpp spec OK scenarios=" << blocks.size() << '\n';
    return 0;
}

}  // namespace lob
