#include "lob/wire.hpp"

#include <cstring>

namespace lob::wire {
namespace {

constexpr uint8_t kMagic[4] = {'L', 'O', 'B', 'M'};

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

}  // namespace

const char* status_name(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::Short: return "short";
        case Status::BadMagic: return "bad magic";
        case Status::BadVersion: return "bad version";
        case Status::BadFlags: return "bad flags";
        case Status::BadCount: return "bad count";
        case Status::BadLength: return "bad length";
    }
    return "unknown";
}

void encode_event(const Event& event, uint8_t* out) {
    out[0] = static_cast<uint8_t>(event.type);
    out[1] = static_cast<uint8_t>(event.side);
    out[2] = static_cast<uint8_t>(event.tif);
    out[3] = 0;
    put_u32(out + 4, event.qty);
    put_u64(out + 8, event.id);
    put_u64(out + 16, static_cast<uint64_t>(event.price));
    put_u64(out + 24, event.ts);
}

void decode_event(const uint8_t* in, Event* out) {
    out->type = static_cast<EventType>(in[0]);
    out->side = static_cast<Side>(in[1]);
    out->tif = static_cast<Tif>(in[2]);
    out->qty = get_u32(in + 4);
    out->id = get_u64(in + 8);
    out->price = static_cast<int64_t>(get_u64(in + 16));
    out->ts = get_u64(in + 24);
}

Status decode_header(const uint8_t* data, std::size_t len, Header* out) {
    if (len < kHeaderSize) {
        return Status::Short;
    }
    if (std::memcmp(data, kMagic, 4) != 0) {
        return Status::BadMagic;
    }
    if (data[4] != kVersion) {
        return Status::BadVersion;
    }
    if ((data[5] & ~kFlagEnd) != 0) {
        return Status::BadFlags;
    }
    const uint16_t count = get_u16(data + 6);
    if (count > kMaxEvents) {
        return Status::BadCount;
    }
    out->flags = data[5];
    out->count = count;
    out->seq = get_u64(data + 8);
    out->send_ns = get_u64(data + 16);
    return Status::Ok;
}

Status decode_packet(const uint8_t* data, std::size_t len, Packet* out) {
    Header header;
    Status status = decode_header(data, len, &header);
    if (status != Status::Ok) {
        return status;
    }
    if (len != packet_size(header.count)) {
        return Status::BadLength;
    }
    out->header = header;
    for (uint16_t i = 0; i < header.count; ++i) {
        decode_event(data + kHeaderSize + static_cast<std::size_t>(i) * kEventSize, &out->events[i]);
    }
    return Status::Ok;
}

std::size_t encode_packet(const Header& header, const Event* events, uint8_t* out) {
    std::memcpy(out, kMagic, 4);
    out[4] = kVersion;
    out[5] = header.flags;
    put_u16(out + 6, header.count);
    put_u64(out + 8, header.seq);
    put_u64(out + 16, header.send_ns);
    for (uint16_t i = 0; i < header.count; ++i) {
        encode_event(events[i], out + kHeaderSize + static_cast<std::size_t>(i) * kEventSize);
    }
    return packet_size(header.count);
}

void set_send_time(uint8_t* packet, uint64_t send_ns) { put_u64(packet + 16, send_ns); }

}  // namespace lob::wire
