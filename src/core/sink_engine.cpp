// SurfaceRT-WiDi - sink engine implementation.
// SPDX-License-Identifier: MIT
#include "widi/sink_engine.h"

#include <cstring>

#include "widi/clock.h"
#include "widi/log.h"

namespace widi {

SinkEngine::SinkEngine(ISinkHost& host) : host_(host) {}

SinkEngine::~SinkEngine() { stop(); }

bool SinkEngine::start(const SinkConfig& config, std::string* err) {
    stop();
    config_ = config;

    std::string localErr;
    if (!net_init(&localErr)) {
        if (err) *err = localErr;
        return false;
    }
    if (!listener_.listen(config_.tcpPort, &localErr)) {
        if (err) *err = localErr;
        return false;
    }
    // config_.tcpPort == 0 asks the OS for an ephemeral port (used by tests).
    config_.tcpPort = listener_.port();

    if (config_.broadcastBeacons) {
        if (beacon_.open(0, true, &localErr)) {
            beacon_.enable_broadcast(true, nullptr);
        } else {
            WIDI_LOGW("discovery beacon disabled: %s", localErr.c_str());
        }
    }

    session_.reset(new SinkSession(*this, *this));
    HelloAckInfo identity;
    identity.screenWidth = config_.screenWidth;
    identity.screenHeight = config_.screenHeight;
    identity.sessionId = now_ms();
    identity.name = config_.name;
    session_->set_identity(identity);

    stats_ = SinkStats();
    stats_.rate = rate_.current();
    listening_ = true;
    lastBeaconMs_ = 0;
    lastStatsMs_ = now_ms();
    windowStartMs_ = lastStatsMs_;
    windowPresented_ = 0;
    windowDecodeMsSum_ = 0;
    totalDecodeMsSum_ = 0;
    WIDI_LOGI("sink listening on tcp/%u as '%s' (%ux%u)", config_.tcpPort,
              config_.name.c_str(), config_.screenWidth, config_.screenHeight);
    return true;
}

void SinkEngine::stop() {
    if (connection_) {
        close_connection(kReasonNormal);
    }
    beacon_.close();
    listener_.close();
    session_.reset();
    listening_ = false;
}

SessionState SinkEngine::state() const {
    return session_ ? session_->state() : SessionState::Idle;
}

void SinkEngine::pump(int timeoutMs) {
    if (!listening_) return;
    maybe_send_beacon(now_ms());

    if (!connection_) {
        accept_if_pending(timeoutMs);
        return;
    }

    uint8_t buffer[64 * 1024];
    // Bounded drain: never starve presentation for longer than a few reads.
    for (int round = 0; round < 8; ++round) {
        const int n = connection_->recv_some(buffer, sizeof(buffer));
        if (n < 0) {
            close_connection(kReasonPeerClosed);
            return;
        }
        if (n == 0) break;
        session_->feed(buffer, static_cast<size_t>(n));
        if (session_->state() == SessionState::Closed) {
            close_connection(kReasonPeerClosed);
            return;
        }
    }

    drain_and_present();
    maybe_send_stats(now_ms());
}

void SinkEngine::accept_if_pending(int timeoutMs) {
    std::string err;
    std::unique_ptr<IConnection> fresh = listener_.accept(timeoutMs, &err);
    if (!fresh) {
        if (!err.empty()) WIDI_LOGW("accept failed: %s", err.c_str());
        return;
    }
    connection_ = std::move(fresh);
    connection_->set_nodelay(true);
    peerLabel_ = connection_->peer_address();

    if (session_) session_->reset();
    hasPending_ = false;
    pending_.clear();
    stats_ = SinkStats();
    stats_.rate = rate_.current();
    stats_.peer = peerLabel_;
    windowStartMs_ = now_ms();
    lastStatsMs_ = windowStartMs_;
    windowPresented_ = 0;
    windowDecodeMsSum_ = 0;
    totalDecodeMsSum_ = 0;
    rate_.reset();
    stats_.rate = rate_.current();

    WIDI_LOGI("sender connected: %s", peerLabel_.c_str());
    host_.on_client_changed(true, peerLabel_);
}

void SinkEngine::close_connection(uint8_t reason) {
    if (!connection_) return;
    const std::string who = peerLabel_;
    if (session_) {
        if (session_->state() == SessionState::Streaming) session_->send_bye(reason);
        session_->reset();
    }
    connection_->close();
    connection_.reset();
    hasPending_ = false;
    pending_.clear();
    peerLabel_.clear();
    stats_.peer.clear();
    WIDI_LOGI("sender disconnected: %s (reason %u)", who.c_str(), reason);
    host_.on_client_changed(false, who);
}

bool SinkEngine::send_message(MsgType type, uint16_t flags, uint32_t seq,
                              const uint8_t* payload, size_t size) {
    if (!connection_) return false;
    const std::vector<uint8_t> bytes = encode_message(type, flags, seq, payload, size);
    return connection_->send_all(bytes.data(), bytes.size());
}

bool SinkEngine::send_input(const InputEvent& ev) {
    if (!session_ || session_->state() != SessionState::Streaming) return false;
    return session_->send_input(ev);
}

}  // namespace widi

namespace widi {

// ---------------------------------------------------------------------------
// Incoming data
// ---------------------------------------------------------------------------

void SinkEngine::on_frame(const FrameInfo& info, const uint8_t* data, size_t size) {
    // Latest-frame-wins: a picture that is still queued is worthless once a
    // newer one has arrived, so we drop it instead of building latency.
    if (hasPending_) {
        ++stats_.dropped;
    }
    pending_.assign(data, data + size);
    pendingInfo_ = info;
    hasPending_ = true;
}

void SinkEngine::on_input(const InputEvent& ev) { host_.on_remote_input(ev); }

void SinkEngine::on_peer_gone(uint8_t reason) {
    WIDI_LOGI("peer said goodbye (reason %u)", reason);
    close_connection(reason);
}

void SinkEngine::on_ready(const HelloInfo& peer) {
    WIDI_LOGI("handshake complete: %s (%ux%u @%u fps, caps 0x%02x)", peer.name.c_str(),
              peer.maxWidth, peer.maxHeight, peer.maxFps, peer.capabilities);
}

void SinkEngine::drain_and_present() {
    if (!hasPending_) return;
    const FrameInfo info = pendingInfo_;
    const uint32_t began = now_ms();
    const bool ok = host_.frames().consume(info, pending_.data(), pending_.size());
    const uint32_t spent = now_ms() - began;
    hasPending_ = false;

    if (!ok) {
        ++stats_.decodeErrors;
        ++stats_.dropped;
        return;
    }
    record_present(spent);
}

void SinkEngine::record_present(uint32_t decodeMs) {
    ++stats_.presented;
    ++windowPresented_;
    windowDecodeMsSum_ += decodeMs;
    totalDecodeMsSum_ += decodeMs;
    const uint32_t avg = totalDecodeMsSum_ / (stats_.presented ? stats_.presented : 1u);
    stats_.decodeMs = static_cast<uint16_t>(avg > 65535u ? 65535u : avg);
}

// ---------------------------------------------------------------------------
// Periodic traffic
// ---------------------------------------------------------------------------

void SinkEngine::maybe_send_stats(uint32_t now) {
    const uint32_t interval = static_cast<uint32_t>(config_.statsIntervalMs > 0
                                                       ? config_.statsIntervalMs
                                                       : 500);
    if (now - lastStatsMs_ < interval) return;
    const uint32_t windowMs = now - windowStartMs_;
    lastStatsMs_ = now;

    uint32_t fps = windowMs ? (windowPresented_ * 1000u / windowMs) : 0u;
    if (fps > 255u) fps = 255u;

    StatsInfo si;
    si.decoded = stats_.presented;
    si.dropped = stats_.dropped;
    const uint32_t cappedMs = stats_.decodeMs > 4000u ? 4000u : stats_.decodeMs;
    si.decodeMsQ8 = static_cast<uint16_t>(cappedMs * 16u);
    si.clientFpsQ8 = static_cast<uint16_t>(fps * 16u);
    if (session_) session_->send_stats(si);

    RateFeedback fb;
    fb.decodeMs = stats_.decodeMs;
    fb.clientFps = static_cast<uint8_t>(fps);
    fb.decoded = stats_.presented;
    fb.dropped = stats_.dropped;
    rate_.feedback(fb);
    stats_.rate = rate_.current();
    stats_.presentedFps = static_cast<uint8_t>(fps);
    host_.frames().on_rate_changed(stats_.rate);

    WIDI_LOGD("stats: presented=%u dropped=%u fps=%u decode=%ums -> q%u/%ufps/%u%%",
              stats_.presented, stats_.dropped, fps, stats_.decodeMs,
              stats_.rate.quality, stats_.rate.fps, stats_.rate.scalePercent);

    windowStartMs_ = now;
    windowPresented_ = 0;
    windowDecodeMsSum_ = 0;
}

void SinkEngine::maybe_send_beacon(uint32_t now) {
    if (!config_.broadcastBeacons || !beacon_.valid()) return;
    const uint32_t interval =
        static_cast<uint32_t>(config_.beaconIntervalMs > 0 ? config_.beaconIntervalMs : 2000);
    if (lastBeaconMs_ != 0 && now - lastBeaconMs_ < interval) return;
    lastBeaconMs_ = (now == 0) ? 1u : now;

    BeaconInfo info;
    info.tcpPort = config_.tcpPort;
    info.screenWidth = config_.screenWidth;
    info.screenHeight = config_.screenHeight;
    info.capabilities = config_.capabilities;
    info.name = config_.name;
    const std::vector<uint8_t> bytes = encode_beacon(info);
    // Broadcast plus a loopback copy so a sender on the same machine (dev) sees it.
    beacon_.send_to("255.255.255.255", config_.discoveryPort, bytes.data(), bytes.size());
    beacon_.send_to("127.0.0.1", config_.discoveryPort, bytes.data(), bytes.size());
}

}  // namespace widi
