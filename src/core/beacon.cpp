// SurfaceRT-WiDi - discovery beacon codec.
// SPDX-License-Identifier: MIT
#include "widi/beacon.h"

#include <cstring>

#include "widi/bytes.h"
#include "widi/protocol.h"

namespace widi {

std::vector<uint8_t> encode_beacon(const BeaconInfo& info) {
    std::vector<uint8_t> out;
    out.reserve(16 + info.name.size());
    BufWriter w(out);
    w.bytes(kBeaconMagic, sizeof(kBeaconMagic));
    w.u8(kProtocolVersion);
    w.u16(info.tcpPort);
    w.u16(info.screenWidth);
    w.u16(info.screenHeight);
    w.u8(info.capabilities);
    w.str(info.name);
    return out;
}

bool decode_beacon(const uint8_t* data, size_t size, BeaconInfo& out, std::string& err) {
    if (!data || size < sizeof(kBeaconMagic) + 9) {
        err = "short beacon";
        return false;
    }
    if (std::memcmp(data, kBeaconMagic, sizeof(kBeaconMagic)) != 0) {
        err = "not a widi beacon";
        return false;
    }
    BufReader r(data, size);
    uint8_t magic[sizeof(kBeaconMagic)];
    if (!r.bytes(magic, sizeof(magic))) {
        err = "short beacon";
        return false;
    }
    const uint8_t version = r.u8();
    out.tcpPort = r.u16();
    out.screenWidth = r.u16();
    out.screenHeight = r.u16();
    out.capabilities = r.u8();
    out.name = r.str();
    if (!r.ok()) {
        err = "malformed beacon";
        return false;
    }
    if (version != kProtocolVersion) {
        err = "beacon version mismatch";
        return false;
    }
    if (out.tcpPort == 0) {
        err = "beacon without port";
        return false;
    }
    return true;
}

}  // namespace widi
