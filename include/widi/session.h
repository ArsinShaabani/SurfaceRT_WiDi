// SurfaceRT-WiDi - sink-side session state machine (transport agnostic, unit tested).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "widi/protocol.h"

namespace widi {

// Outbound path used by the session (implemented by the real socket on device,
// by a recording stub inside the tests).
class IMessageSink {
public:
    virtual ~IMessageSink() {}
    virtual bool send_message(MsgType type, uint16_t flags, uint32_t seq,
                              const uint8_t* payload, size_t size) = 0;
};

class ISessionObserver {
public:
    virtual ~ISessionObserver() {}
    // `data` borrows the session's internal buffer and is only valid for this call.
    virtual void on_frame(const FrameInfo& info, const uint8_t* data, size_t size) = 0;
    virtual void on_input(const InputEvent& ev) { (void)ev; }
    virtual void on_peer_gone(uint8_t reason) { (void)reason; }
    virtual void on_ready(const HelloInfo& peer) { (void)peer; }
};

enum class SessionState { Idle, Handshaking, Streaming, Closed };

// Consumes a byte stream from one sender, validates it and liberates frames.
//
// Threading: not internally synchronised. The caller drives it from the socket
// thread only (the RT app is single threaded apart from the service host).
class SinkSession {
public:
    SinkSession(IMessageSink& out, ISessionObserver& observer);

    // Identity reported in HelloAck.
    void set_identity(const HelloAckInfo& identity) { identity_ = identity; }

    // Feeds raw bytes as they arrive from the socket.
    void feed(const uint8_t* data, size_t size);

    // Builders for sink -> sender traffic.
    bool send_stats(const StatsInfo& info);
    bool send_input(const InputEvent& ev);
    bool send_ping();
    bool send_bye(uint8_t reason);

    void reset();

    SessionState state() const { return state_; }
    const HelloInfo& peer() const { return peer_; }
    uint32_t frames_received() const { return frames_; }
    uint32_t bytes_received() const { return bytes_; }
    uint32_t protocol_errors() const { return errors_; }
    size_t buffered() const { return buffer_.size(); }
    const std::string& last_error() const { return last_error_; }

private:
    bool handle_message(const Header& h, const uint8_t* payload, size_t size);
    void fail(const std::string& why);
    void drain();          // parses as many complete messages as possible
    void compact();        // drops consumed bytes, keeps the remainder
    uint32_t next_seq() { return ++tx_seq_; }

    IMessageSink& out_;
    ISessionObserver& observer_;
    HelloAckInfo identity_;

    std::vector<uint8_t> buffer_;
    size_t consumed_ = 0;

    SessionState state_ = SessionState::Idle;
    HelloInfo peer_;
    std::string last_error_;
    uint32_t tx_seq_ = 0;
    uint32_t frames_ = 0;
    uint32_t bytes_ = 0;
    uint32_t errors_ = 0;
};

}  // namespace widi
