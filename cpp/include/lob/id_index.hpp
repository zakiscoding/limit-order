#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// Open-addressed id -> order map.
//
// std::unordered_map insert and erase were the largest measured cost in the
// matching loop (node allocation and pointer chasing). This table keeps slots
// in one array, probes linearly, and rebuilds when tombstones pile up.
// Empty slots are word 0. Tombstones are word 1. A live slot stores an
// 8-byte-aligned order pointer, which cannot be 0 or 1.
template <typename T>
class IdIndex {
public:
    IdIndex() { grow(16); }

    T* find(uint64_t id) const {
        if (cap_ == 0) {
            return nullptr;
        }
        const uint64_t mask = cap_ - 1;
        uint64_t index = mix_id(id) & mask;
        for (uint64_t n = 0; n < cap_; ++n) {
            const Slot& slot = slots_[static_cast<std::size_t>(index)];
            if (slot.word == kEmpty) {
                return nullptr;
            }
            if (slot.word != kTomb && slot.id == id) {
                return reinterpret_cast<T*>(slot.word);
            }
            index = (index + 1) & mask;
        }
        return nullptr;
    }

    void insert(uint64_t id, T* value) {
        if (cap_ == 0 || (size_ + tombs_ + 1) * 4 > cap_ * 3) {
            uint64_t need = cap_ < 16 ? 16 : cap_ * 2;
            while ((size_ + 1) * 2 > need) {
                need *= 2;
            }
            grow(need);
        }
        place(id, value);
    }

    void erase(uint64_t id) {
        if (cap_ == 0) {
            return;
        }
        const uint64_t mask = cap_ - 1;
        uint64_t index = mix_id(id) & mask;
        for (uint64_t n = 0; n < cap_; ++n) {
            Slot& slot = slots_[static_cast<std::size_t>(index)];
            if (slot.word == kEmpty) {
                return;
            }
            if (slot.word != kTomb && slot.id == id) {
                slot.word = kTomb;
                slot.id = 0;
                --size_;
                ++tombs_;
                return;
            }
            index = (index + 1) & mask;
        }
    }

    void clear() {
        std::vector<Slot> empty;
        slots_.swap(empty);
        cap_ = 0;
        size_ = 0;
        tombs_ = 0;
        grow(16);
    }

    uint64_t size() const { return size_; }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const Slot& slot : slots_) {
            if (slot.word > kTomb) {
                fn(reinterpret_cast<T*>(slot.word));
            }
        }
    }

private:
    struct Slot {
        uint64_t id = 0;
        uintptr_t word = 0;
    };

    static constexpr uintptr_t kEmpty = 0;
    static constexpr uintptr_t kTomb = 1;

    static uint64_t mix_id(uint64_t id) {
        // One multiply. The full SplitMix mix made lookups slower than the
        // insert/erase win was worth; this is enough to spread sequential ids.
        return id * 0x9E3779B97F4A7C15ull;
    }

    void grow(uint64_t need) {
        uint64_t cap = 16;
        while (cap < need) {
            cap *= 2;
        }
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(static_cast<std::size_t>(cap), Slot{});
        cap_ = cap;
        size_ = 0;
        tombs_ = 0;
        for (const Slot& slot : old) {
            if (slot.word > kTomb) {
                place(slot.id, reinterpret_cast<T*>(slot.word));
            }
        }
    }

    void place(uint64_t id, T* value) {
        const uint64_t mask = cap_ - 1;
        uint64_t index = mix_id(id) & mask;
        int64_t tomb = -1;
        const uintptr_t word = reinterpret_cast<uintptr_t>(value);
        for (;;) {
            Slot& slot = slots_[static_cast<std::size_t>(index)];
            if (slot.word == kEmpty) {
                Slot& dest = tomb >= 0 ? slots_[static_cast<std::size_t>(tomb)] : slot;
                if (tomb >= 0) {
                    --tombs_;
                }
                dest.id = id;
                dest.word = word;
                ++size_;
                return;
            }
            if (slot.word == kTomb) {
                if (tomb < 0) {
                    tomb = static_cast<int64_t>(index);
                }
            } else if (slot.id == id) {
                slot.word = word;
                return;
            }
            index = (index + 1) & mask;
        }
    }

    std::vector<Slot> slots_;
    uint64_t cap_ = 0;
    uint64_t size_ = 0;
    uint64_t tombs_ = 0;
};

}  // namespace lob
