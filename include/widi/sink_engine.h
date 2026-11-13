// SurfaceRT-WiDi - sink engine: listener + session + beacon + latest-frame pacing.
// This is the single code path used by the Windows RT app, the Windows service
// host and the Linux dev harness, so behaviour is identical everywhere.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "widi/beacon.h"
#include "widi/net.h"
#include "widi/protocol.h"
#include "widi/rate_control.h"
#include "widi/session.h"

namespace widi {

// Decode + present one picture. Returns false when the frame could not be shown
// (the engine then counts it as dropped and reports that to the sender).
class IFrameConsumer {
public:
    virtual ~IFrameConsumer() {}
    virtual bool consume(const FrameInfo& info, const uint8_t* data, size_t size) = 0;
    // Advisory hint for diagnostics / future adaptive decoding.
    virtual void on_rate_changed(const RateSettings& settings) { (void)settings; }
};

class ISinkHost {
public:
    virtual ~ISinkHost() {}
    virtual IFrameConsumer& frames() = 0;
    virtual void on_client_changed(bool connected, const std::string& who) {
        (void)connected;
        (void)who;
    }
    // Input arriving from the PC (used by the RT app for a HUD indicator only;
    // the Windows sender injects it locally after receiving it).
    virtual void on_remote_input(const InputEvent& ev) { (void)ev; }
};

struct SinkConfig {
    uint16_t tcpPort = kDefaultTcpPort;
    uint16_t discoveryPort = kDefaultDiscoveryPort;
    std::string name = kServiceName;
    uint16_t screenWidth = 1366;
    uint16_t screenHeight = 768;
    uint8_t capabilities = static_cast<uint8_t>(kCapJpeg | kCapBgra8);
    bool broadcastBeacons = true;
    int beaconIntervalMs = 2000;
    int statsIntervalMs = 500;
};

// Run statistics, exposed for the on-screen HUD and the log.
struct SinkStats {
    uint32_t presented = 0;
    uint32_t dropped = 0;
    uint32_t decodeErrors = 0;
    uint16_t decodeMs = 0;
    uint8_t presentedFps = 0;
    RateSettings rate;
    std::string peer;
};

class SinkEngine : public IMessageSink, public ISessionObserver {
public:
    SinkEngine(ISinkHost& host);
    ~SinkEngine() override;

    bool start(const SinkConfig& config, std::string* err);
    void stop();
    bool running() const { return listening_; }

    // Drives I/O. `timeoutMs` is the maximum time to block while idle.
    void pump(int timeoutMs);

    // Sink -> sender input forwarding (touch/mouse captured on the tablet).
    bool send_input(const InputEvent& ev);
    bool connected() const { return connection_.get() != nullptr; }
    const SinkStats& stats() const { return stats_; }
    const SinkConfig& config() const { return config_; }
    SessionState state() const;

    // IMessageSink
    bool send_message(MsgType type, uint16_t flags, uint32_t seq,
                      const uint8_t* payload, size_t size) override;

    // ISessionObserver
    void on_frame(const FrameInfo& info, const uint8_t* data, size_t size) override;
    void on_input(const InputEvent& ev) override;
    void on_peer_gone(uint8_t reason) override;
    void on_ready(const HelloInfo& peer) override;

private:
    void accept_if_pending(int timeoutMs);
    void close_connection(uint8_t reason);
    void drain_and_present();
    void maybe_send_stats(uint32_t nowMs);
    void maybe_send_beacon(uint32_t nowMs);
    void record_present(uint32_t decodeMs);

    ISinkHost& host_;
    SinkConfig config_;
    TcpListener listener_;
    UdpSocket beacon_;
    std::unique_ptr<IConnection> connection_;
    std::unique_ptr<SinkSession> session_;
    bool listening_ = false;
    std::string peerLabel_;
    // A peer-initiated teardown is recorded here and acted upon by pump(), never
    // from inside the session callback (that would re-enter the parser).
    bool closeRequested_ = false;
    uint8_t closeReason_ = kReasonNormal;

    std::vector<uint8_t> pending_;
    FrameInfo pendingInfo_;
    bool hasPending_ = false;

    RateController rate_;
    SinkStats stats_;
    uint32_t lastBeaconMs_ = 0;
    uint32_t lastStatsMs_ = 0;
    uint32_t windowStartMs_ = 0;
    uint32_t windowPresented_ = 0;
    uint32_t windowDecodeMsSum_ = 0;
    uint32_t totalDecodeMsSum_ = 0;
};

}  // namespace widi
