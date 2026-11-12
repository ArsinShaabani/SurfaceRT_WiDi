// SurfaceRT-WiDi - WIC decoder implementation.
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "image_decoder.h"

#include <cstring>

#include "widi/log.h"

#pragma comment(lib, "windowscodecs.lib")

namespace widi {

bool ImageDecoder::init(std::string* err) {
    if (factory_) return true;
    const HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory_.GetAddressOf()));
    if (FAILED(hr)) {
        if (err) *err = "WIC factory unavailable";
        WIDI_LOGE("CoCreateInstance(WICImagingFactory) failed: 0x%08lx", hr);
        return false;
    }
    scratch_.bgra.reserve(1920u * 1080u * 4u);
    return true;
}

void ImageDecoder::shutdown() {
    converter_.Reset();
    factory_.Reset();
    scratch_.bgra.clear();
    scratch_.width = scratch_.height = scratch_.stride = 0;
}

bool ImageDecoder::decode_jpeg(const uint8_t* data, size_t size, DecodedImage& out,
                               std::string* err) {
    if (!factory_ && !init(err)) return false;
    if (!data || size == 0) {
        if (err) *err = "empty jpeg";
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    HRESULT hr = factory_->CreateStream(&stream);
    if (FAILED(hr)) {
        if (err) *err = "WIC CreateStream failed";
        return false;
    }
    // WIC requires a mutable pointer but never writes to the source bytes.
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size));
    if (FAILED(hr)) {
        if (err) *err = "jpeg stream init failed";
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory_->CreateDecoderFromStream(stream.Get(), nullptr,
                                           WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        if (err) *err = "jpeg decoder creation failed";
        return false;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        if (err) *err = "jpeg has no frame";
        return false;
    }

    if (!converter_) {
        hr = factory_->CreateFormatConverter(converter_.GetAddressOf());
        if (FAILED(hr)) {
            if (err) *err = "WIC format converter failed";
            return false;
        }
    }
    hr = converter_->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        if (err) *err = "jpeg pixel format conversion failed";
        return false;
    }

    UINT w = 0;
    UINT h = 0;
    hr = converter_->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0 || w > 8192 || h > 8192) {
        if (err) *err = "jpeg has invalid dimensions";
        return false;
    }

    out.width = w;
    out.height = h;
    out.stride = w * 4u;
    out.bgra.resize(static_cast<size_t>(out.stride) * h);
    hr = converter_->CopyPixels(nullptr, out.stride, static_cast<UINT>(out.bgra.size()),
                               out.bgra.data());
    if (FAILED(hr)) {
        if (err) *err = "jpeg pixels could not be copied";
        out.bgra.clear();
        out.width = out.height = out.stride = 0;
        return false;
    }
    return true;
}

bool ImageDecoder::copy_bgra(const uint8_t* data, size_t size, uint16_t width,
                             uint16_t height, DecodedImage& out, std::string* err) {
    const size_t need = static_cast<size_t>(width) * height * 4u;
    if (!data || size < need || width == 0 || height == 0) {
        if (err) *err = "raw frame size mismatch";
        return false;
    }
    out.width = width;
    out.height = height;
    out.stride = width * 4u;
    out.bgra.assign(data, data + need);
    return true;
}

}  // namespace widi

#endif  // _WIN32
