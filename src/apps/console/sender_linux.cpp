// SurfaceRT-WiDi - synthetic sender used to validate the sink without Windows.
// Sends a moving test pattern as raw BGRA frames and prints the sink's feedback.
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "widi/beacon.h"
#include "widi/clock.h"
#include "widi/log.h"
#include "widi/net.h"
#include "widi/protocol.h"

namespace {

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = widi::kDefaultTcpPort;
    uint16_t width = 640;
    uint16_t height = 360;
    int fps = 20;
    int frames = 60;
    bool discover = false;
};

void usage() {
    std::printf(
        "widi-sender-linux - synthetic sender / discovery tool\n"
        "  --host IP        sink address (default 127.0.0.1)\n"
        "  --port N         sink TCP port\n"
        "  --width N        frame width (default 640)\n"
        "  --height N       frame height (default 360)\n"
        "  --fps N          target frame rate (default 20)\n"
        "  --frames N       how many frames to send (default 60)\n"
        "  --discover       listen for UDP beacons and print sinks, then exit\n");
}

void fill_pattern(std::vector<uint8_t>& bgra, uint16_t w, uint16_t h, int frame) {
    const int barX = (frame * 7) % (w > 0 ? w : 1);
    for (uint16_t y = 0; y < h; ++y) {
        uint8_t* row = bgra.data() + static_cast<size_t>(y) * w * 4;
        for (uint16_t x = 0; x < w; ++x) {
            const bool bar = (x / 16) % 2 == 0;
            const bool marker = (x >= barX && x < barX + 12);
            row[x * 4 + 0] = marker ? 0xFF : static_cast<uint8_t>(bar ? 0x20 : 0x60);
            row[x * 4 + 1] = marker ? 0xFF : static_cast<uint8_t>((x + frame) & 0xFF);
            row[x * 4 + 2] = marker ? 0xFF : static_cast<uint8_t>((y + frame * 3) & 0xFF);
            row[x * 4 + 3] = 0xFF;
        }
    }
}

int run_discovery() {
    widi::UdpSocket sock;
    std::string err;
    if (!sock.open(widi::kDefaultDiscoveryPort, true, &err)) {
        std::fprintf(stderr, "cannot bind udp/%u: %s\n", widi::kDefaultDiscoveryPort,
                     err.c_str());
        return 1;
    }
    std::printf("listening for beacons on udp/%u for 5s...\n", widi::kDefaultDiscoveryPort);
    const uint32_t deadline = widi::now_ms() + 5000;
    int found = 0;
    while (static_cast<int32_t>(deadline - widi::now_ms()) > 0) {
        uint8_t buf[1024];
        std::string from;
        uint16_t fromPort = 0;
        const int n = sock.recv_from(buf, sizeof(buf), &from, &fromPort, 250);
        if (n <= 0) continue;
        widi::BeaconInfo info;
        std::string derr;
        if (!widi::decode_beacon(buf, static_cast<size_t>(n), info, derr)) continue;
        std::printf("found sink '%s' at %s tcp/%u (%ux%u)\n", info.name.c_str(), from.c_str(),
                    info.tcpPort, info.screenWidth, info.screenHeight);
        ++found;
    }
    std::printf("%d beacon(s) decoded\n", found);
    return 0;
}

}  // namespace

#include <unistd.h>

namespace {

class SenderLink {
public:
    explicit SenderLink(widi::IConnection& conn) : conn_(conn) {}

    bool send_hello(const widi::HelloInfo& info) {
        const std::vector<uint8_t> payload = widi::encode_hello(info);
        const std::vector<uint8_t> msg = widi::encode_message(
            widi::MsgType::Hello, 0, next_seq(), payload.data(), payload.size());
        return conn_.send_all(msg.data(), msg.size());
    }

    bool send_frame(const widi::FrameInfo& info, const uint8_t* pixels, size_t size) {
        std::vector<uint8_t> payload;
        payload.reserve(widi::kFrameHeaderSize + size);
        widi::append_frame_header(payload, info);
        payload.insert(payload.end(), pixels, pixels + size);
        const std::vector<uint8_t> msg = widi::encode_message(
            widi::MsgType::Frame, 0, next_seq(), payload.data(), payload.size());
        return conn_.send_all(msg.data(), msg.size());
    }

    bool send_bye() {
        const uint8_t reason = widi::kReasonNormal;
        const std::vector<uint8_t> msg =
            widi::encode_message(widi::MsgType::Bye, 0, next_seq(), &reason, 1);
        return conn_.send_all(msg.data(), msg.size());
    }

    // Non-blocking read of sink feedback.
    void poll() {
        uint8_t chunk[4096];
        for (int i = 0; i < 4; ++i) {
            const int n = conn_.recv_some(chunk, sizeof(chunk));
            if (n < 0) {
                closed_ = true;
                return;
            }
            if (n == 0) break;
            buffer_.insert(buffer_.end(), chunk, chunk + n);
        }
        size_t off = 0;
        while (buffer_.size() - off >= widi::kHeaderSize) {
            widi::Header h;
            std::string err;
            if (!widi::decode_header(buffer_.data() + off, buffer_.size() - off, h, err)) {
                ++off;
                continue;
            }
            if (buffer_.size() - off < widi::kHeaderSize + h.length) break;
            const uint8_t* payload = buffer_.data() + off + widi::kHeaderSize;
            handle(h, payload, h.length);
            off += widi::kHeaderSize + h.length;
        }
        if (off) buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(off));
    }

    bool closed() const { return closed_; }
    bool has_stats() const { return got_stats_; }
    const widi::StatsInfo& stats() const { return stats_; }
    std::string sink_name() const { return sink_name_; }
    bool accepted() const { return accepted_; }
    uint8_t reason() const { return reason_; }

private:
    uint32_t next_seq() { return ++seq_; }

    void handle(const widi::Header& h, const uint8_t* payload, size_t size) {
        std::string err;
        switch (h.type) {
            case widi::MsgType::Stats:
                if (widi::decode_stats(payload, size, stats_, err)) got_stats_ = true;
                break;
            case widi::MsgType::HelloAck: {
                widi::HelloAckInfo ack;
                if (widi::decode_hello_ack(payload, size, ack, err)) {
                    accepted_ = ack.flags == 1;
                    reason_ = ack.reason;
                    sink_name_ = ack.name;
                }
                break;
            }
            case widi::MsgType::Bye:
                closed_ = true;
                break;
            case widi::MsgType::Ping:
                conn_.send_all(payload, 0);
                break;
            default:
                break;
        }
    }

    widi::IConnection& conn_;
    std::vector<uint8_t> buffer_;
    uint32_t seq_ = 0;
    widi::StatsInfo stats_;
    bool got_stats_ = false;
    bool closed_ = false;
    bool accepted_ = false;
    uint8_t reason_ = 0;
    std::string sink_name_;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = (i + 1) < argc;
        if (arg == "--host" && hasNext) {
            opt.host = argv[++i];
        } else if (arg == "--port" && hasNext) {
            opt.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--width" && hasNext) {
            opt.width = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--height" && hasNext) {
            opt.height = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--fps" && hasNext) {
            opt.fps = std::atoi(argv[++i]);
        } else if (arg == "--frames" && hasNext) {
            opt.frames = std::atoi(argv[++i]);
        } else if (arg == "--discover") {
            opt.discover = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }

    if (opt.discover) return run_discovery();

    widi::set_log_level(widi::LogLevel::Info);
    std::string err;
    if (!widi::net_init(&err)) {
        std::fprintf(stderr, "net init failed: %s\n", err.c_str());
        return 1;
    }
    std::unique_ptr<widi::IConnection> conn =
        widi::connect_tcp(opt.host, opt.port, 3000, &err);
    if (!conn) {
        std::fprintf(stderr, "cannot connect to %s:%u (%s)\n", opt.host.c_str(), opt.port,
                     err.c_str());
        return 1;
    }
    std::printf("connected to %s:%u\n", opt.host.c_str(), opt.port);

    SenderLink link(*conn);
    widi::HelloInfo hello;
    hello.maxWidth = opt.width;
    hello.maxHeight = opt.height;
    hello.maxFps = static_cast<uint8_t>(opt.fps > 255 ? 255 : opt.fps);
    hello.capabilities = widi::kCapBgra8;
    hello.name = "LINUX-DEV-SENDER";
    if (!link.send_hello(hello)) {
        std::fprintf(stderr, "handshake send failed\n");
        return 1;
    }
    for (int i = 0; i < 10 && !link.accepted() && !link.closed(); ++i) {
        link.poll();
        usleep(20000);
    }
    if (!link.accepted()) {
        std::fprintf(stderr, "sink rejected the session (reason %u)\n", link.reason());
        return 1;
    }
    std::printf("session accepted by '%s'\n", link.sink_name().c_str());

    std::vector<uint8_t> pixels(static_cast<size_t>(opt.width) * opt.height * 4);
    const int frameIntervalMs = opt.fps > 0 ? 1000 / opt.fps : 50;
    uint32_t nextFrame = widi::now_ms();
    int sent = 0;
    int lastPrinted = 0;

    for (int f = 0; f < opt.frames && !link.closed(); ++f) {
        fill_pattern(pixels, opt.width, opt.height, f);
        widi::FrameInfo info;
        info.width = opt.width;
        info.height = opt.height;
        info.format = widi::PixelFormat::Bgra8;
        info.timestampMs = widi::now_ms();
        if (!link.send_frame(info, pixels.data(), pixels.size())) break;
        ++sent;

        if (f % 5 == 0) link.poll();
        if (link.has_stats() && sent - lastPrinted >= 20) {
            lastPrinted = sent;
            std::printf("[sink] decode=%ums fps=%u presented=%u dropped=%u\n",
                        link.stats().decodeMsQ8 / 16, link.stats().clientFpsQ8 / 16,
                        link.stats().decoded, link.stats().dropped);
        }

        nextFrame += static_cast<uint32_t>(frameIntervalMs);
        const int32_t wait = static_cast<int32_t>(nextFrame - widi::now_ms());
        if (wait > 0) usleep(static_cast<useconds_t>(wait) * 1000u);
    }

    link.poll();
    link.send_bye();
    conn->close();
    std::printf("sent %d frame(s), %ux%u, %zu bytes each\n", sent, opt.width, opt.height,
                pixels.size());
    return sent == opt.frames ? 0 : 1;
}
