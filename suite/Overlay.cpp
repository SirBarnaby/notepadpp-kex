// Overlay.cpp - layered window helpers and software sprite drawing.
#include "Overlay.h"
#include <cmath>
#include <cstring>

namespace kex {

bool Surface::ensure(int width, int height)
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (bmp && width <= stride && height <= capH) { w = width; h = height; return true; }
    int nw = std::max(width, stride), nh = std::max(height, capH);
    nw = (nw + 63) & ~63; nh = (nh + 63) & ~63;
    release();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = nw;
    bi.bmiHeader.biHeight = -nh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = ::GetDC(nullptr);
    void* bits = nullptr;
    bmp = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    dc = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (!bmp || !dc) { release(); return false; }
    old = ::SelectObject(dc, bmp);
    px = static_cast<uint32_t*>(bits);
    stride = nw; capH = nh; w = width; h = height;
    return true;
}

void Surface::clear()
{
    if (!px) return;
    for (int y = 0; y < h; ++y) memset(px + static_cast<size_t>(y) * stride, 0, static_cast<size_t>(w) * 4);
}

void Surface::release()
{
    if (dc) { if (old) ::SelectObject(dc, old); ::DeleteDC(dc); }
    if (bmp) ::DeleteObject(bmp);
    dc = nullptr; bmp = nullptr; old = nullptr; px = nullptr; w = h = stride = capH = 0;
}

static LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    }
    return ::DefWindowProcW(h, m, w, l);
}

HWND createOverlayWindow(HWND owner, bool topmost, const wchar_t* title)
{
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = overlayProc;
        wc.hInstance = hInst;
        wc.lpszClassName = L"KexSuiteOverlay";
        reg = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (topmost) ex |= WS_EX_TOPMOST;
    return ::CreateWindowExW(ex, L"KexSuiteOverlay", title, WS_POPUP, 0, 0, 1, 1, owner, nullptr, hInst, nullptr);
}

bool pushLayered(HWND wnd, Surface& s, POINT pos, SIZE size)
{
    if (!wnd || !s.dc) return false;
    if (binaryAlpha())
    {
        // Wine (no compositor) ignores partial alpha: snap to opaque/transparent so colours stay true.
        for (int y = 0; y < size.cy && y < s.h; ++y)
        {
            uint32_t* row = s.px + static_cast<size_t>(y) * s.stride;
            for (int x = 0; x < size.cx && x < s.w; ++x)
            {
                uint32_t p = row[x];
                uint32_t a = p >> 24;
                if (a >= 110)
                {
                    uint32_t b = (p & 0xFF) * 255 / a, g = ((p >> 8) & 0xFF) * 255 / a, r = ((p >> 16) & 0xFF) * 255 / a;
                    if (b > 255) b = 255; if (g > 255) g = 255; if (r > 255) r = 255;
                    row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
                else row[x] = 0;
            }
        }
    }
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    return ::UpdateLayeredWindow(wnd, nullptr, &pos, &size, s.dc, &src, 0, &bf, ULW_ALPHA) != FALSE;
}

bool renderRuns(Sprite& out, const std::vector<TextRun>& runs, int w, int h, int baseline, int pad, COLORREF back, float backAlpha)
{
    Surface mask;
    int W = w + 2 * pad, H = h + 2 * pad;
    if (!mask.ensure(W, H)) return false;
    out.w = W; out.h = H;
    out.px.assign(static_cast<size_t>(W) * H, 0);
    if (backAlpha > 0)
    {
        const uint32_t a = static_cast<uint32_t>(std::min(1.0f, backAlpha) * 255.0f + 0.5f);
        const uint32_t p = (a << 24) | ((GetRValue(back) * a / 255) << 16) | ((GetGValue(back) * a / 255) << 8) | (GetBValue(back) * a / 255);
        for (int y = pad; y < pad + h; ++y)
            std::fill(out.px.begin() + static_cast<size_t>(y) * W + pad, out.px.begin() + static_cast<size_t>(y) * W + pad + w, p);
    }
    ::SetBkMode(mask.dc, TRANSPARENT);
    ::SetTextColor(mask.dc, RGB(255, 255, 255));
    ::SetTextAlign(mask.dc, TA_BASELINE | TA_LEFT);
    // the runs of one colour are drawn into the mask together and put on the sprite in one pass
    std::vector<size_t> order(runs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return runs[a].color < runs[b].color; });
    for (size_t g = 0; g < order.size();)
    {
        const COLORREF colour = runs[order[g]].color;
        mask.clear();
        size_t e = g;
        for (; e < order.size() && runs[order[e]].color == colour; ++e)
        {
            const TextRun& r = runs[order[e]];
            HGDIOBJ of = ::SelectObject(mask.dc, r.font ? r.font : ::GetStockObject(DEFAULT_GUI_FONT));
            ::ExtTextOutW(mask.dc, pad + r.dx, pad + baseline, 0, nullptr, r.text.c_str(), static_cast<UINT>(r.text.size()), nullptr);
            ::SelectObject(mask.dc, of);
        }
        g = e;
        ::GdiFlush();
        uint32_t cr = GetRValue(colour), cg = GetGValue(colour), cb = GetBValue(colour);
        for (int y = 0; y < H; ++y)
        {
            const uint32_t* mrow = mask.px + static_cast<size_t>(y) * mask.stride;
            uint32_t* orow = out.px.data() + static_cast<size_t>(y) * W;
            for (int x = 0; x < W; ++x)
            {
                uint32_t m = mrow[x];
                uint32_t a = std::max(std::max(m & 0xFF, (m >> 8) & 0xFF), (m >> 16) & 0xFF);
                if (!a) continue;
                uint32_t d = orow[x];
                uint32_t inv = 255 - a;
                uint32_t na = a + (((d >> 24) * inv) / 255);
                uint32_t nr = (cr * a) / 255 + ((((d >> 16) & 0xFF) * inv) / 255);
                uint32_t ng = (cg * a) / 255 + ((((d >> 8) & 0xFF) * inv) / 255);
                uint32_t nb = (cb * a) / 255 + (((d & 0xFF) * inv) / 255);
                orow[x] = (na << 24) | (nr << 16) | (ng << 8) | nb;
            }
        }
    }
    return true;
}

// premultiplied "over" with integer weights: s scaled by k/256 first (k = 256 keeps it)
static inline void blendOverK(uint32_t& d, uint32_t s, uint32_t k)
{
    if (k < 256)
    {
        const uint32_t rb = ((s & 0x00FF00FFu) * k >> 8) & 0x00FF00FFu;
        const uint32_t ag = (((s >> 8) & 0x00FF00FFu) * k >> 8) & 0x00FF00FFu;
        s = rb | (ag << 8);
    }
    const uint32_t sa = s >> 24;
    if (!sa) return;
    if (sa == 255) { d = s; return; }
    const uint32_t inv = 256 - sa;
    const uint32_t rb = ((d & 0x00FF00FFu) * inv >> 8) & 0x00FF00FFu;
    const uint32_t ag = (((d >> 8) & 0x00FF00FFu) * inv >> 8) & 0x00FF00FFu;
    d = s + (rb | (ag << 8));   // no lane can pass 255: s is premultiplied and inv <= 256 - sa
}

// bilinear blend of four premultiplied pixels, weights ux, uy in 0..256
static inline uint32_t lerp4(uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11, uint32_t ux, uint32_t uy)
{
    if (!(p00 | p10 | p01 | p11)) return 0;
    const uint32_t vx = 256 - ux, vy = 256 - uy;
    const uint32_t rbT = (((p00 & 0x00FF00FFu) * vx + (p10 & 0x00FF00FFu) * ux) >> 8) & 0x00FF00FFu;
    const uint32_t agT = ((((p00 >> 8) & 0x00FF00FFu) * vx + ((p10 >> 8) & 0x00FF00FFu) * ux) >> 8) & 0x00FF00FFu;
    const uint32_t rbB = (((p01 & 0x00FF00FFu) * vx + (p11 & 0x00FF00FFu) * ux) >> 8) & 0x00FF00FFu;
    const uint32_t agB = ((((p01 >> 8) & 0x00FF00FFu) * vx + ((p11 >> 8) & 0x00FF00FFu) * ux) >> 8) & 0x00FF00FFu;
    const uint32_t rb = ((rbT * vy + rbB * uy) >> 8) & 0x00FF00FFu;
    const uint32_t ag = ((agT * vy + agB * uy) >> 8) & 0x00FF00FFu;
    return rb | (ag << 8);
}

// The x range [lo, hi] of a row where a + b x stays inside (min, max); false when none.
static inline bool clipLinear(float a, float b, float mn, float mx, float& lo, float& hi)
{
    if (std::fabs(b) < 1e-7f) return a > mn && a < mx;
    float x1 = (mn - a) / b, x2 = (mx - a) / b;
    if (x1 > x2) std::swap(x1, x2);
    lo = std::max(lo, x1); hi = std::min(hi, x2);
    return lo <= hi;
}

void drawSprite(Surface& s, const Sprite& sp, float cx, float cy, float angle, float alpha, float sx, float sy)
{
    if (!s.px || sp.w <= 0 || sp.h <= 0 || alpha <= 0.0f || sx <= 0.01f || sy <= 0.01f) return;
    const uint32_t k = alpha >= 1.0f ? 256u : static_cast<uint32_t>(alpha * 256.0f + 0.5f);
    if (!k) return;
    const float c = std::cos(angle), sn = std::sin(angle);
    const float hw = sp.w * 0.5f, hh = sp.h * 0.5f;
    const float ex = std::fabs(c) * hw * sx + std::fabs(sn) * hh * sy, ey = std::fabs(sn) * hw * sx + std::fabs(c) * hh * sy;
    // screen to sprite: turned back by the angle, then divided by the scale
    const float ux = c / sx, uy = -sn / sy, vx = sn / sx, vy = c / sy;
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - ex))), x1 = std::min(s.w - 1, static_cast<int>(std::ceil(cx + ex)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - ey))), y1 = std::min(s.h - 1, static_cast<int>(std::ceil(cy + ey)));
    if (x0 > x1 || y0 > y1) return;
    const int W = sp.w, H = sp.h;
    const uint32_t* src = sp.px.data();
    // The sprite pixel under the centre of screen pixel x is (ux x + bx, uy x + by): a straight line
    // along each row, walked in 16.16 fixed point. Only the part of the row where the sample touches
    // the sprite is visited.
    const int32_t dfx = static_cast<int32_t>(ux * 65536.0f), dfy = static_cast<int32_t>(uy * 65536.0f);
    for (int y = y0; y <= y1; ++y)
    {
        const float dy = y + 0.5f - cy;
        const float bx = ux * (0.5f - cx) + vx * dy + hw - 0.5f;
        const float by = uy * (0.5f - cx) + vy * dy + hh - 0.5f;
        float lo = static_cast<float>(x0), hi = static_cast<float>(x1);
        if (!clipLinear(bx, ux, -1.0f, static_cast<float>(W), lo, hi)) continue;
        if (!clipLinear(by, uy, -1.0f, static_cast<float>(H), lo, hi)) continue;
        const int xs = std::max(x0, static_cast<int>(std::ceil(lo))), xe = std::min(x1, static_cast<int>(std::floor(hi)));
        if (xs > xe) continue;
        int32_t fx = static_cast<int32_t>((ux * xs + bx) * 65536.0f), fy = static_cast<int32_t>((uy * xs + by) * 65536.0f);
        uint32_t* row = s.px + static_cast<size_t>(y) * s.stride;
        for (int x = xs; x <= xe; ++x, fx += dfx, fy += dfy)
        {
            const int ix = fx >> 16, iy = fy >> 16;   // floor (arithmetic shift)
            if (ix < -1 || iy < -1 || ix >= W || iy >= H) continue;
            const uint32_t ux = (static_cast<uint32_t>(fx) >> 8) & 0xFF, uy = (static_cast<uint32_t>(fy) >> 8) & 0xFF;
            uint32_t p00, p10, p01, p11;
            if (ix >= 0 && iy >= 0 && ix + 1 < W && iy + 1 < H)
            {
                const uint32_t* q = src + static_cast<size_t>(iy) * W + ix;
                p00 = q[0]; p10 = q[1]; p01 = q[W]; p11 = q[W + 1];
            }
            else
            {
                auto at = [&](int px, int py) -> uint32_t {
                    return (px < 0 || py < 0 || px >= W || py >= H) ? 0u : src[static_cast<size_t>(py) * W + px];
                };
                p00 = at(ix, iy); p10 = at(ix + 1, iy); p01 = at(ix, iy + 1); p11 = at(ix + 1, iy + 1);
            }
            const uint32_t p = lerp4(p00, p10, p01, p11, ux, uy);
            if (p >> 24) blendOverK(row[x], p, k);
        }
    }
}

void fillRectF(Surface& s, float l, float t, float r, float b, COLORREF c, float alpha)
{
    if (!s.px || r <= l || b <= t || alpha <= 0) return;
    int x0 = std::max(0, static_cast<int>(std::floor(l))), x1 = std::min(s.w - 1, static_cast<int>(std::ceil(r)) - 1);
    int y0 = std::max(0, static_cast<int>(std::floor(t))), y1 = std::min(s.h - 1, static_cast<int>(std::ceil(b)) - 1);
    if (x0 > x1 || y0 > y1) return;
    const uint32_t opaque = 0xFF000000u | (static_cast<uint32_t>(GetRValue(c)) << 16) | (static_cast<uint32_t>(GetGValue(c)) << 8) | GetBValue(c);
    const float al = std::min(1.0f, alpha);
    for (int y = y0; y <= y1; ++y)
    {
        const float covY = std::min(b, y + 1.0f) - std::max(t, static_cast<float>(y));
        uint32_t* row = s.px + static_cast<size_t>(y) * s.stride;
        // the inner columns are fully covered: one weight for the whole row
        const uint32_t kIn = static_cast<uint32_t>(std::min(1.0f, covY * al) * 256.0f + 0.5f);
        for (int x = x0; x <= x1; ++x)
        {
            uint32_t kk = kIn;
            if (x == x0 || x == x1)
            {
                const float covX = std::min(r, x + 1.0f) - std::max(l, static_cast<float>(x));
                kk = static_cast<uint32_t>(std::min(1.0f, covX * covY * al) * 256.0f + 0.5f);
            }
            if (kk) blendOverK(row[x], opaque, kk);
        }
    }
}

} // namespace kex
