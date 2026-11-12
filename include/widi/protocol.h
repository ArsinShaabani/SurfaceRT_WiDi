// SurfaceRT-WiDi wire protocol v1 (see docs/PROTOCOL.md).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace widi {

// "WIDI" as it appears on the wire (little-endian 0x49444957).
constexpr uint32_t kMagic = 0x49444957u;
constexpr uint8_t kProtocolVersion = 1;

// magic(4) version(1) type(1) flags(2) seq(4) length(4) crc32(4)
constexpr size_t kHeaderSize = 20;
// Hard ceiling so a hostile/broken peer cannot make us allocate without bound.
constexpr size_t kMaxPayload = 16u * 1024u * 1024u;

constexpr uint16_t kDefaultTcpPort = 47000;
constexpr uint16_t kDefaultDiscoveryPort = 47001;
constexpr char kServiceName[] = "SurfaceRT-WiDi";
constexpr char kBeaconMagic[6] = {'W', 'I', 'D', 'I', 'D', 'S'};

enum class MsgType : uint8_t {
    Beacon = 1,    // UDP discovery datagram (never sent over TCP)
    Hello = 2,     // sender -> sink, opens a session
    HelloAck = 3,  // sink -> sender, accepts/rejects
    Frame = 4,     // sender -> sink, one complete picture
    Stats = 5,     // sink -> sender, decode feedback (drives rate control)
    Input = 6,     // sink -> sender, pointer/key remoting
    Ping = 7,      // either direction
    Pong = 8,      // either direction
    Bye = 9,       // graceful teardown, payload[0] = reason
};

enum class PixelFormat : uint8_t { Bgra8 = 0, Jpeg = 1 };

enum class InputKind : uint8_t {
    PointerMove = 0,
    PointerButton = 1,
    Wheel = 2,
    Key = 3,
    PointerLeave = 4,
};

// InputEvent::flags bits.
enum : uint8_t {
    kInputButtonLeft = 0x01,
    kInputButtonRight = 0x02,
    kInputButtonMiddle = 0x04,
    kInputDown = 0x08,
    kInputTouch = 0x10,
};

// Hello::capabilities bits.
enum : uint8_t {
    kCapJpeg = 0x01,
    kCapBgra8 = 0x02,
    kCapInput = 0x04,
    kCapTouch = 0x08,
};

// Bye / HelloAck::reason codes.
enum : uint8_t {
    kReasonNormal = 0,
    kReasonProtocolMismatch = 1,
    kReasonBusy = 2,
    kReasonTimeout = 3,
    kReasonPeerClosed = 4,
    kReasonError = 5,
};

struct Header {
    MsgType type = MsgType::Ping;
    uint16_t flags = 0;
    uint32_t seq = 0;
    uint32_t length = 0;
    uint32_t crc = 0;
};

// Standard CRC-32 (reflected, poly 0xEDB88320). crc32("123456789") == 0xCBF43926.
uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed = 0);

std::vector<uint8_t> encode_message(MsgType type, uint16_t flags, uint32_t seq,
                                    const void* payload, size_t payloadSize);

bool decode_header(const uint8_t* data, size_t size, Header& out, std::string& err);

// ---------------------------------------------------------------------------
// Payload codecs
// ---------------------------------------------------------------------------

struct HelloInfo {
    uint16_t protocolVersion = kProtocolVersion;
    uint16_t maxWidth = 0;
    uint16_t maxHeight = 0;
    uint8_t maxFps = 0;
    uint8_t capabilities = 0;
    std::string name;
};
std::vector<uint8_t> encode_hello(const HelloInfo& info);
bool decode_hello(const uint8_t* data, size_t size, HelloInfo& out, std::string& err);

struct HelloAckInfo {
    uint16_t protocolVersion = kProtocolVersion;
    uint16_t screenWidth = 0;
    uint16_t screenHeight = 0;
    uint32_t sessionId = 0;
    uint8_t flags = 0;   // 1 = accepted
    uint8_t reason = kReasonNormal;
    std::string name;
};
std::vector<uint8_t> encode_hello_ack(const HelloAckInfo& info);
bool decode_hello_ack(const uint8_t* data, size_t size, HelloAckInfo& out, std::string& err);

// Frame payload = 10-byte header followed by pixels (JPEG stream or BGRA rows).
constexpr size_t kFrameHeaderSize = 10;

struct FrameInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    PixelFormat format = PixelFormat::Jpeg;
    uint8_t flags = 0;
    uint32_t timestampMs = 0;
};
// Appends the 10-byte frame header to `out` (pixels must follow it in the payload).
void append_frame_header(std::vector<uint8_t>& out, const FrameInfo& info);
bool decode_frame_header(const uint8_t* payload, size_t size, FrameInfo& out, std::string& err);

struct StatsInfo {
    uint32_t decoded = 0;
    uint32_t dropped = 0;
    uint16_t decodeMsQ8 = 0;    // average decode time, milliseconds * 16
    uint16_t clientFpsQ8 = 0;   // achieved presentation rate, fps * 16
};
std::vector<uint8_t> encode_stats(const StatsInfo& info);
bool decode_stats(const uint8_t* data, size_t size, StatsInfo& out, std::string& err);

struct InputEvent {
    InputKind kind = InputKind::PointerMove;
    uint8_t flags = 0;
    uint16_t x = 0;  // normalised 0..65535 across the virtual desktop
    uint16_t y = 0;
    uint32_t data = 0;  // wheel delta, virtual key, etc.
};
std::vector<uint8_t> encode_input(const InputEvent& ev);
bool decode_input(const uint8_t* data, size_t size, InputEvent& out, std::string& err);

}  // namespace widi
