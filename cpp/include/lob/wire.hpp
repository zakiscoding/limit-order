#pragma once

#include "lob/types.hpp"

#include <cstddef>
#include <cstdint>

namespace lob::wire {

// Market-data packet, little-endian. The same bytes are one UDP datagram or one
// frame of a TCP byte stream.
//
//   offset  size  field
//   0       4     magic "LOBM"
//   4       1     version (1)
//   5       1     flags (bit 0: END)
//   6       2     count of events that follow (0..40)
//   8       8     sequence number of the first event; events are numbered from 0
//   16      8     sender monotonic clock in nanoseconds (0 when not stamped)
//   24      32*n  events, in the replay file layout (see README)
//
// END marks the end of the stream: seq + count is the total number of events.
// A packet is at most 24 + 40 * 32 = 1304 bytes, under a 1500-byte Ethernet MTU.
inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::size_t kEventSize = 32;
inline constexpr std::size_t kMaxEvents = 40;
inline constexpr std::size_t kMaxPacketSize = kHeaderSize + kMaxEvents * kEventSize;
inline constexpr uint8_t kVersion = 1;
inline constexpr uint8_t kFlagEnd = 1;

struct Header {
    uint8_t flags = 0;
    uint16_t count = 0;
    uint64_t seq = 0;
    uint64_t send_ns = 0;
};

struct Packet {
    Header header;
    Event events[kMaxEvents];
};

enum class Status { Ok, Short, BadMagic, BadVersion, BadFlags, BadCount, BadLength };

const char* status_name(Status status);

constexpr std::size_t packet_size(uint16_t count) { return kHeaderSize + static_cast<std::size_t>(count) * kEventSize; }

// Parses the 24-byte header from data[0..len). Does not look at the events.
Status decode_header(const uint8_t* data, std::size_t len, Header* out);

// Parses one complete packet. len must be exactly packet_size(count). Event
// fields are copied as they are: the book, not the wire layer, rejects bad
// sides, types, and quantities, so a replay and a feed treat them alike.
Status decode_packet(const uint8_t* data, std::size_t len, Packet* out);

// Writes the packet to out, which must hold packet_size(header.count) bytes.
std::size_t encode_packet(const Header& header, const Event* events, uint8_t* out);

// Overwrites the send timestamp of an encoded packet.
void set_send_time(uint8_t* packet, uint64_t send_ns);

void encode_event(const Event& event, uint8_t* out32);
void decode_event(const uint8_t* in32, Event* out);

}  // namespace lob::wire
