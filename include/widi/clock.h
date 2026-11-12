// SurfaceRT-WiDi - monotonic millisecond clock.
// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>

namespace widi {

// Wraps roughly every 49 days; every comparison in the codebase uses unsigned
// difference arithmetic, so wrap-around stays correct. Deliberately free of
// function-local statics so it behaves identically on VS2013 (no magic statics).
inline uint32_t now_ms() {
    using namespace std::chrono;
    const long long ticks =
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    return static_cast<uint32_t>(static_cast<unsigned long long>(ticks) & 0xFFFFFFFFull);
}

}  // namespace widi
