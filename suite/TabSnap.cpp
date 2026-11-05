// TabSnap.cpp - "Tab snapping": while a tab is dragged, a translucent rounded rectangle shows where
// it will land: the part of a view a new view would open in, or the whole view it would move to.
// The core works out the target (KexViews.cpp, kexSnapTargetAt) and calls kexsuite::tabSnapPreview;
// this draws the preview and glides it from one target to the next. Esc cancels the drag.
// The moment a drag starts, the tab itself lifts off the bar (a picture of it, slightly larger, with a
// shadow) and follows the mouse until it is dropped (kexsuite::tabDragged).
#include "KexSuite.h"
#include <cmath>

namespace kex {

namespace {

const UINT_PTR TIMER_FRAME = 1;
const double GLIDE_MS = 120;
const BYTE PREVIEW_ALPHA = 110;
const UINT_PTR TIMER_LIFT = 2;
const double LIFT_MS = 130, DROP_MS = 110;
const BYTE LIFT_ALPHA = 225;
const double LIFT_SCALE = 1.06;
SnapStats s_stats;
double s_stretch = 1.0;   // what the designed times are multiplied by: the snapping's own speed and the speed for all

RECT lerpRect(const RECT& a, const RECT& b, double t)
{
    auto l = [t](LONG x, LONG y) { return static_cast<LONG>(x + (y - x) * t + (y >= x ? 0.5 : -0.5)); };
    return RECT{ l(a.left, b.left), l(a.top, b.top), l(a.right, b.right), l(a.bottom, b.bottom) };
}

// The dragged tab, lifted off the bar: a picture of it in a click-through window that follows the mouse.
class TabLift {
public:
    void create()
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc; wc.hInstance = hInst; wc.lpszClassName = L"KexSuiteTabLift";
            wc.style = CS_DROPSHADOW;   // Windows' own shadow, as under a menu
            wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
            registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        _wnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            L"KexSuiteTabLift", L"KEX dragged tab", WS_POPUP, 0, 0, 1, 1, npp._nppHandle, nullptr, hInst, nullptr);
        if (_wnd) { ::SetWindowLongPtrW(_wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this)); ::SetLayeredWindowAttributes(_wnd, 0, 0, LWA_ALPHA); }
    }

    void destroy()
    {
        drop(true);
        if (_wnd) { ::DestroyWindow(_wnd); _wnd = nullptr; }
        freeImage();
    }

    void start(HWND tabBar, bool animate)
    {
        if (!_wnd || !tabBar) return;
        const int idx = static_cast<int>(::SendMessageW(tabBar, TCM_GETCURSEL, 0, 0));
        RECT r{};
        if (idx < 0 || !::SendMessageW(tabBar, TCM_GETITEMRECT, idx, reinterpret_cast<LPARAM>(&r))) return;
        ::MapWindowPoints(tabBar, nullptr, reinterpret_cast<POINT*>(&r), 2);
        RECT bar; ::GetWindowRect(tabBar, &bar);
        ::IntersectRect(&r, &r, &bar);
        if (r.right - r.left < 4 || r.bottom - r.top < 4) return;
        // what the tab looks like on screen right now (CAPTUREBLT: the window may be layered, as with the glass)
        freeImage();
        _w = r.right - r.left; _h = r.bottom - r.top;
        HDC screen = ::GetDC(nullptr);
        _dc = ::CreateCompatibleDC(screen);
        _bmp = ::CreateCompatibleBitmap(screen, _w, _h);
        _old = ::SelectObject(_dc, _bmp);
        ::BitBlt(_dc, 0, 0, _w, _h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
        ::ReleaseDC(nullptr, screen);
        POINT c; ::GetCursorPos(&c);
        _grab = POINT{ c.x - r.left, c.y - r.top };
        _home = r;
        _bar = tabBar;
        _animate = animate;
        _start = nowMs();
        _dropping = false;
        _overTabs = true;
        _shown = true;
        ++s_stats.lifts;
        ::InvalidateRect(_wnd, nullptr, FALSE);
        frame();
        ::ShowWindow(_wnd, SW_SHOWNOACTIVATE);
        ::SetTimer(_wnd, TIMER_LIFT, 15, nullptr);
    }

    void follow(bool overTabs)
    {
        if (!_shown || _dropping) return;
        _overTabs = overTabs;
        frame();
    }

    void drop(bool now)
    {
        if (!_shown) return;
        if (now || !_animate)
        {
            ::KillTimer(_wnd, TIMER_LIFT);
            ::ShowWindow(_wnd, SW_HIDE);
            ::SetLayeredWindowAttributes(_wnd, 0, 0, LWA_ALPHA);
            _shown = _dropping = false;
            s_stats.lifted = false;
            return;
        }
        if (!_dropping) { _dropping = true; _dropStart = nowMs(); _dropFrom = _alpha; }
    }

    void frame()
    {
        if (!_shown) return;
        // Esc while the tab is lifted (also before any snap preview shows): let go of the tab, as the preview does
        if (!_dropping && (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) && _bar && ::GetCapture() == _bar)
        {
            ::ReleaseCapture();   // the tab bar sees its capture end and drops nothing
            drop(false);
            ++s_stats.cancels;
            log(L"tab lift: drag cancelled with Esc");
            return;
        }
        double t = _animate ? std::min(1.0, (nowMs() - _start) / (LIFT_MS * s_stretch)) : 1.0;
        const double e = 1 - (1 - t) * (1 - t) * (1 - t);   // ease out
        const double scale = 1 + (LIFT_SCALE - 1) * e;
        const int lift = static_cast<int>(::MulDiv(3, dpiOf(_wnd), 96) * e + 0.5);
        int alpha = static_cast<int>(LIFT_ALPHA * (_animate ? std::min(1.0, t * 1.5) : 1.0));
        if (_dropping)
        {
            const double d = std::min(1.0, (nowMs() - _dropStart) / (DROP_MS * s_stretch));
            alpha = static_cast<int>(_dropFrom * (1 - d));
            if (d >= 1) { drop(true); return; }
        }
        const int w = static_cast<int>(_w * scale + 0.5), h = static_cast<int>(_h * scale + 0.5);
        POINT c; ::GetCursorPos(&c);
        // the point that was grabbed stays under the mouse; along the tab bar the tab slides in its row
        int x = c.x - static_cast<int>(_grab.x * scale + 0.5);
        int y = c.y - static_cast<int>(_grab.y * scale + 0.5) - lift;
        if (_overTabs) y = _home.top - (h - _h) / 2 - lift;
        ::SetWindowPos(_wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        if (w != _cw || h != _ch) { _cw = w; _ch = h; ::InvalidateRect(_wnd, nullptr, FALSE); }
        if (alpha != _alpha) { ::SetLayeredWindowAttributes(_wnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA); _alpha = alpha; }
        s_stats.lifted = true;
        s_stats.lift = RECT{ x, y, x + w, y + h };
        s_stats.liftAlpha = alpha;
        if (!_dropping && t >= 1) ::SetTimer(_wnd, TIMER_LIFT, 50, nullptr);   // settled: the drag moves it
    }

    HWND wnd() const { return _wnd; }

private:
    void freeImage()
    {
        if (_dc) { ::SelectObject(_dc, _old); ::DeleteDC(_dc); _dc = nullptr; }
        if (_bmp) { ::DeleteObject(_bmp); _bmp = nullptr; }
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        TabLift* self = reinterpret_cast<TabLift*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (m)
        {
        case WM_TIMER:
            if (w == TIMER_LIFT && self) { try { self->frame(); } catch (...) {} }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps; HDC dc = ::BeginPaint(h, &ps);
            RECT rc; ::GetClientRect(h, &rc);
            if (self && self->_dc)
            {
                ::SetStretchBltMode(dc, HALFTONE);
                ::SetBrushOrgEx(dc, 0, 0, nullptr);
                ::StretchBlt(dc, 0, 0, rc.right, rc.bottom, self->_dc, 0, 0, self->_w, self->_h, SRCCOPY);
            }
            HBRUSH edge = ::CreateSolidBrush(::GetSysColor(COLOR_HIGHLIGHT));   // picked up: an accent outline
            ::FrameRect(dc, &rc, edge);
            ::DeleteObject(edge);
            ::EndPaint(h, &ps);
            return 0;
        }
        }
        return ::DefWindowProcW(h, m, w, l);
    }

    HWND _wnd = nullptr, _bar = nullptr;
    HDC _dc = nullptr;
    HBITMAP _bmp = nullptr;
    HGDIOBJ _old = nullptr;
    int _w = 0, _h = 0, _cw = 0, _ch = 0, _alpha = 0, _dropFrom = 0;
    POINT _grab{};
    RECT _home{};
    bool _shown = false, _dropping = false, _animate = true, _overTabs = true;
    double _start = 0, _dropStart = 0;
};

class TabSnap;
TabSnap* s_snap = nullptr;

class TabSnap : public Module {
public:
    const wchar_t* key() const override { return L"TabSnap"; }
    const wchar_t* title() const override { return L"Tab snapping"; }

    void loadOptions() override
    {
        _animate = settings.getInt(key(), L"Animate", 1) != 0;
        _liftOn = settings.getInt(key(), L"LiftTab", 1) != 0;
        s_stretch = animMs(1000, animSpeed(key(), L"Speed")) / 1000.0;
    }

    bool selfCheck(std::wstring& why) override
    {
        // the targets come from the KEX core (NPPM_INTERNAL_KEX_TABDRAG from the tab bars)
        if (!views.patched()) { why = L"needs the KEX four-view Notepad++"; return false; }
        return true;
    }

    bool enable() override
    {
        loadOptions();
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc; wc.hInstance = hInst; wc.lpszClassName = L"KexSuiteSnapPreview";
            wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
            registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        // click-through (WindowFromPoint looks past it), never activated, above the views it covers
        _wnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            L"KexSuiteSnapPreview", L"KEX tab snapping", WS_POPUP, 0, 0, 1, 1, npp._nppHandle, nullptr, hInst, nullptr);
        if (!_wnd) return false;
        ::SetLayeredWindowAttributes(_wnd, 0, 0, LWA_ALPHA);
        _lift.create();
        s_snap = this;
        return true;
    }

    void disable() override
    {
        hide();
        _lift.destroy();
        if (_wnd) { ::DestroyWindow(_wnd); _wnd = nullptr; }
        if (s_snap == this) s_snap = nullptr;
        s_stats = SnapStats();
    }

    void preview(const RECT* rc)
    {
        if (!_wnd) return;
        if (!rc) { hide(); return; }
        const int inset = ::MulDiv(5, dpiOf(npp._nppHandle), 96);   // a little margin, like Windows' snap zones
        RECT to = *rc;
        if (to.right - to.left > 4 * inset && to.bottom - to.top > 4 * inset) ::InflateRect(&to, -inset, -inset);
        s_stats.target = *rc;
        if (!_shown)
        {
            _to = to;
            _start = nowMs();
            if (_animate)
            {
                // grow out of the middle of the target while fading in
                const LONG dx = (to.right - to.left) / 12, dy = (to.bottom - to.top) / 12;
                _from = RECT{ to.left + dx, to.top + dy, to.right - dx, to.bottom - dy };
            }
            else _from = to;
            _cur = RECT{};
            _shown = true;
            _shownTime = nowMs();
            ++s_stats.shows;
            place(_from, _animate ? 0 : PREVIEW_ALPHA);
            ::ShowWindow(_wnd, SW_SHOWNOACTIVATE);
            ::SetTimer(_wnd, TIMER_FRAME, 15, nullptr);
            frame();
        }
        else if (!::EqualRect(&to, &_to))
        {
            _from = _cur;
            _to = to;
            _start = nowMs();
            ++s_stats.moves;
            frame();
        }
    }

    void dragged(HWND tabBar, int state)
    {
        switch (state)
        {
        case 3: if (_liftOn) _lift.start(tabBar, _animate); break;   // KEX_TABDRAG_STARTED
        case 1: _lift.follow(false); break;                           // KEX_TABDRAG_MOVED
        case 2: _lift.follow(true); break;                            // KEX_TABDRAG_OVERTABS
        default: _lift.drop(false); break;                            // KEX_TABDRAG_ENDED
        }
    }

    void frame()
    {
        if (!_shown) return;
        // Esc while dragging: let go of the tab (the tab bar sees its capture end and drops nothing)
        if (::GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        {
            HWND cap = ::GetCapture();
            wchar_t cls[64] = {};
            if (cap) ::GetClassNameW(cap, cls, 63);
            hide();
            if (cap && ::lstrcmpiW(cls, WC_TABCONTROLW) == 0) ::ReleaseCapture();
            ++s_stats.cancels;
            log(L"tab snapping: drag cancelled with Esc");
            return;
        }
        double t = _animate ? (nowMs() - _start) / (GLIDE_MS * s_stretch) : 1.0;
        if (t > 1) t = 1;
        const double e = 1 - (1 - t) * (1 - t) * (1 - t);   // ease out
        int alpha = PREVIEW_ALPHA;
        if (_animate) alpha = static_cast<int>(PREVIEW_ALPHA * std::min(1.0, (nowMs() - _shownTime) / (GLIDE_MS * s_stretch)));
        place(lerpRect(_from, _to, e), static_cast<BYTE>(alpha));
    }

    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_TIMER:
            if (w == TIMER_FRAME && s_snap) { try { s_snap->frame(); } catch (...) {} }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps; HDC dc = ::BeginPaint(h, &ps);
            RECT rc; ::GetClientRect(h, &rc);
            const int dpi = dpiOf(h);
            const COLORREF accent = ::GetSysColor(COLOR_HIGHLIGHT);
            HBRUSH fill = ::CreateSolidBrush(mixColour(accent, RGB(255, 255, 255), 0.45));
            HPEN pen = ::CreatePen(PS_INSIDEFRAME, std::max(2, ::MulDiv(3, dpi, 96)), accent);
            HGDIOBJ ob = ::SelectObject(dc, fill), op = ::SelectObject(dc, pen);
            const int r = ::MulDiv(12, dpi, 96);
            ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
            ::SelectObject(dc, ob); ::SelectObject(dc, op);
            ::DeleteObject(fill); ::DeleteObject(pen);
            ::EndPaint(h, &ps);
            return 0;
        }
        }
        return ::DefWindowProcW(h, m, w, l);
    }

private:
    void place(const RECT& rc, BYTE alpha)
    {
        const int w = std::max(1L, rc.right - rc.left), h = std::max(1L, rc.bottom - rc.top);
        const bool resized = (w != _cur.right - _cur.left) || (h != _cur.bottom - _cur.top);
        const bool moved = resized || rc.left != _cur.left || rc.top != _cur.top;
        if (moved)
        {
            ::SetWindowPos(_wnd, HWND_TOPMOST, rc.left, rc.top, w, h, SWP_NOACTIVATE | (resized ? 0 : SWP_NOSIZE));
            if (resized)
            {
                const int r = ::MulDiv(12, dpiOf(_wnd), 96);
                ::SetWindowRgn(_wnd, ::CreateRoundRectRgn(0, 0, w + 1, h + 1, r, r), TRUE);
                ::InvalidateRect(_wnd, nullptr, FALSE);
            }
            _cur = rc;
        }
        if (alpha != _alpha) { ::SetLayeredWindowAttributes(_wnd, 0, alpha, LWA_ALPHA); _alpha = alpha; }
        if (!_animate || (::EqualRect(&_cur, &_to) && _alpha == PREVIEW_ALPHA))
            ::SetTimer(_wnd, TIMER_FRAME, 50, nullptr);   // settled: only watch for Esc
        else
            ::SetTimer(_wnd, TIMER_FRAME, 15, nullptr);
        s_stats.visible = true;
        s_stats.window = _cur;
        s_stats.alpha = _alpha;
    }

    void hide()
    {
        if (!_wnd || !_shown) return;
        ::KillTimer(_wnd, TIMER_FRAME);
        ::ShowWindow(_wnd, SW_HIDE);
        _shown = false;
        _alpha = 0;
        ::SetLayeredWindowAttributes(_wnd, 0, 0, LWA_ALPHA);
        s_stats.visible = false;
    }

    HWND _wnd = nullptr;
    TabLift _lift;
    bool _animate = true, _shown = false, _liftOn = true;
    RECT _from{}, _to{}, _cur{};
    double _start = 0, _shownTime = 0;
    BYTE _alpha = 0;
};

} // namespace

Module* createTabSnap() { return new TabSnap(); }
SnapStats snapStats()
{
    SnapStats st = s_stats;
    st.glideMs = static_cast<int>(std::lround(GLIDE_MS * s_stretch));
    return st;
}

} // namespace kex

namespace kexsuite {

bool tabSnapOn()
{
    kex::Module* m = kex::modules[kex::M_SNAP];
    return m && m->isEnabled();
}

void tabDragged(HWND tabBar, int state)
{
    try { if (kex::s_snap) kex::s_snap->dragged(tabBar, state); }
    catch (...) { kex::log(L"tab snapping: exception while lifting the tab"); }
}

void tabSnapPreview(const RECT* screenRect)
{
    try { if (kex::s_snap) kex::s_snap->preview(screenRect); }
    catch (...) { kex::log(L"tab snapping: exception while drawing the preview"); }
}

} // namespace kexsuite
