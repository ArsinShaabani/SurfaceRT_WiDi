// SurfaceRT-WiDi - UDP discovery beacon (sink -> LAN broadcast).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace widi {

struct BeaconInfo {
    uint16_t tcpPort = 0;
    uint16_t screenWidth = 0;
    uint16_t screenHeight = 0;
    uint8_t capabilities = 0;
    std::string name;
};

std::vector<uint8_t> encode_beacon(const BeaconInfo& info);
bool decode_beacon(const uint8_t* data, size_t size, BeaconInfo& out, std::string& err);

}  // namespace widi
