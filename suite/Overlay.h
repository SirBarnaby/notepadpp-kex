// Overlay.h - per-pixel-alpha layered windows and a tiny software sprite renderer.
#pragma once
#include "KexSuite.h"

namespace kex {

// A 32-bit premultiplied BGRA DIB section that grows on demand.
struct Surface {
    int w = 0, h = 0, stride = 0, capH = 0;
    uint32_t* px = nullptr;
    HBITMAP bmp = nullptr;
    HDC dc = nullptr;
    HGDIOBJ old = nullptr;
    bool ensure(int width, int height);
    void clear();
    void release();
    ~Surface() { release(); }
};

struct Sprite {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // premultiplied BGRA
};

struct TextRun {
    std::wstring text;
    int dx = 0;                 // x offset inside the sprite (before padding)
    COLORREF color = 0;
    HFONT font = nullptr;
};

// Click-through, non-activating layered popup. owner may be null (then it is topmost).
HWND createOverlayWindow(HWND owner, bool topmost, const wchar_t* title);
// Push the top-left (size.cx x size.cy) part of the surface to the layered window at screen pos.
bool pushLayered(HWND wnd, Surface& s, POINT pos, SIZE size);

// Render text runs into a premultiplied sprite of the given size, baseline at y = baseline; with backAlpha
// above 0, over a box of colour back the size of the text area (w x h, inside the padding).
bool renderRuns(Sprite& out, const std::vector<TextRun>& runs, int w, int h, int baseline, int pad,
                COLORREF back = 0, float backAlpha = 0);

// Composite a sprite rotated by angle (radians) and scaled by sx, sy with its centre at (cx, cy) in surface coords.
void drawSprite(Surface& s, const Sprite& sp, float cx, float cy, float angle, float alpha, float sx = 1, float sy = 1);

// Anti-aliased (fractional-edge) rectangle fill, premultiplied "over".
void fillRectF(Surface& s, float l, float t, float r, float b, COLORREF c, float alpha);

} // namespace kex
