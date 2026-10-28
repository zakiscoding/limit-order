#pragma once

#include <cstdint>

namespace lob {

// Word-wise FNV-style fingerprint. Keep this identical to python/lob/hashutil.py.
inline constexpr uint64_t kFnvOffset = 14695981039346656037ull;
inline constexpr uint64_t kFnvPrime = 1099511628211ull;

inline uint64_t mix(uint64_t h, uint64_t value) {
    h ^= value;
    h *= kFnvPrime;
    return h;
}

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class EventType : uint8_t { Add = 1, Cancel = 2, Modify = 3, Reset = 4 };
enum class Tif : uint8_t { Gtc = 0, Ioc = 1 };

enum class RejectReason : uint8_t {
    DuplicateId = 1,
    UnknownOrder = 2,
    BadQty = 3,
    BadPrice = 4,
    BadSide = 5,
    BadTif = 6,
    BadType = 7,
};

struct Event {
    EventType type = EventType::Add;
    Side side = Side::Buy;
    Tif tif = Tif::Gtc;
    uint32_t qty = 0;
    uint64_t id = 0;
    int64_t price = 0;
    uint64_t ts = 0;
};

struct Trade {
    uint64_t ts = 0;
    uint64_t taker_id = 0;
    uint64_t maker_id = 0;
    int64_t price = 0;
    uint32_t qty = 0;
};

// SplitMix64. Same recurrence as python/lob/generate.py.
class SplitMix64 {
public:
    explicit SplitMix64(uint64_t seed) : state_(seed) {}

    uint64_t next() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

private:
    uint64_t state_;
};

}  // namespace lob
