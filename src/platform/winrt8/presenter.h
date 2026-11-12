// SurfaceRT-WiDi - full screen presenter: Direct2D (hardware) with GDI fallback.
// SPDX-License-Identifier: MIT
#pragma once
#if defined(_WIN32)

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "image_decoder.h"

namespace widi {

class Presenter {
public:
    bool init(HWND hwnd, std::string* err);
    void shutdown();

    // Letterboxes the picture into the client area. Returns false when the
    // frame could not be shown (device lost); the next frame re-creates it.
    bool present(const DecodedImage& image);

    void resize(uint32_t width, uint32_t height);
    void set_status(const std::wstring& text);
    const char* backend() const { return gdi_ ? "gdi" : "d2d"; }

private:
    bool create_target();
    void release_target();
    bool ensure_bitmap(uint32_t width, uint32_t height);
    RECT dest_rect(uint32_t srcW, uint32_t srcH) const;
    void draw_status_gdi(HDC dc, const RECT& area);

    HWND hwnd_ = nullptr;
    uint32_t clientWidth_ = 0;
    uint32_t clientHeight_ = 0;
    bool gdi_ = false;
    bool needTarget_ = true;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush_;
    uint32_t bitmapWidth_ = 0;
    uint32_t bitmapHeight_ = 0;

    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> statusFormat_;
    std::wstring status_;
};

}  // namespace widi

#endif  // _WIN32
