// SurfaceRT-WiDi - wire protocol implementation.
// SPDX-License-Identifier: MIT
#include "widi/protocol.h"

#include <cstring>

#include "widi/bytes.h"

namespace widi {
namespace {

constexpr uint32_t kCrcPoly = 0xEDB88320u;

// Built during static initialisation (before main / before any thread exists),
// so crc32() is safe to call from multiple threads without a lazy-init race.
struct CrcTable {
    uint32_t values[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (kCrcPoly ^ (c >> 1)) : (c >> 1);
            }
            values[i] = c;
        }
    }
};

const CrcTable kCrcTable;

bool known_msg_type(uint8_t raw) { return raw >= 1 && raw <= 9; }

// Rejects headers whose length cannot belong to the type. This keeps random
// garbage from parking the parser in "waiting for 16 MiB" state, which is the
// expensive failure mode of a length-prefixed protocol.
bool plausible_length(uint8_t type, uint32_t length) {
    switch (type) {
        case 2:  // Hello: version+geometry+caps+name
            return length >= 9 && length <= 300;
        case 3:  // HelloAck: Hello + session id
            return length >= 13 && length <= 300;
        case 4:  // Frame: 10-byte header + at least one pixel byte
            return length >= kFrameHeaderSize + 1 && length <= kMaxPayload;
        case 5:  // Stats: 4+4+2+2
            return length == 12;
        case 6:  // Input: 1+1+2+2+4
            return length == 10;
        case 7:  // Ping
        case 8:  // Pong
            return length == 0;
        case 9:  // Bye: optional reason byte
            return length <= 1;
        case 1:  // Beacon is UDP-only
        default:
            return false;
    }
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed) {
    const uint32_t* table = kCrcTable.values;
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> encode_message(MsgType type, uint16_t flags, uint32_t seq,
                                   const void* payload, size_t payloadSize) {
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + payloadSize);
    BufWriter w(out);
    w.u32(kMagic);
    w.u8(kProtocolVersion);
    w.u8(static_cast<uint8_t>(type));
    w.u16(flags);
    w.u32(seq);
    w.u32(static_cast<uint32_t>(payloadSize));
    const uint8_t* body = static_cast<const uint8_t*>(payload);
    const uint32_t crc = (body && payloadSize) ? crc32(body, payloadSize) : 0u;
    w.u32(crc);
    if (payloadSize) w.bytes(body, payloadSize);
    return out;
}

bool decode_header(const uint8_t* data, size_t size, Header& out, std::string& err) {
    if (size < kHeaderSize) {
        err = "short header";
        return false;
    }
    BufReader r(data, size);
    const uint32_t magic = r.u32();
    const uint8_t version = r.u8();
    const uint8_t rawType = r.u8();
    out.flags = r.u16();
    out.seq = r.u32();
    out.length = r.u32();
    out.crc = r.u32();
    if (!r.ok()) {
        err = "truncated header";
        return false;
    }
    if (magic != kMagic) {
        err = "bad magic";
        return false;
    }
    if (version != kProtocolVersion) {
        err = "unsupported protocol version";
        return false;
    }
    if (!known_msg_type(rawType)) {
        err = "unknown message type";
        return false;
    }
    if (out.length > kMaxPayload) {
        err = "payload too large";
        return false;
    }
    if (!plausible_length(rawType, out.length)) {
        err = "implausible payload length for message type";
        return false;
    }
    switch (rawType) {
        case 1: out.type = MsgType::Beacon; break;
        case 2: out.type = MsgType::Hello; break;
        case 3: out.type = MsgType::HelloAck; break;
        case 4: out.type = MsgType::Frame; break;
        case 5: out.type = MsgType::Stats; break;
        case 6: out.type = MsgType::Input; break;
        case 7: out.type = MsgType::Ping; break;
        case 8: out.type = MsgType::Pong; break;
        default: out.type = MsgType::Bye; break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hello / HelloAck
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_hello(const HelloInfo& info) {
    std::vector<uint8_t> out;
    BufWriter w(out);
    w.u16(info.protocolVersion);
    w.u16(info.maxWidth);
    w.u16(info.maxHeight);
    w.u8(info.maxFps);
    w.u8(info.capabilities);
    w.str(info.name);
    return out;
}

bool decode_hello(const uint8_t* data, size_t size, HelloInfo& out, std::string& err) {
    BufReader r(data, size);
    out.protocolVersion = r.u16();
    out.maxWidth = r.u16();
    out.maxHeight = r.u16();
    out.maxFps = r.u8();
    out.capabilities = r.u8();
    out.name = r.str();
    if (!r.ok()) {
        err = "malformed hello";
        return false;
    }
    if (out.protocolVersion != kProtocolVersion) {
        err = "peer protocol version mismatch";
        return false;
    }
    return true;
}

std::vector<uint8_t> encode_hello_ack(const HelloAckInfo& info) {
    std::vector<uint8_t> out;
    BufWriter w(out);
    w.u16(info.protocolVersion);
    w.u16(info.screenWidth);
    w.u16(info.screenHeight);
    w.u32(info.sessionId);
    w.u8(info.flags);
    w.u8(info.reason);
    w.str(info.name);
    return out;
}

bool decode_hello_ack(const uint8_t* data, size_t size, HelloAckInfo& out, std::string& err) {
    BufReader r(data, size);
    out.protocolVersion = r.u16();
    out.screenWidth = r.u16();
    out.screenHeight = r.u16();
    out.sessionId = r.u32();
    out.flags = r.u8();
    out.reason = r.u8();
    out.name = r.str();
    if (!r.ok()) {
        err = "malformed hello ack";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void append_frame_header(std::vector<uint8_t>& out, const FrameInfo& info) {
    BufWriter w(out);
    w.u16(info.width);
    w.u16(info.height);
    w.u8(static_cast<uint8_t>(info.format));
    w.u8(info.flags);
    w.u32(info.timestampMs);
}

bool decode_frame_header(const uint8_t* payload, size_t size, FrameInfo& out, std::string& err) {
    if (size < kFrameHeaderSize) {
        err = "short frame header";
        return false;
    }
    BufReader r(payload, size);
    out.width = r.u16();
    out.height = r.u16();
    const uint8_t format = r.u8();
    out.flags = r.u8();
    out.timestampMs = r.u32();
    if (!r.ok()) {
        err = "truncated frame header";
        return false;
    }
    if (format > static_cast<uint8_t>(PixelFormat::Jpeg)) {
        err = "unsupported pixel format";
        return false;
    }
    out.format = static_cast<PixelFormat>(format);
    if (out.width == 0 || out.height == 0) {
        err = "empty frame";
        return false;
    }
    if (size == kFrameHeaderSize) {
        err = "frame carries no pixels";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Stats / Input
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_stats(const StatsInfo& info) {
    std::vector<uint8_t> out;
    BufWriter w(out);
    w.u32(info.decoded);
    w.u32(info.dropped);
    w.u16(info.decodeMsQ8);
    w.u16(info.clientFpsQ8);
    return out;
}

bool decode_stats(const uint8_t* data, size_t size, StatsInfo& out, std::string& err) {
    BufReader r(data, size);
    out.decoded = r.u32();
    out.dropped = r.u32();
    out.decodeMsQ8 = r.u16();
    out.clientFpsQ8 = r.u16();
    if (!r.ok()) {
        err = "malformed stats";
        return false;
    }
    return true;
}

std::vector<uint8_t> encode_input(const InputEvent& ev) {
    std::vector<uint8_t> out;
    BufWriter w(out);
    w.u8(static_cast<uint8_t>(ev.kind));
    w.u8(ev.flags);
    w.u16(ev.x);
    w.u16(ev.y);
    w.u32(ev.data);
    return out;
}

bool decode_input(const uint8_t* data, size_t size, InputEvent& out, std::string& err) {
    BufReader r(data, size);
    const uint8_t kind = r.u8();
    out.flags = r.u8();
    out.x = r.u16();
    out.y = r.u16();
    out.data = r.u32();
    if (!r.ok()) {
        err = "malformed input event";
        return false;
    }
    if (kind > static_cast<uint8_t>(InputKind::PointerLeave)) {
        err = "unknown input kind";
        return false;
    }
    out.kind = static_cast<InputKind>(kind);
    return true;
}

}  // namespace widi
