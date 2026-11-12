// SurfaceRT-WiDi - unit tests for the portable core.
// Build & run: make test
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "widi/beacon.h"
#include "widi/net.h"
#include "widi/protocol.h"
#include "widi/rate_control.h"
#include "widi/session.h"
#include "widi/sink_engine.h"

using namespace widi;

namespace {

int g_checks = 0;
int g_failures = 0;

void report(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d  %s\n", file, line, expr);
    }
}

#define CHECK(expr) report((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                      \
    do {                                                                    \
        const long long va = static_cast<long long>(a);                      \
        const long long vb = static_cast<long long>(b);                      \
        ++g_checks;                                                          \
        if (va != vb) {                                                      \
            ++g_failures;                                                    \
            std::printf("  FAIL %s:%d  %s == %s (%lld vs %lld)\n", __FILE__, \
                        __LINE__, #a, #b, va, vb);                          \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// Recording stubs
// ---------------------------------------------------------------------------

struct SentMessage {
    MsgType type;
    uint16_t flags;
    uint32_t seq;
    std::vector<uint8_t> payload;
};

class RecordingSink : public IMessageSink {
public:
    bool send_message(MsgType type, uint16_t flags, uint32_t seq,
                      const uint8_t* payload, size_t size) override {
        SentMessage m;
        m.type = type;
        m.flags = flags;
        m.seq = seq;
        m.payload.assign(payload, payload + size);
        sent.push_back(m);
        return !fail_next;
    }
    std::vector<SentMessage> sent;
    bool fail_next = false;
};

class RecordingObserver : public ISessionObserver {
public:
    void on_frame(const FrameInfo& info, const uint8_t* data, size_t size) override {
        frames.push_back(info);
        payloads.push_back(std::vector<uint8_t>(data, data + size));
    }
    void on_input(const InputEvent& ev) override { inputs.push_back(ev); }
    void on_peer_gone(uint8_t reason) override { goodbye = reason; }
    void on_ready(const HelloInfo& peer) override {
        ready = true;
        peerName = peer.name;
    }
    std::vector<FrameInfo> frames;
    std::vector<std::vector<uint8_t> > payloads;
    std::vector<InputEvent> inputs;
    bool ready = false;
    int goodbye = -1;
    std::string peerName;
};

std::vector<uint8_t> make_hello_bytes(uint16_t version = kProtocolVersion) {
    HelloInfo info;
    info.protocolVersion = version;
    info.maxWidth = 1920;
    info.maxHeight = 1080;
    info.maxFps = 30;
    info.capabilities = kCapJpeg | kCapInput;
    info.name = "DESKTOP-TEST";
    const std::vector<uint8_t> payload = encode_hello(info);
    return encode_message(MsgType::Hello, 0, 1, payload.data(), payload.size());
}

std::vector<uint8_t> make_frame_bytes(uint16_t w, uint16_t h, size_t pixelBytes,
                                      uint8_t seed) {
    FrameInfo info;
    info.width = w;
    info.height = h;
    info.format = PixelFormat::Bgra8;
    info.timestampMs = 1234;
    std::vector<uint8_t> payload;
    append_frame_header(payload, info);
    for (size_t i = 0; i < pixelBytes; ++i) {
        payload.push_back(static_cast<uint8_t>((i * 31u + seed) & 0xFFu));
    }
    return encode_message(MsgType::Frame, 0, 7, payload.data(), payload.size());
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Codec tests
// ---------------------------------------------------------------------------

void test_crc32() {
    const char* known = "123456789";
    CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>(known), 9), 0xCBF43926u);
    CHECK_EQ(crc32(nullptr, 0), 0u);
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 5};
    CHECK(crc32(a, 4) != crc32(b, 4));
}

void test_message_roundtrip() {
    // 16 bytes: a plausible Frame payload (>= 10-byte frame header + pixels).
    const std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x42,
                                          0x10, 0x20, 0x30, 0x40, 0x50,
                                          0x60, 0x70, 0x80, 0x90, 0xA0};
    const std::vector<uint8_t> bytes =
        encode_message(MsgType::Frame, 0x1234, 99, payload.data(), payload.size());
    CHECK_EQ(bytes.size(), kHeaderSize + payload.size());

    Header h;
    std::string err;
    CHECK(decode_header(bytes.data(), bytes.size(), h, err));
    CHECK(h.type == MsgType::Frame);
    CHECK_EQ(h.flags, 0x1234);
    CHECK_EQ(h.seq, 99);
    CHECK_EQ(h.length, payload.size());
    CHECK(std::memcmp(bytes.data() + kHeaderSize, payload.data(), payload.size()) == 0);
    CHECK_EQ(crc32(bytes.data() + kHeaderSize, h.length), h.crc);
}

void test_header_rejects() {
    Header h;
    std::string err;

    const uint8_t tiny[4] = {0, 0, 0, 0};
    CHECK(!decode_header(tiny, sizeof(tiny), h, err));

    std::vector<uint8_t> bytes = encode_message(MsgType::Ping, 0, 0, nullptr, 0);
    bytes[0] = 0x00;
    CHECK(!decode_header(bytes.data(), bytes.size(), h, err));
    CHECK(err == "bad magic");

    bytes = encode_message(MsgType::Ping, 0, 0, nullptr, 0);
    bytes[4] = 99;
    CHECK(!decode_header(bytes.data(), bytes.size(), h, err));

    bytes = encode_message(MsgType::Ping, 0, 0, nullptr, 0);
    bytes[5] = 0x7F;
    CHECK(!decode_header(bytes.data(), bytes.size(), h, err));

    // Type/length mismatch: a Stats message must carry exactly 12 bytes.
    bytes = encode_message(MsgType::Stats, 0, 0, nullptr, 0);
    CHECK(!decode_header(bytes.data(), bytes.size(), h, err));

    bytes = encode_message(MsgType::Frame, 0, 0, nullptr, 0);
    const uint32_t tooBig = static_cast<uint32_t>(kMaxPayload + 1);
    for (int i = 0; i < 4; ++i) {
        bytes[12 + i] = static_cast<uint8_t>((tooBig >> (8 * i)) & 0xFF);
    }
    CHECK(!decode_header(bytes.data(), bytes.size(), h, err));
}

void test_payload_codecs() {
    std::string err;

    HelloInfo hello;
    hello.maxWidth = 1366;
    hello.maxHeight = 768;
    hello.maxFps = 24;
    hello.capabilities = kCapJpeg;
    hello.name = "LAPTOP-7";
    const std::vector<uint8_t> hb = encode_hello(hello);
    HelloInfo helloOut;
    CHECK(decode_hello(hb.data(), hb.size(), helloOut, err));
    CHECK_EQ(helloOut.maxWidth, 1366);
    CHECK_EQ(helloOut.maxHeight, 768);
    CHECK_EQ(helloOut.maxFps, 24);
    CHECK(helloOut.name == "LAPTOP-7");

    HelloInfo bad = hello;
    bad.protocolVersion = 2;
    const std::vector<uint8_t> badBytes = encode_hello(bad);
    CHECK(!decode_hello(badBytes.data(), badBytes.size(), helloOut, err));

    FrameInfo frame;
    frame.width = 1280;
    frame.height = 800;
    frame.format = PixelFormat::Jpeg;
    frame.timestampMs = 4242;
    std::vector<uint8_t> fp;
    append_frame_header(fp, frame);
    fp.push_back(0xFF);
    FrameInfo frameOut;
    CHECK(decode_frame_header(fp.data(), fp.size(), frameOut, err));
    CHECK_EQ(frameOut.width, 1280);
    CHECK_EQ(frameOut.height, 800);
    CHECK(frameOut.format == PixelFormat::Jpeg);
    CHECK_EQ(frameOut.timestampMs, 4242);

    std::vector<uint8_t> empty;
    append_frame_header(empty, frame);
    CHECK(!decode_frame_header(empty.data(), empty.size(), frameOut, err));

    StatsInfo stats;
    stats.decoded = 1000;
    stats.dropped = 7;
    stats.decodeMsQ8 = 220;
    stats.clientFpsQ8 = 320;
    const std::vector<uint8_t> sb = encode_stats(stats);
    StatsInfo statsOut;
    CHECK(decode_stats(sb.data(), sb.size(), statsOut, err));
    CHECK_EQ(statsOut.decoded, 1000);
    CHECK_EQ(statsOut.dropped, 7);
    CHECK_EQ(statsOut.decodeMsQ8, 220);
    CHECK_EQ(statsOut.clientFpsQ8, 320);

    InputEvent ev;
    ev.kind = InputKind::Wheel;
    ev.flags = kInputButtonLeft;
    ev.x = 32768;
    ev.y = 65535;
    ev.data = 0xFFFFFF88u;  // -120 as u32
    const std::vector<uint8_t> ib = encode_input(ev);
    InputEvent evOut;
    CHECK(decode_input(ib.data(), ib.size(), evOut, err));
    CHECK(evOut.kind == InputKind::Wheel);
    CHECK_EQ(evOut.flags, kInputButtonLeft);
    CHECK_EQ(evOut.x, 32768);
    CHECK_EQ(evOut.y, 65535);
    CHECK_EQ(evOut.data, 0xFFFFFF88u);

    std::vector<uint8_t> bogus = ib;
    bogus[0] = 99;
    CHECK(!decode_input(bogus.data(), bogus.size(), evOut, err));
}

void test_beacon() {
    BeaconInfo info;
    info.tcpPort = kDefaultTcpPort;
    info.screenWidth = 1366;
    info.screenHeight = 768;
    info.capabilities = static_cast<uint8_t>(kCapJpeg | kCapTouch);
    info.name = "SURFACE-RT";
    const std::vector<uint8_t> bytes = encode_beacon(info);
    BeaconInfo out;
    std::string err;
    CHECK(decode_beacon(bytes.data(), bytes.size(), out, err));
    CHECK_EQ(out.tcpPort, kDefaultTcpPort);
    CHECK_EQ(out.screenWidth, 1366);
    CHECK_EQ(out.screenHeight, 768);
    CHECK(out.name == "SURFACE-RT");

    std::vector<uint8_t> junk = bytes;
    junk[0] = 'X';
    CHECK(!decode_beacon(junk.data(), junk.size(), out, err));
    CHECK(!decode_beacon(bytes.data(), 3, out, err));
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Session tests
// ---------------------------------------------------------------------------

void test_session_handshake() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    HelloAckInfo id;
    id.screenWidth = 1366;
    id.screenHeight = 768;
    id.sessionId = 0xCAFE;
    id.name = "SURFACE-RT";
    session.set_identity(id);

    CHECK(session.state() == SessionState::Idle);
    const std::vector<uint8_t> hello = make_hello_bytes();
    session.feed(hello.data(), hello.size());

    CHECK(session.state() == SessionState::Streaming);
    CHECK(obs.ready);
    CHECK(obs.peerName == "DESKTOP-TEST");
    CHECK_EQ(out.sent.size(), 1u);
    if (!out.sent.empty()) {
        CHECK(out.sent[0].type == MsgType::HelloAck);
        HelloAckInfo ack;
        std::string err;
        CHECK(decode_hello_ack(out.sent[0].payload.data(), out.sent[0].payload.size(), ack, err));
        CHECK_EQ(ack.flags, 1);
        CHECK_EQ(ack.screenWidth, 1366);
        CHECK_EQ(ack.sessionId, 0xCAFE);
        CHECK(ack.name == "SURFACE-RT");
    }
}

void test_session_rejects_wrong_version() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    const std::vector<uint8_t> hello = make_hello_bytes(7);
    session.feed(hello.data(), hello.size());
    CHECK(session.state() == SessionState::Closed);
    CHECK(!obs.ready);
    CHECK(!out.sent.empty());
    if (!out.sent.empty()) {
        CHECK(out.sent[0].type == MsgType::HelloAck);
        CHECK_EQ(out.sent[0].payload[10], 0);  // flags = rejected
    }
}

void test_session_frame_delivery() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    const std::vector<uint8_t> hello = make_hello_bytes();
    session.feed(hello.data(), hello.size());

    const std::vector<uint8_t> frame = make_frame_bytes(64, 32, 64 * 32 * 4, 3);
    session.feed(frame.data(), frame.size());

    CHECK_EQ(obs.frames.size(), 1u);
    CHECK_EQ(obs.payloads.size(), 1u);
    CHECK_EQ(obs.frames[0].width, 64);
    CHECK_EQ(obs.frames[0].height, 32);
    CHECK(obs.frames[0].format == PixelFormat::Bgra8);
    CHECK_EQ(obs.payloads[0].size(), 64u * 32u * 4u);
    CHECK_EQ(obs.payloads[0][100], static_cast<uint8_t>((100u * 31u + 3u) & 0xFFu));
    CHECK_EQ(session.frames_received(), 1u);
}

void test_session_trickle_feed() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    const std::vector<uint8_t> hello = make_hello_bytes();
    for (size_t i = 0; i < hello.size(); ++i) session.feed(&hello[i], 1);
    CHECK(session.state() == SessionState::Streaming);

    const std::vector<uint8_t> frame = make_frame_bytes(8, 8, 256, 9);
    for (size_t i = 0; i < frame.size(); ++i) session.feed(&frame[i], 1);
    CHECK_EQ(obs.frames.size(), 1u);
    CHECK_EQ(obs.payloads[0].size(), 256u);
    CHECK_EQ(session.buffered(), 0u);
}

void test_session_resync_after_garbage() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    const std::vector<uint8_t> hello = make_hello_bytes();
    session.feed(hello.data(), hello.size());

    const uint8_t garbage[37] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    session.feed(garbage, sizeof(garbage));
    CHECK(session.protocol_errors() > 0);

    const std::vector<uint8_t> frame = make_frame_bytes(4, 4, 64, 1);
    session.feed(frame.data(), frame.size());
    CHECK_EQ(obs.frames.size(), 1u);
}

void test_session_drops_corrupt_frame() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    const std::vector<uint8_t> hello = make_hello_bytes();
    session.feed(hello.data(), hello.size());

    std::vector<uint8_t> frame = make_frame_bytes(4, 4, 64, 1);
    frame[kHeaderSize + kFrameHeaderSize + 3] ^= 0xFF;  // flip a pixel byte
    session.feed(frame.data(), frame.size());
    CHECK_EQ(obs.frames.size(), 0u);

    const std::vector<uint8_t> good = make_frame_bytes(4, 4, 64, 2);
    session.feed(good.data(), good.size());
    CHECK_EQ(obs.frames.size(), 1u);
}

void test_session_input_ping_bye() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    const std::vector<uint8_t> hello = make_hello_bytes();
    session.feed(hello.data(), hello.size());

    InputEvent ev;
    ev.kind = InputKind::PointerMove;
    ev.x = 1000;
    ev.y = 2000;
    const std::vector<uint8_t> in = encode_input(ev);
    const std::vector<uint8_t> inMsg =
        encode_message(MsgType::Input, 0, 2, in.data(), in.size());
    session.feed(inMsg.data(), inMsg.size());
    CHECK_EQ(obs.inputs.size(), 1u);
    CHECK_EQ(obs.inputs[0].x, 1000);

    const std::vector<uint8_t> ping = encode_message(MsgType::Ping, 0, 42, nullptr, 0);
    session.feed(ping.data(), ping.size());
    CHECK_EQ(out.sent.size(), 2u);
    if (out.sent.size() >= 2) {
        CHECK(out.sent.back().type == MsgType::Pong);
        CHECK_EQ(out.sent.back().seq, 42u);
    }

    CHECK(session.send_stats(StatsInfo()));
    CHECK(session.send_input(ev));

    const uint8_t goodbyeBody = kReasonNormal;
    const std::vector<uint8_t> bye =
        encode_message(MsgType::Bye, 0, 5, &goodbyeBody, 1);
    session.feed(bye.data(), bye.size());
    CHECK(session.state() == SessionState::Closed);
    CHECK_EQ(obs.goodbye, kReasonNormal);
}

void test_session_frame_before_handshake() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    const std::vector<uint8_t> frame = make_frame_bytes(4, 4, 64, 1);
    session.feed(frame.data(), frame.size());
    CHECK_EQ(obs.frames.size(), 0u);
    CHECK(session.protocol_errors() > 0);
    // A later handshake must still succeed (sender restarted quickly).
    const std::vector<uint8_t> hello = make_hello_bytes();
    session.feed(hello.data(), hello.size());
    CHECK(session.state() == SessionState::Streaming);
}

void test_session_buffer_overflow_is_bounded() {
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    std::vector<uint8_t> noise(256 * 1024, 0xAA);
    for (int i = 0; i < 40; ++i) session.feed(noise.data(), noise.size());
    CHECK(session.buffered() <= kHeaderSize * 2);
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Rate control tests
// ---------------------------------------------------------------------------

void test_rate_control_degrades_on_overload() {
    RateController rc;
    CHECK_EQ(rc.current().quality, 60);
    CHECK_EQ(rc.current().fps, 20);
    CHECK_EQ(rc.current().scalePercent, 100);

    RateFeedback heavy;
    heavy.decodeMs = 100;  // budget at 20 fps is 35 ms
    heavy.clientFps = 9;
    rc.feedback(heavy);
    CHECK(rc.last_action() == RateAction::StepDown);
    CHECK_EQ(rc.current().fps, 16);

    rc.feedback(heavy);
    CHECK_EQ(rc.current().fps, 12);

    // Once fps is at the floor, scale is reduced before quality.
    rc.feedback(heavy);
    CHECK_EQ(rc.current().scalePercent, 90);

    for (int i = 0; i < 10; ++i) rc.feedback(heavy);
    CHECK(rc.current().scalePercent >= 50);
    CHECK(rc.current().quality >= 20);
    CHECK(rc.current().fps >= 5);
}

void test_rate_control_recovers_when_healthy() {
    RateController rc;
    RateFeedback healthy;
    healthy.decodeMs = 5;
    healthy.clientFps = 20;

    RateAction sawUp = RateAction::None;
    for (int i = 0; i < 20; ++i) {
        rc.feedback(healthy);
        if (rc.last_action() == RateAction::StepUp) sawUp = RateAction::StepUp;
    }
    CHECK(sawUp == RateAction::StepUp);
    CHECK(rc.current().quality > 60);
    CHECK_EQ(rc.current().fps, 20);
}

void test_rate_control_drops_trigger_degrade() {
    RateController rc;
    RateFeedback fb;
    fb.decodeMs = 5;
    fb.clientFps = 20;
    fb.decoded = 100;
    fb.dropped = 0;
    rc.feedback(fb);
    CHECK(rc.last_action() == RateAction::None);

    fb.decoded = 120;
    fb.dropped = 3;  // three frames were not presented
    rc.feedback(fb);
    CHECK(rc.last_action() == RateAction::StepDown);
}

void test_rate_control_budget() {
    RateController rc;
    CHECK_EQ(rc.decode_budget_ms(), 35);  // 1000/20 * 0.7
    RateSettings start;
    start.fps = 10;
    RateController slow(start);
    CHECK_EQ(slow.decode_budget_ms(), 70);
}

// ---------------------------------------------------------------------------
// Loopback integration test: real sockets, real framing, 50 frames
// ---------------------------------------------------------------------------

struct SocketMessageSink : public IMessageSink {
    IConnection* conn = nullptr;
    bool send_message(MsgType type, uint16_t flags, uint32_t seq, const uint8_t* payload,
                      size_t size) override {
        const std::vector<uint8_t> bytes = encode_message(type, flags, seq, payload, size);
        return conn->send_all(bytes.data(), bytes.size());
    }
};

struct CountingObserver : public ISessionObserver {
    void on_frame(const FrameInfo& info, const uint8_t* data, size_t size) override {
        (void)data;
        ++frames;
        bytes += size;
        lastWidth = info.width;
    }
    void on_ready(const HelloInfo& peer) override {
        ready = true;
        peerName = peer.name;
    }
    int frames = 0;
    size_t bytes = 0;
    uint16_t lastWidth = 0;
    bool ready = false;
    std::string peerName;
};

}  // namespace

#include <thread>

namespace {

void test_loopback_stream() {
    TcpListener listener;
    std::string err;
    CHECK(listener.listen(0, &err));
    CHECK(listener.port() != 0);

    const int kFrames = 50;
    const uint16_t kW = 32;
    const uint16_t kH = 24;
    const size_t kPixels = static_cast<size_t>(kW) * kH * 4;

    bool serverOk = false;
    std::thread server([&]() {
        std::string acceptErr;
        std::unique_ptr<IConnection> conn = listener.accept(3000, &acceptErr);
        if (!conn) return;
        SocketMessageSink out;
        out.conn = conn.get();
        CountingObserver obs;
        SinkSession session(out, obs);
        std::vector<uint8_t> buf(64 * 1024);
        while (obs.frames < kFrames) {
            const int n = conn->recv_some(buf.data(), buf.size());
            if (n < 0) break;
            if (n > 0) session.feed(buf.data(), static_cast<size_t>(n));
        }
        serverOk = obs.ready && obs.frames == kFrames && obs.bytes == kFrames * kPixels &&
                   obs.lastWidth == kW && obs.peerName == "LINUX-TEST";
    });

    std::string connectErr;
    std::unique_ptr<IConnection> client =
        connect_tcp("127.0.0.1", listener.port(), 2000, &connectErr);
    CHECK(client != nullptr);
    if (client) {
        HelloInfo hello;
        hello.maxWidth = kW;
        hello.maxHeight = kH;
        hello.maxFps = 30;
        hello.capabilities = kCapJpeg;
        hello.name = "LINUX-TEST";
        const std::vector<uint8_t> hp = encode_hello(hello);
        const std::vector<uint8_t> hm = encode_message(MsgType::Hello, 0, 1, hp.data(), hp.size());
        CHECK(client->send_all(hm.data(), hm.size()));

        std::vector<uint8_t> pixels(kPixels);
        for (int f = 0; f < kFrames; ++f) {
            for (size_t i = 0; i < kPixels; ++i) {
                pixels[i] = static_cast<uint8_t>((i + static_cast<size_t>(f) * 7u) & 0xFFu);
            }
            FrameInfo info;
            info.width = kW;
            info.height = kH;
            info.format = PixelFormat::Bgra8;
            info.timestampMs = static_cast<uint32_t>(f * 33);
            std::vector<uint8_t> payload;
            payload.reserve(kFrameHeaderSize + kPixels);
            append_frame_header(payload, info);
            payload.insert(payload.end(), pixels.begin(), pixels.end());
            const std::vector<uint8_t> msg =
                encode_message(MsgType::Frame, 0, static_cast<uint32_t>(10 + f),
                               payload.data(), payload.size());
            CHECK(client->send_all(msg.data(), msg.size()));
        }
        const uint8_t bye = kReasonNormal;
        const std::vector<uint8_t> byeMsg = encode_message(MsgType::Bye, 0, 999, &bye, 1);
        client->send_all(byeMsg.data(), byeMsg.size());
        client->close();
    }
    server.join();
    CHECK(serverOk);
}

}  // namespace


namespace {

// ---------------------------------------------------------------------------
// SinkEngine tests (listener + session + pacing + stats)
// ---------------------------------------------------------------------------

struct FakeConsumer : public IFrameConsumer {
    int consumed = 0;
    size_t lastBytes = 0;
    uint16_t lastWidth = 0;
    bool failAll = false;
    bool consume(const FrameInfo& info, const uint8_t* data, size_t size) override {
        (void)data;
        ++consumed;
        lastBytes = size;
        lastWidth = info.width;
        return !failAll;
    }
};

struct FakeHost : public ISinkHost {
    FakeConsumer consumer;
    bool connected = false;
    int connects = 0;
    IFrameConsumer& frames() override { return consumer; }
    void on_client_changed(bool conn, const std::string& who) override {
        (void)who;
        connected = conn;
        if (conn) ++connects;
    }
};

void test_engine_stream() {
    FakeHost host;
    SinkEngine engine(host);
    SinkConfig cfg;
    cfg.tcpPort = 0;
    cfg.broadcastBeacons = false;
    cfg.statsIntervalMs = 20;
    std::string err;
    CHECK(engine.start(cfg, &err));
    CHECK(engine.running());
    const uint16_t port = engine.config().tcpPort;
    CHECK(port != 0);

    std::unique_ptr<IConnection> client = connect_tcp("127.0.0.1", port, 1000, &err);
    CHECK(client != nullptr);
    if (!client) return;

    const std::vector<uint8_t> hello = make_hello_bytes();
    CHECK(client->send_all(hello.data(), hello.size()));
    for (int i = 0; i < 25 && !engine.connected(); ++i) engine.pump(20);
    CHECK(engine.connected());
    CHECK(host.connected);

    const int kFrames = 24;
    for (int f = 0; f < kFrames; ++f) {
        const std::vector<uint8_t> frame = make_frame_bytes(16, 8, 16 * 8 * 4, (uint8_t)f);
        CHECK(client->send_all(frame.data(), frame.size()));
    }
    for (int i = 0; i < 60; ++i) engine.pump(5);

    CHECK(host.consumer.consumed >= 1);
    CHECK(host.consumer.lastBytes == 16u * 8u * 4u);
    CHECK(host.consumer.lastWidth == 16);
    CHECK_EQ(host.consumer.consumed + static_cast<int>(engine.stats().dropped), kFrames);
    CHECK_EQ(engine.stats().decodeErrors, 0u);
    CHECK(engine.stats().presented >= 1u);

    client->close();
    for (int i = 0; i < 20 && engine.connected(); ++i) engine.pump(10);
    CHECK(!engine.connected());
    CHECK(!host.connected);
    engine.stop();
    CHECK(!engine.running());
}

void test_engine_counts_decode_failures() {
    FakeHost host;
    host.consumer.failAll = true;
    SinkEngine engine(host);
    SinkConfig cfg;
    cfg.tcpPort = 0;
    cfg.broadcastBeacons = false;
    std::string err;
    CHECK(engine.start(cfg, &err));

    std::unique_ptr<IConnection> client =
        connect_tcp("127.0.0.1", engine.config().tcpPort, 1000, &err);
    CHECK(client != nullptr);
    if (!client) return;
    const std::vector<uint8_t> hello = make_hello_bytes();
    client->send_all(hello.data(), hello.size());
    for (int i = 0; i < 25 && !engine.connected(); ++i) engine.pump(20);

    const std::vector<uint8_t> frame = make_frame_bytes(8, 8, 256, 4);
    client->send_all(frame.data(), frame.size());
    for (int i = 0; i < 20; ++i) engine.pump(5);

    CHECK(engine.stats().decodeErrors >= 1u);
    CHECK_EQ(engine.stats().presented, 0u);
    client->close();
    engine.stop();
}

}  // namespace


void test_fuzz_feed_is_memory_safe() {
    // Deterministic pseudo-random stream: the session must survive anything and
    // never buffer without bound. Run this target under -fsanitize=address too.
    RecordingSink out;
    RecordingObserver obs;
    SinkSession session(out, obs);
    const std::vector<uint8_t> hello = make_hello_bytes();
    session.feed(hello.data(), hello.size());

    uint32_t state = 0x12345678u;
    std::vector<uint8_t> chunk;
    for (int round = 0; round < 4000; ++round) {
        state = state * 1664525u + 1013904223u;
        const size_t size = 1 + (state % 512);
        chunk.resize(size);
        for (size_t i = 0; i < size; ++i) {
            state = state * 1664525u + 1013904223u;
            chunk[i] = static_cast<uint8_t>(state >> 16);
        }
        session.feed(chunk.data(), chunk.size());
        CHECK(session.buffered() <= kMaxPayload + kHeaderSize * 2);
        if (session.state() == SessionState::Closed) break;
    }

    // A valid frame after all that noise must still be accepted if the session
    // survived; if it closed, a fresh session must work.
    if (session.state() == SessionState::Closed) {
        session.reset();
        session.feed(hello.data(), hello.size());
    }
    const std::vector<uint8_t> frame = make_frame_bytes(8, 8, 256, 5);
    session.feed(frame.data(), frame.size());
    CHECK_EQ(obs.frames.size(), 1u);
}

int main() {
    struct TestCase {
        const char* name;
        void (*fn)();
    };
    const TestCase cases[] = {
        {"crc32", test_crc32},
        {"message roundtrip", test_message_roundtrip},
        {"header rejects", test_header_rejects},
        {"payload codecs", test_payload_codecs},
        {"beacon codec", test_beacon},
        {"session handshake", test_session_handshake},
        {"session rejects wrong version", test_session_rejects_wrong_version},
        {"session frame delivery", test_session_frame_delivery},
        {"session trickle feed", test_session_trickle_feed},
        {"session resync after garbage", test_session_resync_after_garbage},
        {"session drops corrupt frame", test_session_drops_corrupt_frame},
        {"session input/ping/bye", test_session_input_ping_bye},
        {"session frame before handshake", test_session_frame_before_handshake},
        {"session buffer bounded", test_session_buffer_overflow_is_bounded},
        {"rate control degrade", test_rate_control_degrades_on_overload},
        {"rate control recover", test_rate_control_recovers_when_healthy},
        {"rate control drops", test_rate_control_drops_trigger_degrade},
        {"rate control budget", test_rate_control_budget},
        {"loopback stream", test_loopback_stream},
        {"engine stream", test_engine_stream},
        {"engine decode failures", test_engine_counts_decode_failures},
        {"fuzz feed", test_fuzz_feed_is_memory_safe},
    };

    int failures = 0;
    for (const TestCase& tc : cases) {
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", tc.name);
        tc.fn();
        if (g_failures != before) {
            std::printf("[ FAIL ] %s\n", tc.name);
            ++failures;
        } else {
            std::printf("[  OK  ] %s\n", tc.name);
        }
    }
    std::printf("\n%d checks, %d failed assertions, %d failing test(s)\n", g_checks,
                g_failures, failures);
    return failures == 0 ? 0 : 1;
}
