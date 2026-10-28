#include "lob/book.hpp"

#include <cstdlib>
#include <iostream>
#include <unordered_set>
#include <utility>

namespace lob {
namespace {

constexpr int kCalibIters = 200000;

[[maybe_unused]] uint64_t clock_overhead() {
    if constexpr (!kProf) {
        return 0;
    }
    Probe sink;
    uint64_t start = rdtsc_raw();
    for (int i = 0; i < kCalibIters; ++i) {
        Clock<true> clock(sink);
    }
    uint64_t mid = rdtsc_raw();
    for (int i = 0; i < kCalibIters; ++i) {
    }
    uint64_t end = rdtsc_raw();
    uint64_t timed = mid - start;
    uint64_t empty = end - mid;
    if (timed <= empty) {
        return 0;
    }
    return (timed - empty) / static_cast<uint64_t>(kCalibIters);
}

}  // namespace

Book::Book() = default;

Book::~Book() { clear_book(); }

void Book::set_record(bool on) {
    record_ = on;
    if (!on) {
        recorded_.clear();
    }
}

void Book::clear_book() {
    index_.for_each([](Order* order) { delete order; });
    index_.clear();
    bids_.clear();
    asks_.clear();
}

void Book::reject(RejectReason reason) {
    ++reject_count_;
    ++reason_counts_[static_cast<unsigned>(reason)];
}

void Book::emit(uint64_t ts, uint64_t taker, uint64_t maker, int64_t price, uint32_t qty) {
    ++trade_count_;
    uint64_t h = trade_hash_;
    h = mix(h, ts);
    h = mix(h, taker);
    h = mix(h, maker);
    h = mix(h, static_cast<uint64_t>(price));
    h = mix(h, qty);
    trade_hash_ = h;
    if (record_) {
        recorded_.push_back(Trade{ts, taker, maker, price, qty});
    }
}

void Book::unlink(Order* order) {
    Level* level = order->level;
    if (order->prev != nullptr) {
        order->prev->next = order->next;
    } else {
        level->head = order->next;
    }
    if (order->next != nullptr) {
        order->next->prev = order->prev;
    } else {
        level->tail = order->prev;
    }
    order->prev = nullptr;
    order->next = nullptr;
    order->level = nullptr;
}

void Book::erase_level_if_empty(Level* level, int64_t price, Side side) {
    if (level->head != nullptr) {
        return;
    }
    if (side == Side::Buy) {
        bids_.erase(price);
    } else {
        asks_.erase(price);
    }
}

template <typename Map>
void Book::rest_on(Map& book, Order* order) {
    Clock<kProf> link(rest_);
    auto it = book.find(order->price);
    if (it == book.end()) {
        link.pause();
        {
            Clock<kProf> insert(level_insert_);
            Level level;
            level.price = order->price;
            it = book.emplace(order->price, level).first;
        }
        link.resume();
    }
    Level& level = it->second;
    order->prev = level.tail;
    order->next = nullptr;
    if (level.tail != nullptr) {
        level.tail->next = order;
    } else {
        level.head = order;
    }
    level.tail = order;
    order->level = &level;
    level.total_qty += order->qty;
    link.pause();
    Clock<kProf> index(index_write_);
    index_.insert(order->id, order);
}

void Book::rest(Order* order) {
    if (order->side == Side::Buy) {
        rest_on(bids_, order);
    } else {
        rest_on(asks_, order);
    }
}

template <typename Map, typename Cross>
void Book::match_side(Map& book, Order* taker, uint64_t ts, Cross cross) {
    while (taker->qty > 0 && !book.empty()) {
        Clock<kProf> walk(match_);
        auto it = book.begin();
        if (!cross(it->first, taker->price)) {
            break;
        }
        Level* level = &it->second;
        Order* maker = level->head;
        while (taker->qty > 0 && maker != nullptr) {
            Order* next = maker->next;
            uint32_t fill = taker->qty < maker->qty ? taker->qty : maker->qty;
            walk.pause();
            {
                Clock<kProf> hash(trade_hash_probe_);
                emit(ts, taker->id, maker->id, maker->price, fill);
            }
            walk.resume();
            taker->qty -= fill;
            maker->qty -= fill;
            level->total_qty -= fill;
            if (maker->qty == 0) {
                walk.pause();
                {
                    Clock<kProf> rel(release_);
                    unlink(maker);
                }
                {
                    Clock<kProf> index(index_write_);
                    index_.erase(maker->id);
                }
                {
                    Clock<kProf> del(order_delete_);
                    delete maker;
                }
                walk.resume();
            }
            maker = next;
        }
        if (level->head == nullptr) {
#ifndef NDEBUG
            if (level->total_qty != 0) {
                std::cerr << "level quantity drift at price " << level->price << '\n';
                std::abort();
            }
#endif
            walk.pause();
            Clock<kProf> erase(level_erase_);
            book.erase(it);
        }
    }
}

void Book::match(Order* taker, uint64_t ts) {
    if (taker->side == Side::Buy) {
        match_side(asks_, taker, ts, [](int64_t level_px, int64_t taker_px) {
            return level_px <= taker_px;
        });
    } else {
        match_side(bids_, taker, ts, [](int64_t level_px, int64_t taker_px) {
            return level_px >= taker_px;
        });
    }
}

void Book::add(const Event& event) {
    if (event.qty == 0) {
        reject(RejectReason::BadQty);
        return;
    }
    if (event.price <= 0) {
        reject(RejectReason::BadPrice);
        return;
    }
    if (event.side != Side::Buy && event.side != Side::Sell) {
        reject(RejectReason::BadSide);
        return;
    }
    if (event.tif != Tif::Gtc && event.tif != Tif::Ioc) {
        reject(RejectReason::BadTif);
        return;
    }
    {
        Clock<kProf> found(id_find_);
        if (index_.find(event.id) != nullptr) {
            found.pause();
            reject(RejectReason::DuplicateId);
            return;
        }
    }
    Order taker;
    taker.id = event.id;
    taker.side = event.side;
    taker.price = event.price;
    taker.qty = event.qty;
    taker.ts = event.ts;
    match(&taker, event.ts);
    if (taker.qty > 0 && event.tif == Tif::Gtc) {
        Order* resting = nullptr;
        {
            Clock<kProf> created(order_new_);
            resting = new Order(taker);
        }
        rest(resting);
    }
}

void Book::cancel(const Event& event) {
    Order* order = nullptr;
    {
        Clock<kProf> found(id_find_);
        order = index_.find(event.id);
        if (order == nullptr) {
            found.pause();
            reject(RejectReason::UnknownOrder);
            return;
        }
    }
    Level* level = order->level;
    int64_t price = order->price;
    Side side = order->side;
    {
        Clock<kProf> rel(release_);
        level->total_qty -= order->qty;
        unlink(order);
    }
    {
        Clock<kProf> index(index_write_);
        index_.erase(order->id);
    }
    {
        Clock<kProf> del(order_delete_);
        delete order;
    }
    if (level->head == nullptr) {
        Clock<kProf> erase(level_erase_);
        erase_level_if_empty(level, price, side);
    }
}

void Book::modify(const Event& event) {
    Order* order = nullptr;
    {
        Clock<kProf> found(id_find_);
        order = index_.find(event.id);
        if (order == nullptr) {
            found.pause();
            reject(RejectReason::UnknownOrder);
            return;
        }
    }
    if (event.qty == 0) {
        reject(RejectReason::BadQty);
        return;
    }
    if (event.price <= 0) {
        reject(RejectReason::BadPrice);
        return;
    }
    if (event.price == order->price && event.qty == order->qty) {
        return;
    }
    if (event.price == order->price && event.qty < order->qty) {
        Clock<kProf> inplace(modify_inplace_);
        order->level->total_qty -= static_cast<uint64_t>(order->qty - event.qty);
        order->qty = event.qty;
        return;
    }

    Level* level = order->level;
    int64_t price = order->price;
    Side side = order->side;
    {
        Clock<kProf> rel(release_);
        level->total_qty -= order->qty;
        unlink(order);
    }
    {
        Clock<kProf> index(index_write_);
        index_.erase(order->id);
    }
    if (level->head == nullptr) {
        Clock<kProf> erase(level_erase_);
        erase_level_if_empty(level, price, side);
    }
    order->price = event.price;
    order->qty = event.qty;
    order->ts = event.ts;
    match(order, event.ts);
    if (order->qty > 0) {
        rest(order);
    } else {
        Clock<kProf> del(order_delete_);
        delete order;
    }
}

void Book::apply(const Event& event) {
    uint64_t start = 0;
    if constexpr (kProf) {
        start = rdtsc_raw();
    }
    switch (event.type) {
        case EventType::Add:
            add(event);
            break;
        case EventType::Cancel:
            cancel(event);
            break;
        case EventType::Modify:
            modify(event);
            break;
        case EventType::Reset:
            clear_book();
            break;
        default:
            reject(RejectReason::BadType);
            break;
    }
    if constexpr (kProf) {
        apply_ticks_ += rdtsc_raw() - start;
        ++apply_calls_;
    }
}

uint64_t Book::book_hash() const {
    uint64_t h = kFnvOffset;
    auto mix_level = [&](const Level& level) {
        for (const Order* order = level.head; order != nullptr; order = order->next) {
            h = mix(h, order->id);
            h = mix(h, static_cast<uint64_t>(order->price));
            h = mix(h, order->qty);
            h = mix(h, static_cast<uint64_t>(order->side));
            h = mix(h, order->ts);
        }
    };
    for (const auto& entry : bids_) {
        mix_level(entry.second);
    }
    for (const auto& entry : asks_) {
        mix_level(entry.second);
    }
    return h;
}

Book::Top Book::top() const {
    Top top;
    if (!bids_.empty()) {
        top.bid_px = bids_.begin()->first;
        top.bid_qty = bids_.begin()->second.total_qty;
    }
    if (!asks_.empty()) {
        top.ask_px = asks_.begin()->first;
        top.ask_qty = asks_.begin()->second.total_qty;
    }
    return top;
}

Book::Depth Book::depth() const {
    Depth depth;
    depth.bid_levels = bids_.size();
    depth.ask_levels = asks_.size();
    auto accumulate = [](const auto& book, uint64_t& orders, uint64_t& qty) {
        for (const auto& entry : book) {
            qty += entry.second.total_qty;
            for (const Order* order = entry.second.head; order != nullptr; order = order->next) {
                ++orders;
            }
        }
    };
    accumulate(bids_, depth.bid_orders, depth.bid_qty);
    accumulate(asks_, depth.ask_orders, depth.ask_qty);
    return depth;
}

void Book::snapshot(std::vector<LevelView>& bids, std::vector<LevelView>& asks) const {
    bids.clear();
    asks.clear();
    for (const auto& entry : bids_) {
        for (const Order* order = entry.second.head; order != nullptr; order = order->next) {
            bids.push_back(LevelView{order->id, order->price, order->qty});
        }
    }
    for (const auto& entry : asks_) {
        for (const Order* order = entry.second.head; order != nullptr; order = order->next) {
            asks.push_back(LevelView{order->id, order->price, order->qty});
        }
    }
}

bool Book::consistent(std::string& error) const {
    std::unordered_set<uint64_t> seen;
    auto walk = [&](const auto& book, Side side) -> bool {
        for (const auto& entry : book) {
            const Level& level = entry.second;
            if (level.price != entry.first || level.head == nullptr || level.tail == nullptr) {
                error = "empty or mislabeled price level";
                return false;
            }
            uint64_t qty = 0;
            const Order* prev = nullptr;
            for (const Order* order = level.head; order != nullptr; order = order->next) {
                if (order->prev != prev || order->level != &level || order->side != side ||
                    order->price != level.price || order->qty == 0) {
                    error = "broken order link";
                    return false;
                }
                Order* found = index_.find(order->id);
                if (found != order || !seen.insert(order->id).second) {
                    error = "order index mismatch";
                    return false;
                }
                qty += order->qty;
                prev = order;
            }
            if (level.tail != prev || qty != level.total_qty) {
                error = "level quantity mismatch";
                return false;
            }
        }
        return true;
    };
    if (!walk(bids_, Side::Buy) || !walk(asks_, Side::Sell)) {
        return false;
    }
    if (seen.size() != index_.size()) {
        error = "index contains orders missing from the ladders";
        return false;
    }
    return true;
}

void Book::dump_profile(std::ostream& out) const {
    if constexpr (!kProf) {
        out << "profile: instrumentation disabled (configure with -DLOB_INSTRUMENT=ON)\n";
        return;
    }
    const uint64_t overhead = clock_overhead();
    struct Row {
        const char* name;
        Probe probe;
    };
    const Row rows[] = {
        {"id_find", id_find_},
        {"match", match_},
        {"trade_hash", trade_hash_probe_},
        {"rest", rest_},
        {"order_new", order_new_},
        {"level_insert", level_insert_},
        {"index_write", index_write_},
        {"release", release_},
        {"order_delete", order_delete_},
        {"level_erase", level_erase_},
        {"modify_inplace", modify_inplace_},
    };
    uint64_t accounted = 0;
    for (const Row& row : rows) {
        uint64_t cost = row.probe.calls * overhead;
        uint64_t ticks = row.probe.ticks > cost ? row.probe.ticks - cost : 0;
        accounted += ticks;
    }
    uint64_t apply_cost = apply_calls_ * overhead;
    uint64_t apply_ticks = apply_ticks_ > apply_cost ? apply_ticks_ - apply_cost : 0;
    out << "profile_apply_calls " << apply_calls_ << "\n";
    out << "profile_clock_overhead_cycles " << overhead << "\n";
    out << "phase calls raw_cycles adj_cycles avg_adj pct_of_apply\n";
    auto emit_row = [&](const char* name, uint64_t calls, uint64_t raw) {
        uint64_t cost = calls * overhead;
        uint64_t adj = raw > cost ? raw - cost : 0;
        double avg = calls == 0 ? 0.0 : static_cast<double>(adj) / static_cast<double>(calls);
        double pct = apply_ticks == 0 ? 0.0 : (100.0 * static_cast<double>(adj) / static_cast<double>(apply_ticks));
        out << name << ' ' << calls << ' ' << raw << ' ' << adj << ' ' << avg << ' ' << pct << '\n';
    };
    for (const Row& row : rows) {
        emit_row(row.name, row.probe.calls, row.probe.ticks);
    }
    uint64_t other = apply_ticks > accounted ? apply_ticks - accounted : 0;
    out << "accounted_cycles " << accounted << "\n";
    out << "apply_cycles " << apply_ticks << "\n";
    out << "unaccounted_cycles " << other << "\n";
    (void)accounted;
}

}  // namespace lob
