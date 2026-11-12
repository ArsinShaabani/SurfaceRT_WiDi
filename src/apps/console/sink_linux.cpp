// SurfaceRT-WiDi - reference sink for developers (Linux/macOS).
// It runs the exact SinkEngine loop the Windows RT app uses and dumps the
// received pictures to disk, which makes the wire format debuggable without
// touching the tablet.
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "widi/clock.h"
#include "widi/log.h"
#include "widi/sink_engine.h"

namespace {

struct Options {
    uint16_t port = widi::kDefaultTcpPort;
    uint16_t discoveryPort = widi::kDefaultDiscoveryPort;
    std::string outDir = ".";
    std::string name = "SurfaceRT-WiDi (Linux dev sink)";
    int stopAfterFrames = 0;  // 0 = run forever
    bool beacons = true;
    int saveEvery = 30;
};

void usage() {
    std::printf(
        "widi-sink-linux - reference wireless display sink\n"
        "  --port N          TCP port to listen on (default %u, 0 = ephemeral)\n"
        "  --discovery N     UDP discovery port (default %u)\n"
        "  --out DIR         where to save frames (default .)\n"
        "  --name NAME       advertised device name\n"
        "  --frames N        exit after N presented frames\n"
        "  --save-every N    write every Nth frame to disk (default 30, 0 = never)\n"
        "  --no-beacon       do not broadcast discovery beacons\n",
        static_cast<unsigned>(widi::kDefaultTcpPort),
        static_cast<unsigned>(widi::kDefaultDiscoveryPort));
}

bool write_ppm(const std::string& path, const widi::FrameInfo& info, const uint8_t* bgra) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", static_cast<unsigned>(info.width),
                 static_cast<unsigned>(info.height));
    std::vector<uint8_t> rgb(static_cast<size_t>(info.width) * 3);
    for (uint16_t y = 0; y < info.height; ++y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * info.width * 4;
        for (uint16_t x = 0; x < info.width; ++x) {
            rgb[x * 3 + 0] = row[x * 4 + 2];
            rgb[x * 3 + 1] = row[x * 4 + 1];
            rgb[x * 3 + 2] = row[x * 4 + 0];
        }
        std::fwrite(rgb.data(), 1, rgb.size(), f);
    }
    std::fclose(f);
    return true;
}

class ConsoleSink : public widi::ISinkHost, public widi::IFrameConsumer {
public:
    explicit ConsoleSink(const Options& opt) : opt_(opt) {}

    widi::IFrameConsumer& frames() override { return *this; }

    bool consume(const widi::FrameInfo& info, const uint8_t* data, size_t size) override {
        ++seen_;
        if (opt_.saveEvery > 0 && (seen_ - 1) % static_cast<unsigned>(opt_.saveEvery) == 0) {
            char path[512];
            const char* ext = info.format == widi::PixelFormat::Bgra8 ? "ppm" : "jpg";
            std::snprintf(path, sizeof(path), "%s/frame_%05u.%s", opt_.outDir.c_str(), seen_,
                          ext);
            if (info.format == widi::PixelFormat::Bgra8) {
                const size_t need = static_cast<size_t>(info.width) * info.height * 4;
                if (size < need) return false;
                write_ppm(path, info, data);
            } else {
                std::FILE* f = std::fopen(path, "wb");
                if (f) {
                    std::fwrite(data, 1, size, f);
                    std::fclose(f);
                }
            }
        }
        return true;
    }

    void on_client_changed(bool connected, const std::string& who) override {
        std::printf("[session] %s %s\n", connected ? "connected:" : "disconnected:",
                    who.c_str());
    }

    void on_remote_input(const widi::InputEvent& ev) override {
        std::printf("[input] kind=%u flags=0x%02x x=%u y=%u data=%u\n",
                    static_cast<unsigned>(ev.kind), ev.flags, ev.x, ev.y, ev.data);
    }

    void on_rate_changed(const widi::RateSettings& s) override {
        std::printf("[rate] quality=%u fps=%u scale=%u%%\n", s.quality, s.fps, s.scalePercent);
    }

    unsigned seen() const { return seen_; }

private:
    Options opt_;
    unsigned seen_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = (i + 1) < argc;
        if (arg == "--port" && hasNext) {
            opt.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--discovery" && hasNext) {
            opt.discoveryPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--out" && hasNext) {
            opt.outDir = argv[++i];
        } else if (arg == "--name" && hasNext) {
            opt.name = argv[++i];
        } else if (arg == "--frames" && hasNext) {
            opt.stopAfterFrames = std::atoi(argv[++i]);
        } else if (arg == "--save-every" && hasNext) {
            opt.saveEvery = std::atoi(argv[++i]);
        } else if (arg == "--no-beacon") {
            opt.beacons = false;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }

    widi::set_log_level(widi::LogLevel::Info);
    ConsoleSink host(opt);
    widi::SinkEngine engine(host);

    widi::SinkConfig cfg;
    cfg.tcpPort = opt.port;
    cfg.discoveryPort = opt.discoveryPort;
    cfg.name = opt.name;
    cfg.broadcastBeacons = opt.beacons;
    std::string err;
    if (!engine.start(cfg, &err)) {
        std::fprintf(stderr, "cannot start sink: %s\n", err.c_str());
        return 1;
    }
    std::printf("listening on tcp/%u (discovery udp/%u), saving every %d frame(s) to %s\n",
                engine.config().tcpPort, cfg.discoveryPort, opt.saveEvery, opt.outDir.c_str());
    std::printf("connect with: widi-sender-linux --host 127.0.0.1 --port %u\n",
                engine.config().tcpPort);

    uint32_t lastReport = widi::now_ms();
    while (true) {
        engine.pump(50);
        if (opt.stopAfterFrames > 0 &&
            host.seen() >= static_cast<unsigned>(opt.stopAfterFrames)) {
            break;
        }
        const uint32_t now = widi::now_ms();
        if (now - lastReport >= 5000) {
            lastReport = now;
            const widi::SinkStats& s = engine.stats();
            std::printf("[stats] state=%d presented=%u dropped=%u decode=%ums fps=%u peer=%s\n",
                        static_cast<int>(engine.state()), s.presented, s.dropped, s.decodeMs,
                        s.presentedFps, s.peer.c_str());
        }
    }
    engine.stop();
    std::printf("done: %u frame(s) seen\n", host.seen());
    return 0;
}
