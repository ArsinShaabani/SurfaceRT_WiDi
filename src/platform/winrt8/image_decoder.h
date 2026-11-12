// SurfaceRT-WiDi - WIC based JPEG/BGRA decoder (Windows RT 8.1 / ARM).
// SPDX-License-Identifier: MIT
#pragma once
#if defined(_WIN32)

#include <windows.h>  // must precede wincodec.h
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace widi {

struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    std::vector<uint8_t> bgra;
};

// One decoder instance per sink; WIC objects are not thread safe, and the sink
// is single threaded by design.
class ImageDecoder {
public:
    bool init(std::string* err);
    void shutdown();

    // Decodes a baseline/progressive JPEG into a BGRA8 buffer.
    // Corrupt input returns false with a message; nothing throws.
    bool decode_jpeg(const uint8_t* data, size_t size, DecodedImage& out, std::string* err);

    // Wraps a raw BGRA payload (protocol pixel format 0).
    bool copy_bgra(const uint8_t* data, size_t size, uint16_t width, uint16_t height,
                   DecodedImage& out, std::string* err);

private:
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter_;
    DecodedImage scratch_;
};

}  // namespace widi

#endif  // _WIN32
