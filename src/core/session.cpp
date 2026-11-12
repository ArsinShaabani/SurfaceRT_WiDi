// SurfaceRT-WiDi - sink session state machine.
// SPDX-License-Identifier: MIT
#include "widi/session.h"

#include <cstring>

namespace widi {
namespace {
// Never buffer more than one maximal message plus a header's worth of slack.
constexpr size_t kBufferLimit = kMaxPayload + kHeaderSize * 2;
}  // namespace

SinkSession::SinkSession(IMessageSink& out, ISessionObserver& observer)
    : out_(out), observer_(observer) {
    identity_.name = kServiceName;
    buffer_.reserve(64 * 1024);
}

void SinkSession::reset() {
    buffer_.clear();
    consumed_ = 0;
    state_ = SessionState::Idle;
    peer_ = HelloInfo();
    last_error_.clear();
    frames_ = 0;
    bytes_ = 0;
    errors_ = 0;
}

void SinkSession::fail(const std::string& why) {
    last_error_ = why;
    ++errors_;
}

void SinkSession::feed(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    if (state_ == SessionState::Closed) return;  // teardown finished; ignore the tail
    bytes_ += static_cast<uint32_t>(size);
    if (buffer_.size() + size > kBufferLimit) {
        // The peer is either broken or hostile: keep only what is still parsable.
        compact();
        if (buffer_.size() + size > kBufferLimit) {
            buffer_.clear();
            consumed_ = 0;
            fail("rx buffer overflow");
            return;
        }
    }
    buffer_.insert(buffer_.end(), data, data + size);
    drain();
}

void SinkSession::compact() {
    if (consumed_ == 0) return;
    // An observer callback (Bye / error) may have reset the session mid-drain, in
    // which case the buffer is already empty and `consumed_` is stale.
    if (consumed_ >= buffer_.size()) {
        buffer_.clear();
        consumed_ = 0;
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
    consumed_ = 0;
}

void SinkSession::drain() {
    for (;;) {
        const size_t available = buffer_.size() - consumed_;
        if (available < kHeaderSize) break;

        Header h;
        std::string err;
        if (!decode_header(buffer_.data() + consumed_, available, h, err)) {
            // Skip one byte and try to resynchronise on the next magic value.
            fail(err);
            ++consumed_;
            continue;
        }
        if (available < kHeaderSize + h.length) break;  // wait for the rest

        const uint8_t* payload = buffer_.data() + consumed_ + kHeaderSize;
        if (h.length && crc32(payload, h.length) != h.crc) {
            fail("payload crc mismatch");
            ++consumed_;
            continue;
        }
        const size_t consumedTotal = kHeaderSize + h.length;
        if (!handle_message(h, payload, h.length)) {
            // Fatal for the session; keep the buffer consistent.
            consumed_ += consumedTotal;
            compact();
            return;
        }
        consumed_ += consumedTotal;
    }
    compact();
}

bool SinkSession::handle_message(const Header& h, const uint8_t* payload, size_t size) {
    std::string err;
    switch (h.type) {
        case MsgType::Hello: {
            HelloInfo info;
            if (!decode_hello(payload, size, info, err)) {
                fail(err);
                HelloAckInfo rej = identity_;
                rej.flags = 0;
                rej.reason = kReasonProtocolMismatch;
                auto bytes = encode_hello_ack(rej);
                out_.send_message(MsgType::HelloAck, 0, next_seq(), bytes.data(), bytes.size());
                state_ = SessionState::Closed;
                return false;
            }
            peer_ = info;
            HelloAckInfo ack = identity_;
            ack.flags = 1;
            ack.reason = kReasonNormal;
            const auto bytes = encode_hello_ack(ack);
            if (!out_.send_message(MsgType::HelloAck, 0, next_seq(), bytes.data(), bytes.size())) {
                fail("cannot send hello ack");
                return false;
            }
            state_ = SessionState::Streaming;
            observer_.on_ready(peer_);
            return true;
        }
        case MsgType::Frame: {
            if (state_ != SessionState::Streaming) {
                // Frames before the handshake are dropped but not fatal: a sender
                // restarting quickly may race its own Hello.
                fail("frame before handshake");
                return true;
            }
            FrameInfo info;
            if (!decode_frame_header(payload, size, info, err)) {
                fail(err);
                return true;
            }
            ++frames_;
            observer_.on_frame(info, payload + kFrameHeaderSize, size - kFrameHeaderSize);
            return true;
        }
        case MsgType::Input: {
            InputEvent ev;
            if (!decode_input(payload, size, ev, err)) {
                fail(err);
                return true;
            }
            observer_.on_input(ev);
            return true;
        }
        case MsgType::Ping: {
            out_.send_message(MsgType::Pong, h.flags, h.seq, nullptr, 0);
            return true;
        }
        case MsgType::Pong:
            return true;
        case MsgType::Bye: {
            const uint8_t reason = size ? payload[0] : static_cast<uint8_t>(kReasonPeerClosed);
            state_ = SessionState::Closed;
            observer_.on_peer_gone(reason);
            return false;
        }
        case MsgType::HelloAck:
        case MsgType::Stats:
        case MsgType::Beacon:
            // Sender -> sink messages that a sink must ignore.
            return true;
    }
    fail("unhandled message type");
    return true;
}

bool SinkSession::send_stats(const StatsInfo& info) {
    const auto bytes = encode_stats(info);
    return out_.send_message(MsgType::Stats, 0, next_seq(), bytes.data(), bytes.size());
}

bool SinkSession::send_input(const InputEvent& ev) {
    const auto bytes = encode_input(ev);
    return out_.send_message(MsgType::Input, 0, next_seq(), bytes.data(), bytes.size());
}

bool SinkSession::send_ping() {
    return out_.send_message(MsgType::Ping, 0, next_seq(), nullptr, 0);
}

bool SinkSession::send_bye(uint8_t reason) {
    const uint8_t body = reason;
    const bool ok = out_.send_message(MsgType::Bye, 0, next_seq(), &body, 1);
    state_ = SessionState::Closed;
    return ok;
}

}  // namespace widi
