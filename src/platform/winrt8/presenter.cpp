// SurfaceRT-WiDi - presenter implementation.
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#include "presenter.h"

#include <cstdio>

#include "widi/log.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace widi {
namespace {
const wchar_t* kStatusFont = L"Segoe UI";
}  // namespace

bool Presenter::init(HWND hwnd, std::string* err) {
    hwnd_ = hwnd;
    RECT rc;
    ::GetClientRect(hwnd, &rc);
    clientWidth_ = static_cast<uint32_t>(rc.right - rc.left);
    clientHeight_ = static_cast<uint32_t>(rc.bottom - rc.top);

    HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.GetAddressOf());
    if (FAILED(hr)) {
        WIDI_LOGW("Direct2D unavailable (0x%08lx); using GDI", hr);
        gdi_ = true;
        return true;
    }

    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        dwriteFactory_->CreateTextFormat(kStatusFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        18.0f, L"en-us", statusFormat_.GetAddressOf());
    }

    if (!create_target()) {
        WIDI_LOGW("D2D render target failed; using GDI");
        gdi_ = true;
        return true;
    }
    if (err) err->clear();
    return true;
}

bool Presenter::create_target() {
    if (!d2dFactory_ || !hwnd_) return false;
    if (clientWidth_ == 0 || clientHeight_ == 0) return false;
    D2D1_SIZE_U size = D2D1::SizeU(clientWidth_, clientHeight_);
    const HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        target_.GetAddressOf());
    if (FAILED(hr)) return false;
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                   textBrush_.GetAddressOf());
    needTarget_ = false;
    bitmap_.Reset();
    bitmapWidth_ = bitmapHeight_ = 0;
    return true;
}

void Presenter::release_target() {
    textBrush_.Reset();
    bitmap_.Reset();
    bitmapWidth_ = bitmapHeight_ = 0;
    target_.Reset();
    needTarget_ = true;
}

void Presenter::shutdown() {
    release_target();
    statusFormat_.Reset();
    dwriteFactory_.Reset();
    d2dFactory_.Reset();
    hwnd_ = nullptr;
}

void Presenter::resize(uint32_t width, uint32_t height) {
    clientWidth_ = width;
    clientHeight_ = height;
    if (target_) {
        target_->Resize(D2D1::SizeU(width, height));
        bitmap_.Reset();
        bitmapWidth_ = bitmapHeight_ = 0;
    }
}

void Presenter::set_status(const std::wstring& text) {
    if (text == status_) return;
    status_ = text;
}

RECT Presenter::dest_rect(uint32_t srcW, uint32_t srcH) const {
    RECT out = {0, 0, static_cast<LONG>(clientWidth_), static_cast<LONG>(clientHeight_)};
    if (srcW == 0 || srcH == 0 || clientWidth_ == 0 || clientHeight_ == 0) return out;
    const double scaleX = static_cast<double>(clientWidth_) / srcW;
    const double scaleY = static_cast<double>(clientHeight_) / srcH;
    const double scale = scaleX < scaleY ? scaleX : scaleY;
    const LONG w = static_cast<LONG>(srcW * scale + 0.5);
    const LONG h = static_cast<LONG>(srcH * scale + 0.5);
    out.left = (static_cast<LONG>(clientWidth_) - w) / 2;
    out.top = (static_cast<LONG>(clientHeight_) - h) / 2;
    out.right = out.left + w;
    out.bottom = out.top + h;
    return out;
}

bool Presenter::ensure_bitmap(uint32_t width, uint32_t height) {
    if (!target_) return false;
    if (bitmap_ && bitmapWidth_ == width && bitmapHeight_ == height) return true;
    bitmap_.Reset();
    D2D1_SIZE_U size = D2D1::SizeU(width, height);
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    const HRESULT hr = target_->CreateBitmap(size, nullptr, 0, props,
                                            bitmap_.GetAddressOf());
    if (FAILED(hr)) {
        bitmap_.Reset();
        return false;
    }
    bitmapWidth_ = width;
    bitmapHeight_ = height;
    return true;
}

}  // namespace widi

#endif  // _WIN32

namespace widi {

bool Presenter::present(const DecodedImage& image) {
    if (image.width == 0 || image.height == 0 || image.bgra.empty()) return false;
    const RECT dst = dest_rect(image.width, image.height);

    if (gdi_) {
        HDC dc = ::GetDC(hwnd_);
        if (!dc) return false;
        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(image.width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(image.height);  // top-down rows
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        ::SetStretchBltMode(dc, HALFTONE);
        ::SetBrushOrgEx(dc, 0, 0, nullptr);
        ::StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top, 0, 0,
                        static_cast<int>(image.width), static_cast<int>(image.height),
                        image.bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        if (!status_.empty()) draw_status_gdi(dc, dst);
        ::ReleaseDC(hwnd_, dc);
        return true;
    }

    if (needTarget_ || !target_) {
        if (!create_target()) {
            gdi_ = true;
            WIDI_LOGW("render target lost; falling back to GDI");
            return present(image);
        }
    }
    if (!ensure_bitmap(image.width, image.height)) return false;

    HRESULT hr = bitmap_->CopyFromMemory(nullptr, image.bgra.data(), image.stride);
    if (FAILED(hr)) {
        release_target();
        return false;
    }

    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    const D2D1_RECT_F dest = D2D1::RectF(static_cast<FLOAT>(dst.left),
                                        static_cast<FLOAT>(dst.top),
                                        static_cast<FLOAT>(dst.right),
                                        static_cast<FLOAT>(dst.bottom));
    target_->DrawBitmap(bitmap_.Get(), &dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                        nullptr);

    if (!status_.empty() && statusFormat_ && textBrush_) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                status_.c_str(), static_cast<UINT32>(status_.size()), statusFormat_.Get(),
                static_cast<FLOAT>(clientWidth_ - 32), 60.0f, layout.GetAddressOf())) &&
            layout) {
            // Dark plate keeps the HUD readable over bright content.
            const D2D1_RECT_F plate = D2D1::RectF(8.0f, 8.0f, 8.0f + 520.0f, 44.0f);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> plateBrush;
            if (SUCCEEDED(target_->CreateSolidColorBrush(
                    D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f), plateBrush.GetAddressOf())) &&
                plateBrush) {
                target_->FillRectangle(&plate, plateBrush.Get());
            }
            target_->DrawTextLayout(D2D1::Point2F(16.0f, 12.0f), layout.Get(), textBrush_.Get());
        }
    }

    hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        release_target();
        return false;
    }
    return SUCCEEDED(hr);
}

void Presenter::draw_status_gdi(HDC dc, const RECT& area) {
    (void)area;
    RECT plate = {8, 8, 8 + 520, 44};
    HBRUSH brush = ::CreateSolidBrush(RGB(0, 0, 0));
    ::FillRect(dc, &plate, brush);
    ::DeleteObject(brush);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(255, 255, 255));
    ::DrawTextW(dc, status_.c_str(), -1, &plate, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                                       DT_NOPREFIX | DT_END_ELLIPSIS);
}

}  // namespace widi

#endif  // _WIN32
