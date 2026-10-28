#pragma once

#include "lob/id_index.hpp"
#include "lob/profile.hpp"
#include "lob/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace lob {

// Single-symbol limit order book.
//
// Bids: highest price first, then earliest rest time.
// Asks: lowest price first, then earliest rest time.
// The FIFO at a price level is the time key. Timestamps come from the event.
// Matching never reads the wall clock, so a recording replays bit-for-bit.
//
// A trade takes the resting order's price. GTC leftovers rest. IOC leftovers
// do not. Modify keeps priority only when the price is unchanged and the
// remaining quantity decreases. Any other successful modify detaches the
// order and re-enters it at the tail, where it may also take liquidity.
class Book {
public:
    struct Top {
        int64_t bid_px = 0;
        uint64_t bid_qty = 0;
        int64_t ask_px = 0;
        uint64_t ask_qty = 0;
    };

    struct LevelView {
        uint64_t id = 0;
        int64_t price = 0;
        uint32_t qty = 0;
    };

    struct Depth {
        uint64_t bid_levels = 0;
        uint64_t ask_levels = 0;
        uint64_t bid_orders = 0;
        uint64_t ask_orders = 0;
        uint64_t bid_qty = 0;
        uint64_t ask_qty = 0;
    };

    Book();
    ~Book();
    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    void apply(const Event& event);
    void set_record(bool on);

    uint64_t book_hash() const;
    uint64_t trade_hash() const { return trade_hash_; }
    uint64_t trade_count() const { return trade_count_; }
    uint64_t reject_count() const { return reject_count_; }
    uint64_t order_count() const { return index_.size(); }

    Top top() const;
    Depth depth() const;
    void snapshot(std::vector<LevelView>& bids, std::vector<LevelView>& asks) const;
    const std::vector<Trade>& recorded() const { return recorded_; }
    bool consistent(std::string& error) const;
    void dump_profile(std::ostream& out) const;

private:
    struct Level;

    struct Order {
        uint64_t id = 0;
        int64_t price = 0;
        uint32_t qty = 0;
        Side side = Side::Buy;
        uint64_t ts = 0;
        Order* prev = nullptr;
        Order* next = nullptr;
        Level* level = nullptr;
    };

    struct Level {
        int64_t price = 0;
        uint64_t total_qty = 0;
        Order* head = nullptr;
        Order* tail = nullptr;
    };

    using BidMap = std::map<int64_t, Level, std::greater<int64_t>>;
    using AskMap = std::map<int64_t, Level>;
    using Index = IdIndex<Order>;

    void clear_book();
    void reject(RejectReason reason);
    void emit(uint64_t ts, uint64_t taker, uint64_t maker, int64_t price, uint32_t qty);
    void unlink(Order* order);
    void erase_level_if_empty(Level* level, int64_t price, Side side);
    void rest(Order* order);
    void match(Order* taker, uint64_t ts);
    void add(const Event& event);
    void cancel(const Event& event);
    void modify(const Event& event);

    template <typename Map>
    void rest_on(Map& book, Order* order);

    template <typename Map, typename Cross>
    void match_side(Map& book, Order* taker, uint64_t ts, Cross cross);

    BidMap bids_;
    AskMap asks_;
    // Id lookup. Open addressing replaced std::unordered_map after insert/erase
    // dominated the phase profile. Price levels stay in std::map; creating one
    // was under 1% of apply time at this depth.
    Index index_;
    uint64_t trade_hash_ = kFnvOffset;
    uint64_t trade_count_ = 0;
    uint64_t reject_count_ = 0;
    bool record_ = false;
    std::vector<Trade> recorded_;
    uint64_t reason_counts_[8] = {};

    Probe id_find_{};
    Probe match_{};
    Probe rest_{};
    Probe order_new_{};
    Probe release_{};
    Probe order_delete_{};
    Probe modify_inplace_{};
    Probe trade_hash_probe_{};
    Probe level_insert_{};
    Probe index_write_{};
    Probe level_erase_{};
    uint64_t apply_ticks_ = 0;
    uint64_t apply_calls_ = 0;
};

}  // namespace lob
