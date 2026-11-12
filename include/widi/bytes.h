// SurfaceRT-WiDi - little-endian serialisation helpers shared by every component.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace widi {

// Appends little-endian scalars to a growable byte buffer.
class BufWriter {
public:
    explicit BufWriter(std::vector<uint8_t>& out) : out_(out) {}

    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { push_le(v, 2); }
    void u32(uint32_t v) { push_le(v, 4); }
    void i32(int32_t v) { push_le(static_cast<uint32_t>(v), 4); }
    void bytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out_.insert(out_.end(), b, b + n);
    }
    // Length-prefixed string (u8 length, so max 255 bytes).
    void str(const std::string& s) {
        const size_t n = s.size() > 255 ? 255 : s.size();
        u8(static_cast<uint8_t>(n));
        bytes(s.data(), n);
    }

private:
    void push_le(uint32_t v, int width) {
        for (int i = 0; i < width; ++i) {
            out_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
        }
    }
    std::vector<uint8_t>& out_;
};

// Bounds-checked little-endian reader. Once `ok()` turns false it stays false,
// which lets callers decode a whole structure and validate once at the end.
class BufReader {
public:
    BufReader(const uint8_t* data, size_t size) : p_(data), n_(size) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ ? n_ - off_ : 0; }

    uint8_t u8() {
        if (!need(1)) return 0;
        return p_[off_++];
    }
    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = static_cast<uint16_t>(p_[off_] | (p_[off_ + 1] << 8));
        off_ += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = static_cast<uint32_t>(p_[off_]) |
                     (static_cast<uint32_t>(p_[off_ + 1]) << 8) |
                     (static_cast<uint32_t>(p_[off_ + 2]) << 16) |
                     (static_cast<uint32_t>(p_[off_ + 3]) << 24);
        off_ += 4;
        return v;
    }
    std::string str() {
        const uint8_t len = u8();
        if (!ok_ || !need(len)) return std::string();
        std::string s(reinterpret_cast<const char*>(p_ + off_), len);
        off_ += len;
        return s;
    }
    bool bytes(void* dst, size_t count) {
        if (!need(count)) return false;
        std::memcpy(dst, p_ + off_, count);
        off_ += count;
        return true;
    }

private:
    bool need(size_t count) {
        if (!ok_) return false;
        if (count > n_ - off_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

}  // namespace widi
