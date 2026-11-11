#pragma once

#include "lob/wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lob {

// Puts packets from one or more lines back into sequence order.
//
// Events are numbered from first_seq. push() hands events to a sink exactly once
// and strictly in order, whatever order or duplication they arrive in. Packets
// that arrive ahead of a missing one wait in a fixed buffer of kWindow packets
// (no allocation). If a packet arrives ahead of a missing one and the buffer is
// full, the missing events are treated as lost: gap() turns true and stays true.
// The caller must stop there. Skipping the gap would apply events to a book that
// is missing some, and the result could never match a replay of the full log.
//
// The sink is called as sink(const Event* events, std::size_t count, uint64_t send_ns).
class Sequencer {
public:
    static constexpr std::size_t kWindow = 64;

    struct Stats {
        uint64_t packets = 0;          // push() calls
        uint64_t duplicates = 0;       // packets (or buffered copies) with nothing new
        uint64_t reordered = 0;        // packets that had to wait for an earlier one
        uint64_t partial_overlaps = 0; // packets delivered from the middle (different batching)
        uint64_t empty = 0;            // non-END packets with no events
        uint64_t invalid = 0;          // sequence arithmetic overflow
        uint64_t conflicting_end = 0;  // an END that disagrees with the first one
        uint64_t gaps = 0;             // packets refused because the buffer was full
    };

    enum class Result { Delivered, Buffered, Duplicate, Gap, Invalid };

    explicit Sequencer(uint64_t first_seq = 0) : next_(first_seq) {}

    template <typename Sink>
    Result push(const wire::Packet& packet, Sink&& sink) {
        ++stats_.packets;
        const uint64_t first = packet.header.seq;
        const uint64_t last = first + packet.header.count;
        if (last < first) {
            ++stats_.invalid;
            return Result::Invalid;
        }
        if ((packet.header.flags & wire::kFlagEnd) != 0) {
            if (!end_known_) {
                end_known_ = true;
                end_seq_ = last;
            } else if (end_seq_ != last) {
                ++stats_.conflicting_end;
            }
        }
        if (packet.header.count == 0) {
            if ((packet.header.flags & wire::kFlagEnd) == 0) {
                ++stats_.empty;
            }
            return Result::Duplicate;
        }
        if (last <= next_) {
            ++stats_.duplicates;
            return Result::Duplicate;
        }
        if (first <= next_) {
            deliver(packet, sink);
            drain(sink);
            return Result::Delivered;
        }
        for (const Slot& slot : slots_) {
            if (slot.used && slot.packet.header.seq == first) {
                ++stats_.duplicates;
                return Result::Duplicate;
            }
        }
        for (Slot& slot : slots_) {
            if (!slot.used) {
                slot.packet = packet;
                slot.used = true;
                ++buffered_;
                ++stats_.reordered;
                return Result::Buffered;
            }
        }
        if (!gap_) {
            gap_ = true;
            gap_at_ = next_;
        }
        ++stats_.gaps;
        return Result::Gap;
    }

    uint64_t next_seq() const { return next_; }
    bool end_known() const { return end_known_; }
    uint64_t end_seq() const { return end_seq_; }
    bool finished() const { return end_known_ && next_ >= end_seq_; }
    bool gap() const { return gap_; }
    uint64_t gap_at() const { return gap_at_; }
    std::size_t buffered() const { return buffered_; }
    const Stats& stats() const { return stats_; }

private:
    struct Slot {
        wire::Packet packet;
        bool used = false;
    };

    template <typename Sink>
    void deliver(const wire::Packet& packet, Sink& sink) {
        const std::size_t skip = static_cast<std::size_t>(next_ - packet.header.seq);
        if (skip > 0) {
            ++stats_.partial_overlaps;
        }
        sink(packet.events + skip, static_cast<std::size_t>(packet.header.count) - skip, packet.header.send_ns);
        next_ = packet.header.seq + packet.header.count;
    }

    template <typename Sink>
    void drain(Sink& sink) {
        bool progress = true;
        while (progress && buffered_ > 0) {
            progress = false;
            for (Slot& slot : slots_) {
                if (!slot.used) {
                    continue;
                }
                const uint64_t first = slot.packet.header.seq;
                const uint64_t last = first + slot.packet.header.count;
                if (last <= next_) {
                    slot.used = false;
                    --buffered_;
                    ++stats_.duplicates;
                    progress = true;
                } else if (first <= next_) {
                    deliver(slot.packet, sink);
                    slot.used = false;
                    --buffered_;
                    progress = true;
                }
            }
        }
    }

    std::array<Slot, kWindow> slots_{};
    uint64_t next_;
    bool end_known_ = false;
    uint64_t end_seq_ = 0;
    bool gap_ = false;
    uint64_t gap_at_ = 0;
    std::size_t buffered_ = 0;
    Stats stats_;
};

}  // namespace lob
