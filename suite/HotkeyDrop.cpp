// HotkeyDrop.cpp - quick note: a global hotkey (RegisterHotKey, default Ctrl+Alt+M) fades a small
// Notepad++-styled note window in at the centre of the screen (or where it was left), over whatever
// program is in front and whether or not Notepad++ is minimised. Pressing it again fades the note out
// and gives the focus back; if you had clicked away from it, the window you clicked keeps the focus.
// The text is saved as you type, to kex-quicknote.txt in Notepad++'s settings folder or to a file of
// the user's choosing (kept in its own encoding and line endings). The note has no title bar: a thin
// strip along its top moves it, the other edges and corners resize it. A small pin at the right of that
// strip keeps the note where it is (it opens there next time); pinned, the same button sends it back to
// the middle of the screen. Its editor gets the smooth caret, the falling letters, the copy flash and the
// editing motion, as the views do, and keeps the indentation of the line above when Enter is pressed.
// With "Zoom in as it opens" on (off by default) the note grows from 94 % to its size as it fades in, on a
// spring without bounce, and shrinks a little as it fades out: a picture of it on a click-through layered
// window plays that, the note itself shows once it is in place, and typing ends it at once.
//
// Virtual desktops: the note is a tool window, and Windows shows tool windows on every desktop, so it
// stays in front when the user switches desktops. Hiding it never hands the focus to a window on another
// desktop (that would make Windows switch back to that desktop): the focus goes back to the window it came
// from only when that window is on the desktop being shown, otherwise to the next window down on this
// desktop, or to the desktop itself.
#include "KexSuite.h"
#include "KexScintilla.h"
#include "Effects.h"
#include <dwmapi.h>
#include <cmath>
#include <climits>
#include <cstring>

namespace kex {

namespace {

const int HOTKEY_ID = 0x4B45;
const UINT_PTR TIMER_FADE = 1, TIMER_SAVE = 2, TIMER_TAKEOVER = 3, TIMER_GLIDE = 4, TIMER_ZOOM = 5;
const double FADE_MS = 150.0;
const int FRAME96 = 7;                 // the edge around the text: the sides and bottom resize it
const int TOP96 = 16;                  // the strip along the top: moves it, holds the grip and the pin
const int PIN96 = 22;                  // the pin button's width
const double GLIDE_RESPONSE_S = 0.32;  // the glide back to the middle: a spring without bounce
const wchar_t* NOTE_CLASS = L"KexQuickNote";
HotkeyStats s_stats;
double s_zoomFreeze = -1;              // self-test: the zoom stops at this fraction

// a critically damped spring from rest, settled (to 0.5 %) at u = 1
double springTo(double u)
{
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    return 1.0 - (1.0 + 7.43 * u) * std::exp(-7.43 * u);
}

// A window the focus can go to without Windows switching to another virtual desktop: shown, not
// minimised, and on the desktop the user is looking at (a window on another desktop is cloaked).
bool focusableHere(HWND h)
{
    if (!h || !::IsWindow(h) || !::IsWindowVisible(h) || ::IsIconic(h) || isCloaked(h)) return false;
    DesktopProvider* p = desktopProvider();
    return !p || !p->ok() || p->isOnCurrent(h);
}

std::wstring keyName(UINT vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) { wchar_t b[8]; swprintf(b, 8, L"F%u", vk - VK_F1 + 1); return b; }
    UINT sc = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t b[64] = {};
    LONG lp = static_cast<LONG>(sc << 16);
    switch (vk) { case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
                  case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: lp |= 1 << 24; break; }
    if (::GetKeyNameTextW(lp, b, 63) > 0) return b;
    swprintf(b, 64, L"VK 0x%02X", vk);
    return b;
}

class QuickNote : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"HotkeyDropDown"; }   // INI section kept from the drop-down
    const wchar_t* title() const override { return L"Quick note (global hotkey)"; }

    void loadOptions() override
    {
        _mods = static_cast<UINT>(settings.getInt(key(), L"Modifiers", MOD_CONTROL | MOD_ALT));
        _vk = static_cast<UINT>(settings.getInt(key(), L"Key", 'M'));
        _keepText = settings.getInt(key(), L"KeepText", 1) != 0;
        _fade = settings.getInt(key(), L"Fade", 1) != 0;
        _w96 = std::max(240, settings.getInt(key(), L"Width", 640));
        _h96 = std::max(140, settings.getInt(key(), L"Height", 420));
        _lineNumbers = settings.getInt(key(), L"LineNumbers", 0) != 0;
        _wrap = settings.getInt(key(), L"WordWrap", 1) != 0;
        _caretLine = settings.getInt(key(), L"CurrentLine", 0) != 0;
        _onTop = settings.getInt(key(), L"OnTop", 1) != 0;
        _remember = settings.getInt(key(), L"RememberPlace", 0) != 0;
        _x = settings.getInt(key(), L"X", INT_MIN);
        _y = settings.getInt(key(), L"Y", INT_MIN);
        _zoom = settings.getInt(key(), L"Zoom", INT_MIN);
        _smoothCaret = settings.getInt(key(), L"SmoothCaret", 1) != 0;
        _zoomOpen = settings.getInt(key(), L"ZoomOpen", 0) != 0;
        _zoomOpenMs = std::min(5120, animMs(320, animSpeed(key(), L"ZoomSpeed", 320, L"ZoomOpenMs")));
        _autoHide = settings.getInt(key(), L"HideWhenInactive", 0) != 0;
        _opacity = std::max(30, std::min(100, settings.getInt(key(), L"Opacity", 100)));
        const std::wstring f = noteFileSetting();
        if (f != _file)
        {
            // another note file: what is typed so far goes to the old one, the note shows the new one
            if (_sci && !_closing) save();
            _file = f;
            if (_sci && !_closing) load();
        }
        if (_sci && !_closing) applyEffects();
        if (_note && !_closing) ::InvalidateRect(_note, nullptr, FALSE);   // the pin shows the setting
    }

    // the file the note keeps its text in: the chosen one (environment variables expanded) or ours
    static std::wstring noteFileSetting()
    {
        const std::wstring set = settings.getStr(L"HotkeyDropDown", L"NoteFile");
        if (set.empty()) return settings.dir() + L"\\kex-quicknote.txt";
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = ::ExpandEnvironmentStringsW(set.c_str(), buf, MAX_PATH * 2);
        return (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf) : set;
    }
    bool ownFile() const { return settings.getStr(key(), L"NoteFile").empty(); }

    bool selfCheck(std::wstring& why) override
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        if (!::GetClassInfoExW(::GetModuleHandleW(nullptr), L"Scintilla", &wc))
        { why = L"Notepad++'s Scintilla window class is not registered"; return false; }
        wchar_t cls[64] = {};
        ::GetClassNameW(npp._nppHandle, cls, 63);
        if (wcscmp(cls, L"Notepad++")) { why = L"unexpected main window class"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexHotkey")) return false;
        registerKey();
        if (!s_stats.registered && s_stats.byOtherNpp)
            ::SetTimer(hwnd(), TIMER_TAKEOVER, 3000, nullptr);   // take the hotkey over once that window closes
        else if (!s_stats.registered && !selfTestMode)
            showNote(L"KEX: the quick note hotkey " + s_stats.text + L" is already taken by another program. Pick another one in KEX > Quick Note > Settings.", 8000);
        return true;
    }

    void disable() override
    {
        if (hwnd()) ::KillTimer(hwnd(), TIMER_TAKEOVER);
        if (hwnd() && s_stats.registered) ::UnregisterHotKey(hwnd(), HOTKEY_ID);
        s_stats.registered = false;
        destroyNote(true);
        zoomEnd();
        destroyMsgWindow();
        if (_glyphFont) { ::DeleteObject(_glyphFont); _glyphFont = nullptr; _glyphPx = 0; }
    }

    bool registerKey()
    {
        if (!hwnd()) return false;
        if (s_stats.registered) ::UnregisterHotKey(hwnd(), HOTKEY_ID);
        s_stats.registered = ::RegisterHotKey(hwnd(), HOTKEY_ID, _mods | MOD_NOREPEAT, _vk) != FALSE;
        s_stats.lastError = s_stats.registered ? 0 : ::GetLastError();
        if (!s_stats.registered && s_stats.lastError == ERROR_INVALID_PARAMETER)
        {
            // MOD_NOREPEAT needs Windows 7; retry without it
            s_stats.registered = ::RegisterHotKey(hwnd(), HOTKEY_ID, _mods, _vk) != FALSE;
            s_stats.lastError = s_stats.registered ? 0 : ::GetLastError();
        }
        s_stats.mods = _mods; s_stats.vk = _vk;
        // With a Notepad++ for each desktop, the first window holds the hotkey and opens the note on
        // whichever desktop the user is on.
        s_stats.byOtherNpp = !s_stats.registered && s_stats.lastError == ERROR_HOTKEY_ALREADY_REGISTERED && otherNotepadPlusPlus();
        s_stats.text = text(_mods, _vk);
        log(L"hotkey %ls: %ls (error %lu)", s_stats.text.c_str(), s_stats.registered ? L"registered" : L"NOT registered", s_stats.lastError);
        return s_stats.registered;
    }

    static bool otherNotepadPlusPlus()
    {
        HWND h = nullptr;
        while ((h = ::FindWindowExW(nullptr, h, L"Notepad++", nullptr)) != nullptr)
        {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid != ::GetCurrentProcessId()) return true;
        }
        return false;
    }

    static std::wstring text(UINT mods, UINT vk)
    {
        std::wstring t;
        if (mods & MOD_CONTROL) t += L"Ctrl+";
        if (mods & MOD_ALT) t += L"Alt+";
        if (mods & MOD_SHIFT) t += L"Shift+";
        if (mods & MOD_WIN) t += L"Win+";
        return t + keyName(vk);
    }

    void press()
    {
        ++s_stats.presses;
        if (_note && isCloaked(_note)) destroyNote(true);     // left open on another desktop: start again here
        if (!_note) { open(); return; }
        if (_closing) { fadeTo(maxAlpha()); _closing = false; front(); return; }
        HWND fg = ::GetForegroundWindow();
        if (fg && (fg == _note || ::GetAncestor(fg, GA_ROOT) == _note)) close();
        // clicked away but still on screen: one press hides it, and the window you clicked keeps the focus
        else if (onScreen()) close(false);
        else front();   // only when other windows cover it all ("Always on top" off)
    }

    HWND noteWindow() const { return _closing ? nullptr : _note; }
    HWND editor() const { return _closing ? nullptr : _sci; }
    std::wstring file() const { return _file.empty() ? noteFileSetting() : _file; }
    std::wstring fileError() const { return _fileError; }

    // KEX > Quick Note > Open Note File in Notepad++
    void openFileInNpp()
    {
        if (_sci && !_closing) { _dirty = true; save(); }   // a file that doesn't exist yet is made first
        const std::wstring f = file();
        if (::GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            HANDLE h = ::CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        }
        if (_note && !_closing) close();
        nppMsg(NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(f.c_str()));
    }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_HOTKEY && w == HOTKEY_ID) { log(L"WM_HOTKEY received"); press(); return 0; }
        if (m == WM_TIMER && w == TIMER_ZOOM) { zoomStep(); return 0; }
        if (m == WM_TIMER && w == TIMER_TAKEOVER)
        {
            if (registerKey() || !s_stats.byOtherNpp) { ::KillTimer(hwnd(), TIMER_TAKEOVER); refreshPrefPages(); }
            return 0;
        }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    // ---- the note window ------------------------------------------------------------------------
    void open()
    {
        if (!registerClass()) return;
        HWND fg = ::GetForegroundWindow();
        // only a window on this desktop gets the focus back later (right after a switch to an empty desktop
        // Windows can still name a window of the desktop that was left)
        _prevFg = (fg && fg != _note && focusableHere(fg)) ? fg : nullptr;
        if (fg && fg != _note && !_prevFg) ++s_stats.otherDesktopFg;

        POINT pt{}; ::GetCursorPos(&pt);
        MONITORINFO mi{ sizeof(mi) };
        ::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        RECT wa = mi.rcWork;
        int dpi = dpiAt(pt);   // the monitor it opens on (Notepad++ is per-monitor DPI aware)
        int w = std::min<int>(::MulDiv(_w96, dpi, 96), wa.right - wa.left - 40);
        int h = std::min<int>(::MulDiv(_h96, dpi, 96), wa.bottom - wa.top - 40);
        int x = wa.left + (wa.right - wa.left - w) / 2;
        int y = wa.top + (wa.bottom - wa.top - h) / 2;
        if (_remember && _x != INT_MIN && _y != INT_MIN)
        {
            // where it was left, if that is still on a screen
            POINT tl{ _x, _y }, br{ _x + w - 1, _y + h - 1 };
            if (::MonitorFromPoint(tl, MONITOR_DEFAULTTONULL) && ::MonitorFromPoint(br, MONITOR_DEFAULTTONULL))
            {
                x = _x; y = _y;
                const int there = dpiAt(tl);
                if (there != dpi) { w = ::MulDiv(w, there, dpi); h = ::MulDiv(h, there, dpi); }
            }
        }

        std::wstring caption = L"Quick note  (" + s_stats.text + L" or Esc to hide)";
        // Layered for its whole life when it fades: switching the style on a shown window is what made
        // the fade skip now and then. (The self-test runs the zoom under Wine too, which has no partial alpha.)
        zoomEnd();
        _layered = (_fade || _opacity < 100 || _zoomOpen) && (!binaryAlpha() || (selfTestMode && _zoomOpen));
        DWORD ex = WS_EX_TOOLWINDOW | (_onTop ? WS_EX_TOPMOST : 0) | (_layered ? WS_EX_LAYERED : 0);
        // Unowned, so it shows even while Notepad++ is minimised; created fresh each time, so it opens on
        // the desktop you are looking at. No title bar: the edge around the text moves and sizes it.
        _note = ::CreateWindowExW(ex, NOTE_CLASS, caption.c_str(), WS_POPUP | WS_CLIPCHILDREN,
                                  x, y, w, h, nullptr, nullptr, hInst, this);
        if (!_note) { log(L"quick note: CreateWindowEx failed (%lu)", ::GetLastError()); return; }
        if (nppMsg(NPPM_ISDARKMODEENABLED, 0, 0))
        {
            BOOL dark = TRUE;
            ::DwmSetWindowAttribute(_note, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
        _sci = ::CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                 _note, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!_sci) { log(L"quick note: Scintilla failed (%lu)", ::GetLastError()); destroyNote(false); return; }
        ::SetWindowSubclass(_sci, editorSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
        makeTooltip();
        styleLike(views.sci(views.current()));
        _closing = false;
        load();
        applyEffects();
        layout();
        restoreView();

        const bool appear = _fade || _zoomOpen;
        if (_layered) { _alpha = appear ? 0 : maxAlpha(); ::SetLayeredWindowAttributes(_note, 0, static_cast<BYTE>(_alpha), LWA_ALPHA); }
        else _alpha = 255;
        ::ShowWindow(_note, SW_SHOWNA);
        // paint the text before the first visible frame, so the fade shows the note and not a blank box
        ::RedrawWindow(_note, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        front();
        if (_layered && _zoomOpen && zoomBegin(true)) {}
        else if (_layered && appear) fadeTo(maxAlpha());
        s_stats.noteOpen = true;
        log(L"quick note: opened at %d,%d %dx%d, previous window %p", x, y, w, h, _prevFg);
    }

    void front()
    {
        if (!_note) return;
        ::SetWindowPos(_note, _onTop ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        ::SetForegroundWindow(_note);
        ::SetFocus(_sci);
    }

    // can any of the note be seen? Samples a grid over it and asks which window is on top at each point
    bool onScreen() const
    {
        if (!_note || !::IsWindowVisible(_note)) return false;
        RECT r{}; ::GetWindowRect(_note, &r);
        const int N = 6;
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j)
            {
                POINT pt{ r.left + 2 + static_cast<int>((r.right - r.left - 5) * i / (N - 1)),
                          r.top + 2 + static_cast<int>((r.bottom - r.top - 5) * j / (N - 1)) };
                if (!::MonitorFromPoint(pt, MONITOR_DEFAULTTONULL)) continue;
                HWND at = ::WindowFromPoint(pt);
                if (at && (at == _note || ::GetAncestor(at, GA_ROOT) == _note)) return true;
            }
        return false;
    }

    void close(bool giveFocusBack = true)
    {
        if (!_note || _closing) return;
        save();
        RECT r{}; ::GetWindowRect(_note, &r);
        int dpi = dpiOf(_note);
        settings.setInt(key(), L"Width", ::MulDiv(r.right - r.left, 96, dpi));
        settings.setInt(key(), L"Height", ::MulDiv(r.bottom - r.top, 96, dpi));
        settings.setInt(key(), L"X", r.left);
        settings.setInt(key(), L"Y", r.top);
        if (_sci) settings.setInt(key(), L"Zoom", static_cast<int>(sci(_sci, SCI_GETZOOM)));
        saveView();
        loadOptions();
        stopGlide();
        _closing = true;
        s_stats.noteOpen = false;
        HWND to = nullptr;
        if (giveFocusBack) to = handFocusOn();
        log(L"quick note: closed, focus to %p (came from %p)", to, _prevFg);
        if (_layered && _zoomOpen && zoomBegin(false)) destroyNote(false);   // the picture of it zooms out
        else if (_layered && _fade) fadeTo(0);
        else destroyNote(false);
    }

    // The note is about to go while it has the focus: give the focus to a window on this desktop (the one
    // it came from when it is here), so Windows doesn't pick one itself, maybe on another desktop.
    HWND handFocusOn()
    {
        HWND to = focusableHere(_prevFg) ? _prevFg : focusFallback();
        if (to != _prevFg) ++s_stats.focusFallbacks;
        if (to) ::SetForegroundWindow(to);
        s_stats.lastFocusTo = to;
        return to;
    }

    // the first ordinary window below the note on this desktop, or the desktop itself
    HWND focusFallback() const
    {
        for (HWND h = _note ? ::GetWindow(_note, GW_HWNDNEXT) : ::GetTopWindow(nullptr); h; h = ::GetWindow(h, GW_HWNDNEXT))
        {
            if (h == _note || !focusableHere(h)) continue;
            const LONG_PTR ex = ::GetWindowLongPtrW(h, GWL_EXSTYLE), st = ::GetWindowLongPtrW(h, GWL_STYLE);
            if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) continue;   // the taskbar, overlays, palettes
            if ((ex & WS_EX_NOACTIVATE) || (st & WS_DISABLED)) continue;
            RECT r{};
            if (!::GetWindowRect(h, &r) || r.right - r.left < 2 || r.bottom - r.top < 2) continue;
            return h;
        }
        return ::GetShellWindow();   // the desktop: on every virtual desktop
    }

    void destroyNote(bool saveFirst)
    {
        if (!_note) return;
        if (saveFirst && !_closing) { save(); saveView(); }
        stopGlide();
        HWND n = _note;
        if (!_closing && ::GetForegroundWindow() == n) handFocusOn();
        _note = nullptr; _sci = nullptr; _closing = false; _tip = nullptr;
        _hover = _pressed = false;
        s_stats.noteOpen = false;
        ::DestroyWindow(n);
    }

    void fadeTo(int target)
    {
        _alphaFrom = _alpha; _alphaTo = target; _fadeT0 = nowMs();
        ::SetTimer(_note, TIMER_FADE, 10, nullptr);
        fadeStep();
    }

    void fadeStep()
    {
        double t = std::min(1.0, (nowMs() - _fadeT0) / (FADE_MS * animScale()));
        double e = 1.0 - (1.0 - t) * (1.0 - t);
        _alpha = static_cast<int>(std::lround(_alphaFrom + (_alphaTo - _alphaFrom) * e));
        ::SetLayeredWindowAttributes(_note, 0, static_cast<BYTE>(_alpha), LWA_ALPHA);
        if (t < 1.0) return;
        ::KillTimer(_note, TIMER_FADE);
        if (_alphaTo == 0) destroyNote(false);
    }

    // ---- zoom: a picture of the note grows into place (opening) or shrinks away (closing) ----
    bool zoomBegin(bool opening)
    {
        zoomEnd();
        if (!_note || !hwnd()) return false;
        RECT r{};
        ::GetWindowRect(_note, &r);
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (w < 8 || h < 8) return false;
        // what the note shows, as Windows composes it (the editor draws with Direct2D)
        Surface shot;
        if (!shot.ensure(w, h)) return false;
        shot.clear();
        if (!::PrintWindow(_note, shot.dc, 2 /* PW_RENDERFULLCONTENT */) && !::PrintWindow(_note, shot.dc, 0)) return false;
        ::GdiFlush();
        _zoomPic.w = w;
        _zoomPic.h = h;
        _zoomPic.px.resize(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) _zoomPic.px[static_cast<size_t>(y) * w + x] = shot.px[static_cast<size_t>(y) * shot.stride + x] | 0xFF000000u;
        _zoomOverlay = createOverlayWindow(nullptr, _onTop, L"KEX quick note zoom");
        if (!_zoomOverlay) { _zoomPic.px.clear(); return false; }
        _zoomAt = POINT{ r.left, r.top };
        _zoomOpening = opening;
        _zoomT0 = nowMs();
        _zoomPeak = maxAlpha() / 255.0;
        ++s_stats.zooms;
        s_stats.zoomOpening = opening;
        s_stats.zoomLive = true;
        zoomStep();
        ::SetWindowPos(_zoomOverlay, _onTop ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::SetTimer(hwnd(), TIMER_ZOOM, 10, nullptr);
        return true;
    }

    void zoomStep()
    {
        if (!_zoomOverlay) { if (hwnd()) ::KillTimer(hwnd(), TIMER_ZOOM); return; }
        // going is a little quicker than coming, as Apple's sheets are
        const double ms = _zoomOpening ? _zoomOpenMs : _zoomOpenMs * 0.75;
        double t = (nowMs() - _zoomT0) / ms;
        if (s_zoomFreeze >= 0) t = std::min(t, s_zoomFreeze);
        t = std::max(0.0, std::min(1.0, t));
        double scale, alpha;
        if (_zoomOpening)
        {
            scale = 0.94 + 0.06 * springTo(t);
            const double f = std::min(1.0, t / 0.6);
            alpha = 1.0 - (1.0 - f) * (1.0 - f) * (1.0 - f);
        }
        else
        {
            scale = 1.0 - 0.04 * t * t;
            alpha = (1.0 - t) * (1.0 - t);
        }
        alpha *= _zoomPeak;
        const int w = _zoomPic.w, h = _zoomPic.h;
        if (_zoomFrame.ensure(w, h))
        {
            _zoomFrame.clear();
            drawSprite(_zoomFrame, _zoomPic, w * 0.5f, h * 0.5f, 0.0f, static_cast<float>(alpha), static_cast<float>(scale), static_cast<float>(scale));
            pushLayered(_zoomOverlay, _zoomFrame, _zoomAt, SIZE{ w, h });
        }
        ++s_stats.zoomFrames;
        s_stats.zoomScale = static_cast<float>(scale);
        s_stats.zoomAlpha = static_cast<float>(alpha);
        if (t >= 1.0) zoomEnd();
    }

    // the picture goes; an opening note shows itself (at once when typing cut the zoom short)
    void zoomEnd()
    {
        if (hwnd()) ::KillTimer(hwnd(), TIMER_ZOOM);
        if (!_zoomOverlay) return;
        if (_zoomOpening && _note && !_closing && _layered)
        {
            ::KillTimer(_note, TIMER_FADE);
            _alpha = maxAlpha();
            ::SetLayeredWindowAttributes(_note, 0, static_cast<BYTE>(_alpha), LWA_ALPHA);
        }
        ::DestroyWindow(_zoomOverlay);
        _zoomOverlay = nullptr;
        _zoomPic.px.clear();
        _zoomPic.px.shrink_to_fit();
        _zoomFrame.release();
        s_stats.zoomLive = false;
    }

    int frame() const { return ::MulDiv(FRAME96, dpiOf(_note), 96); }
    int topStrip() const { return ::MulDiv(TOP96, dpiOf(_note), 96); }

    void layout()
    {
        if (!_note || !_sci) return;
        RECT rc{}; ::GetClientRect(_note, &rc);
        const int f = frame(), t = topStrip();
        ::MoveWindow(_sci, f, t, std::max(0L, rc.right - 2 * f), std::max(0L, rc.bottom - t - f), TRUE);
        if (_tip)
        {
            TOOLINFOW ti{ sizeof(ti) };
            ti.hwnd = _note; ti.uId = 1; ti.rect = pinRect();
            ::SendMessageW(_tip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }

    // ---- the pin: keep the note here / back to the middle ------------------------------------------

    // its place: the right end of the top strip, short of the corner that resizes the note
    RECT pinRect() const
    {
        RECT rc{}; ::GetClientRect(_note, &rc);
        const int f = frame(), w = ::MulDiv(PIN96, dpiOf(_note), 96), right = rc.right - 2 * f - ::MulDiv(2, dpiOf(_note), 96);
        return RECT{ right - w, 0, right, topStrip() };
    }

    bool inPin(POINT clientPt) const { const RECT r = pinRect(); return ::PtInRect(&r, clientPt) != FALSE; }

    void makeTooltip()
    {
        _tip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, _note, nullptr, hInst, nullptr);
        if (!_tip) return;
        TOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = _note; ti.uId = 1; ti.rect = pinRect();
        _tipText = pinTip();
        ti.lpszText = const_cast<wchar_t*>(_tipText.c_str());
        ::SendMessageW(_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }

    std::wstring pinTip() const
    {
        return _remember ? L"Back to the middle of the screen (the note stops opening here)"
                         : L"Keep the note here: it opens in this spot next time";
    }

    void updateTip()
    {
        if (!_tip) return;
        _tipText = pinTip();
        TOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = _note; ti.uId = 1;
        ti.lpszText = const_cast<wchar_t*>(_tipText.c_str());
        ::SendMessageW(_tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }

public:
    // the pin's click: pin the note where it is, or (pinned) send it back to the middle and unpin it
    void togglePin()
    {
        if (!_note || _closing) return;
        if (!_remember)
        {
            RECT r{}; ::GetWindowRect(_note, &r);
            settings.setInt(key(), L"RememberPlace", 1);
            settings.setInt(key(), L"X", r.left);
            settings.setInt(key(), L"Y", r.top);
            ++s_stats.pins;
            log(L"quick note: pinned at %d,%d", r.left, r.top);
        }
        else
        {
            settings.setInt(key(), L"RememberPlace", 0);
            ++s_stats.resets;
            glideTo(middleSpot());
            log(L"quick note: back to the middle, unpinned");
        }
        loadOptions();
        updateTip();
        ::InvalidateRect(_note, nullptr, FALSE);
        refreshPrefPages();
        if (_sci) ::SetFocus(_sci);
    }

    bool pinned() const { return _remember; }
    RECT pinScreenRect() const
    {
        RECT r = _note ? pinRect() : RECT{};
        if (_note) ::MapWindowPoints(_note, nullptr, reinterpret_cast<POINT*>(&r), 2);
        return r;
    }
    bool gliding() const { return _glideT0 >= 0; }

private:
    // where an unpinned note opens: the middle of the work area of the screen it is on
    POINT middleSpot() const
    {
        RECT r{}; ::GetWindowRect(_note, &r);
        MONITORINFO mi{ sizeof(mi) };
        ::GetMonitorInfoW(::MonitorFromWindow(_note, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& wa = mi.rcWork;
        return POINT{ wa.left + (wa.right - wa.left - (r.right - r.left)) / 2, wa.top + (wa.bottom - wa.top - (r.bottom - r.top)) / 2 };
    }

    // a critically damped spring (no bounce, as Apple's default) from where the note is to `to`
    void glideTo(POINT to)
    {
        RECT r{}; ::GetWindowRect(_note, &r);
        _glideFrom = POINT{ r.left, r.top };
        _glideTarget = to;
        _glideT0 = nowMs();
        if (!fx::animationsOn()) { glideStep(true); return; }   // Windows' "Show animations" off: straight there
        ::SetTimer(_note, TIMER_GLIDE, 10, nullptr);
        glideStep(false);
    }

    void glideStep(bool finish)
    {
        if (_glideT0 < 0 || !_note) return;
        const double t = (nowMs() - _glideT0) / 1000.0 / animScale(), w = 2.0 * 3.14159265358979 / GLIDE_RESPONSE_S;
        double p = 1.0 - (1.0 + w * t) * std::exp(-w * t);
        if (finish || p > 0.998) p = 1.0;
        const int x = static_cast<int>(std::lround(_glideFrom.x + (_glideTarget.x - _glideFrom.x) * p));
        const int y = static_cast<int>(std::lround(_glideFrom.y + (_glideTarget.y - _glideFrom.y) * p));
        ::SetWindowPos(_note, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (p >= 1.0) stopGlide();
    }

    void stopGlide()
    {
        if (_note) ::KillTimer(_note, TIMER_GLIDE);
        _glideT0 = -1;
    }

    // the glyph fonts Windows has for its own pins (Windows 11, then Windows 10); none under Wine
    HFONT glyphFont(int px)
    {
        if (_glyphFont && _glyphPx == px) return _glyphFont;
        if (_glyphFont) ::DeleteObject(_glyphFont);
        _glyphFont = nullptr;
        _glyphPx = px;
        for (const wchar_t* face : { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" })
        {
            HFONT f = ::CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
            if (!f) continue;
            HDC dc = ::GetDC(nullptr);
            HGDIOBJ old = ::SelectObject(dc, f);
            wchar_t got[LF_FACESIZE] = {};
            ::GetTextFaceW(dc, LF_FACESIZE, got);
            ::SelectObject(dc, old);
            ::ReleaseDC(nullptr, dc);
            if (!_wcsicmp(got, face)) { _glyphFont = f; break; }
            ::DeleteObject(f);
        }
        return _glyphFont;
    }

    void paintPin(HDC dc, COLORREF back, COLORREF fore)
    {
        const RECT r = pinRect();
        const int dpi = dpiOf(_note);
        if (_hover || _pressed)
        {
            // a soft rounded plate under the pointer, as Windows' own small title-bar buttons have
            HBRUSH hb = ::CreateSolidBrush(mixColour(back, fore, _pressed ? 0.18 : 0.10));
            HPEN np = static_cast<HPEN>(::GetStockObject(NULL_PEN));
            HGDIOBJ ob = ::SelectObject(dc, hb), op = ::SelectObject(dc, np);
            const int rr = ::MulDiv(6, dpi, 96);
            ::RoundRect(dc, r.left, r.top + ::MulDiv(1, dpi, 96), r.right + 1, r.bottom, rr, rr);
            ::SelectObject(dc, ob); ::SelectObject(dc, op);
            ::DeleteObject(hb);
        }
        const COLORREF accent = mixColour(::GetSysColor(COLOR_HIGHLIGHT), fore, 0.15);
        const COLORREF ink = _remember ? (_hover ? fore : accent) : mixColour(back, fore, _hover ? 0.85 : 0.45);
        // pinned: a filled pin, which turns into "unpin" under the pointer; not pinned: an outline pin
        const wchar_t glyph = _remember ? (_hover ? 0xE77A : 0xE841) : 0xE718;
        if (HFONT f = glyphFont(::MulDiv(10, dpi, 96)))
        {
            HGDIOBJ of = ::SelectObject(dc, f);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, ink);
            RECT tr = r;
            ::DrawTextW(dc, &glyph, 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            ::SelectObject(dc, of);
            return;
        }
        // no glyph font: a small pin drawn by hand (head, needle; filled when pinned; struck through to unpin)
        const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, u = std::max(1, ::MulDiv(1, dpi, 96));
        HPEN pen = ::CreatePen(PS_SOLID, u, ink);
        HBRUSH fill = _remember && !_hover ? ::CreateSolidBrush(ink) : static_cast<HBRUSH>(::GetStockObject(NULL_BRUSH));
        HGDIOBJ op = ::SelectObject(dc, pen), ob = ::SelectObject(dc, fill);
        ::Ellipse(dc, cx - 3 * u, cy - 5 * u, cx + 3 * u + 1, cy + u + 1);
        ::MoveToEx(dc, cx, cy + u, nullptr); ::LineTo(dc, cx, cy + 5 * u);
        if (_remember && _hover) { ::MoveToEx(dc, cx - 5 * u, cy - 5 * u, nullptr); ::LineTo(dc, cx + 5 * u, cy + 5 * u); }
        ::SelectObject(dc, op); ::SelectObject(dc, ob);
        ::DeleteObject(pen);
        if (_remember && !_hover) ::DeleteObject(fill);
    }

    // the edge around the text, in the editor's background with a hairline in a quieter text colour
    void paintFrame(HDC dc)
    {
        RECT rc{}; ::GetClientRect(_note, &rc);
        const COLORREF back = _sci ? static_cast<COLORREF>(sci(_sci, SCI_STYLEGETBACK, STYLE_DEFAULT)) : RGB(255, 255, 255);
        const COLORREF fore = _sci ? static_cast<COLORREF>(sci(_sci, SCI_STYLEGETFORE, STYLE_DEFAULT)) : RGB(0, 0, 0);
        // drawn off screen and put up in one go, so the pin's hover doesn't flicker
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bmp = ::CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ oldBmp = ::SelectObject(mem, bmp);
        HBRUSH bb = ::CreateSolidBrush(back);
        ::FillRect(mem, &rc, bb);
        ::DeleteObject(bb);
        HBRUSH lb = ::CreateSolidBrush(mixColour(back, fore, 0.35));
        ::FrameRect(mem, &rc, lb);
        ::DeleteObject(lb);
        // a short grip in the middle of the top strip says "drag here"
        const int t = topStrip(), gw = ::MulDiv(36, dpiOf(_note), 96);
        RECT g{ (rc.right - gw) / 2, t / 2 - 1, (rc.right + gw) / 2, t / 2 + 1 };
        HBRUSH gb = ::CreateSolidBrush(mixColour(back, fore, 0.30));
        ::FillRect(mem, &g, gb);
        ::DeleteObject(gb);
        paintPin(mem, back, fore);
        ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, oldBmp);
        ::DeleteObject(bmp);
        ::DeleteDC(mem);
    }

    LRESULT hitTest(HWND h, LPARAM l)
    {
        POINT pt{ static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)) };
        ::ScreenToClient(h, &pt);
        RECT rc{}; ::GetClientRect(h, &rc);
        const int f = frame(), c = f * 2, t = topStrip();
        const bool left = pt.x < f, right = pt.x >= rc.right - f, top = pt.y < f, bottom = pt.y >= rc.bottom - f;
        const bool nearLeft = pt.x < c, nearRight = pt.x >= rc.right - c, nearTop = pt.y < c, nearBottom = pt.y >= rc.bottom - c;
        if ((top && nearLeft) || (left && nearTop)) return HTTOPLEFT;
        if ((top && nearRight) || (right && nearTop)) return HTTOPRIGHT;
        if ((bottom && nearLeft) || (left && nearBottom)) return HTBOTTOMLEFT;
        if ((bottom && nearRight) || (right && nearBottom)) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (bottom) return HTBOTTOM;
        if (pt.y < t) return inPin(pt) ? HTCLIENT : HTCAPTION;
        return HTCLIENT;
    }

    void updateMargins()
    {
        if (!_sci) return;
        if (!_lineNumbers) { sci(_sci, SCI_SETMARGINWIDTHN, 0, 0); return; }
        const LRESULT lines = sci(_sci, SCI_GETLINECOUNT);
        int digits = 1;
        for (LRESULT n = lines; n >= 10; n /= 10) ++digits;
        std::string sample = "_" + std::string(static_cast<size_t>(std::max(2, digits)), '9');
        const LRESULT w = sci(_sci, SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<LPARAM>(sample.c_str()));
        if (w != _marginWidth) { _marginWidth = w; sci(_sci, SCI_SETMARGINWIDTHN, 0, w + 4); }
    }

    // Enter keeps the indentation of the line above, as Notepad++ does with auto-indent on (the same
    // whitespace, so tabs stay tabs)
    void keepIndent(int ch)
    {
        if (!_sci || _loading) return;
        const LRESULT eol = sci(_sci, SCI_GETEOLMODE);
        if (!(((eol == SC_EOL_CRLF || eol == SC_EOL_LF) && ch == '\n') || (eol == SC_EOL_CR && ch == '\r'))) return;
        if (!nppMsg(NPPM_ISAUTOINDENTON, 0, 0) && !selfTestMode) return;
        if (sci(_sci, SCI_GETSELECTIONS) != 1) return;
        const LRESULT caret = sci(_sci, SCI_GETCURRENTPOS);
        const LRESULT line = sci(_sci, SCI_LINEFROMPOSITION, caret);
        if (line < 1 || caret != sci(_sci, SCI_POSITIONFROMLINE, line)) return;
        const LRESULT from = sci(_sci, SCI_POSITIONFROMLINE, line - 1), to = sci(_sci, SCI_GETLINEINDENTPOSITION, line - 1);
        if (to <= from) return;
        std::string ws(static_cast<size_t>(to - from) + 1, '\0');
        Sci_TextRangeFull tr{ { from, to }, &ws[0] };
        sci(_sci, SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&tr));
        ws.resize(static_cast<size_t>(to - from));
        // the new line's own leading blanks (Enter in the middle of indentation) give way to the copy
        const LRESULT ownEnd = sci(_sci, SCI_GETLINEINDENTPOSITION, line);
        sci(_sci, SCI_SETTARGETRANGE, caret, ownEnd);
        sci(_sci, SCI_REPLACETARGET, ws.size(), reinterpret_cast<LPARAM>(ws.c_str()));
        sci(_sci, SCI_GOTOPOS, caret + static_cast<LRESULT>(ws.size()));
        ++s_stats.indents;
    }

    // Look like the editor the user is in: font, colours, caret, selection, rendering, zoom.
    void styleLike(HWND src)
    {
        HWND s = _sci;
        sci(s, SCI_SETCODEPAGE, SC_CP_UTF8);
        if (src)
        {
            // DirectWrite drawn through a DC: an HWND render target doesn't show in a layered window
            const LRESULT tech = sci(src, SCI_GETTECHNOLOGY);
            sci(s, SCI_SETTECHNOLOGY, tech == SC_TECHNOLOGY_DEFAULT ? SC_TECHNOLOGY_DEFAULT : SC_TECHNOLOGY_DIRECTWRITEDC);
            char font[128] = {};
            if (sci(src, SCI_STYLEGETFONT, STYLE_DEFAULT, 0) < static_cast<LRESULT>(sizeof(font)))
                sci(src, SCI_STYLEGETFONT, STYLE_DEFAULT, reinterpret_cast<LPARAM>(font));
            if (font[0]) sci(s, SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<LPARAM>(font));
            sci(s, SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, sci(src, SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT));
            sci(s, SCI_STYLESETFORE, STYLE_DEFAULT, sci(src, SCI_STYLEGETFORE, STYLE_DEFAULT));
            sci(s, SCI_STYLESETBACK, STYLE_DEFAULT, sci(src, SCI_STYLEGETBACK, STYLE_DEFAULT));
            sci(s, SCI_STYLECLEARALL);
            sci(s, SCI_SETCARETFORE, sci(src, SCI_GETCARETFORE));
            sci(s, SCI_SETCARETWIDTH, std::max<LRESULT>(1, sci(src, SCI_GETCARETWIDTH)));
            sci(s, SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, sci(src, SCI_GETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK));
            sci(s, SCI_SETZOOM, _zoom != INT_MIN ? _zoom : sci(src, SCI_GETZOOM));
            sci(s, SCI_STYLESETFORE, STYLE_LINENUMBER, sci(src, SCI_STYLEGETFORE, STYLE_LINENUMBER));
            sci(s, SCI_STYLESETBACK, STYLE_LINENUMBER, sci(src, SCI_STYLEGETBACK, STYLE_LINENUMBER));
            // Tab and indentation as in the editor: tab width, tabs or spaces, Tab/Backspace on indentation
            sci(s, SCI_SETTABWIDTH, std::max<LRESULT>(1, sci(src, SCI_GETTABWIDTH)));
            sci(s, SCI_SETUSETABS, sci(src, SCI_GETUSETABS));
            sci(s, SCI_SETINDENT, sci(src, SCI_GETINDENT));
            sci(s, SCI_SETTABINDENTS, sci(src, SCI_GETTABINDENTS));
            sci(s, SCI_SETBACKSPACEUNINDENTS, sci(src, SCI_GETBACKSPACEUNINDENTS));
            if (_caretLine)
            {
                LRESULT back = sci(src, SCI_GETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK);
                if (!back) back = sci(src, SCI_GETCARETLINEBACK) | 0xFF000000;
                sci(s, SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, back);
            }
        }
        for (int m = 0; m < 5; ++m) sci(s, SCI_SETMARGINWIDTHN, m, 0);
        sci(s, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
        sci(s, SCI_SETMARGINLEFT, 0, _lineNumbers ? 4 : 6);
        sci(s, SCI_SETMARGINRIGHT, 0, 6);
        sci(s, SCI_SETWRAPMODE, _wrap ? SC_WRAP_WORD : SC_WRAP_NONE);
        sci(s, SCI_SETHSCROLLBAR, _wrap ? 0 : 1);
        sci(s, SCI_SETSCROLLWIDTHTRACKING, 1);
        sci(s, SCI_SETSCROLLWIDTH, 1);
        sci(s, SCI_SETCARETLINEVISIBLE, _caretLine ? 1 : 0);
        sci(s, SCI_SETCARETLINEVISIBLEALWAYS, _caretLine ? 1 : 0);
        if (!_caretLine) sci(s, SCI_RESETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK);
        sci(s, SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_MOD_BEFOREDELETE);
        _marginWidth = -1;
    }

    // the smooth caret, as the views have it (when that feature is on)
    void applyEffects()
    {
        if (_sci) { smoothCaretApply(_sci, _smoothCaret); applyMotions(_sci); }
    }

    int maxAlpha() const { return _opacity * 255 / 100; }

    enum Encoding { ENC_UTF8 = 0, ENC_UTF8_BOM, ENC_UTF16LE, ENC_UTF16BE, ENC_ANSI };

    static FILETIME writeTime(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return FILETIME{};
        return fa.ftLastWriteTime;
    }

    // a file's bytes as UTF-8 for the editor, remembering the encoding to write it back in
    static std::string decode(const std::string& raw, int& enc)
    {
        auto wideToUtf8 = [](const wchar_t* w, int n) {
            std::string out;
            const int len = ::WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
            if (len > 0) { out.resize(static_cast<size_t>(len)); ::WideCharToMultiByte(CP_UTF8, 0, w, n, &out[0], len, nullptr, nullptr); }
            return out;
        };
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF)
        { enc = ENC_UTF8_BOM; return raw.substr(3); }
        if (raw.size() >= 2 && (static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE))
        { enc = ENC_UTF16LE; std::wstring w((raw.size() - 2) / 2, L'\0'); memcpy(&w[0], raw.data() + 2, w.size() * 2); return wideToUtf8(w.data(), static_cast<int>(w.size())); }
        if (raw.size() >= 2 && (static_cast<unsigned char>(raw[0]) == 0xFE && static_cast<unsigned char>(raw[1]) == 0xFF))
        {
            enc = ENC_UTF16BE;
            std::wstring w((raw.size() - 2) / 2, L'\0');
            for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<wchar_t>((static_cast<unsigned char>(raw[2 + 2 * i]) << 8) | static_cast<unsigned char>(raw[3 + 2 * i]));
            return wideToUtf8(w.data(), static_cast<int>(w.size()));
        }
        if (raw.empty() || ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0) > 0)
        { enc = ENC_UTF8; return raw; }
        enc = ENC_ANSI;
        const int n = ::MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
        if (n > 0) ::MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), &w[0], n);
        return wideToUtf8(w.data(), static_cast<int>(w.size()));
    }

    static std::string encode(const std::string& utf8, int enc)
    {
        if (enc == ENC_UTF8) return utf8;
        if (enc == ENC_UTF8_BOM) return "\xEF\xBB\xBF" + utf8;
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
        if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], n);
        if (enc == ENC_ANSI)
        {
            const int len = ::WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(std::max(0, len)), '\0');
            if (len > 0) ::WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
            return out;
        }
        std::string out(enc == ENC_UTF16LE ? "\xFF\xFE" : "\xFE\xFF");
        for (wchar_t c : w)
        {
            const char lo = static_cast<char>(c & 0xFF), hi = static_cast<char>((c >> 8) & 0xFF);
            if (enc == ENC_UTF16LE) { out += lo; out += hi; } else { out += hi; out += lo; }
        }
        return out;
    }

    void load()
    {
        std::string text;
        _encoding = ENC_UTF8;
        _fileError.clear();
        const bool keep = _keepText || !ownFile();   // a chosen file is always the note's text
        const std::wstring path = file();
        if (keep)
        {
            HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER sz{};
                if (::GetFileSizeEx(f, &sz) && sz.QuadPart < (16 << 20))
                {
                    std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
                    DWORD got = 0;
                    if (!raw.empty() && !::ReadFile(f, &raw[0], static_cast<DWORD>(raw.size()), &got, nullptr)) got = 0;
                    raw.resize(got);
                    text = decode(raw, _encoding);
                }
                else _fileError = L"the file is larger than 16 MB";
                ::CloseHandle(f);
            }
            else if (::GetLastError() != ERROR_FILE_NOT_FOUND && ::GetLastError() != ERROR_PATH_NOT_FOUND)
            {
                wchar_t e[96]; swprintf(e, 96, L"the file can't be read (error %lu)", ::GetLastError());
                _fileError = e;
            }
        }
        // its line endings: the first one in the file (Windows' for a new note)
        int eol = SC_EOL_CRLF;
        const size_t nl = text.find_first_of("\r\n");
        if (nl != std::string::npos) eol = text[nl] == '\n' ? SC_EOL_LF : (nl + 1 < text.size() && text[nl + 1] == '\n' ? SC_EOL_CRLF : SC_EOL_CR);
        _loading = true;
        sci(_sci, SCI_SETEOLMODE, eol);
        sci(_sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
        sci(_sci, SCI_EMPTYUNDOBUFFER);
        sci(_sci, SCI_DOCUMENTEND);
        _loading = false;
        _dirty = false;
        _fileTime = writeTime(path);
        if (!_fileError.empty()) log(L"quick note: %ls: %ls", path.c_str(), _fileError.c_str());
        updateMargins();
    }

    // Where the note was scrolled to, and its caret: it opens there again next time (the text unchanged since).
    // The first line is kept as a line of the text and how far into it (wrapped), so a new width or zoom
    // still shows the same text at the top.
    void saveView()
    {
        if (!_sci) return;
        const LRESULT first = sci(_sci, SCI_GETFIRSTVISIBLELINE);
        const LRESULT line = sci(_sci, SCI_DOCLINEFROMVISIBLE, first);
        settings.setInt(key(), L"ViewLine", static_cast<int>(line));
        settings.setInt(key(), L"ViewSubLine", static_cast<int>(first - sci(_sci, SCI_VISIBLEFROMDOCLINE, line)));
        settings.setInt(key(), L"ViewX", static_cast<int>(sci(_sci, SCI_GETXOFFSET)));
        settings.setInt(key(), L"Caret", static_cast<int>(sci(_sci, SCI_GETCURRENTPOS)));
        settings.setInt(key(), L"Anchor", static_cast<int>(sci(_sci, SCI_GETANCHOR)));
        settings.setInt(key(), L"ViewLength", static_cast<int>(sci(_sci, SCI_GETLENGTH)));
    }

    // Back where it was left. Text changed by another program since (another length): the end, as before.
    bool restoreView()
    {
        if (!_sci) return false;
        const LRESULT len = sci(_sci, SCI_GETLENGTH);
        const int was = settings.getInt(key(), L"ViewLength", -1);
        if (was < 0 || was != len) return false;
        const LRESULT caret = std::clamp<LRESULT>(settings.getInt(key(), L"Caret", 0), 0, len);
        const LRESULT anchor = std::clamp<LRESULT>(settings.getInt(key(), L"Anchor", static_cast<int>(caret)), 0, len);
        sci(_sci, SCI_SETSEL, anchor, caret);
        const LRESULT lines = sci(_sci, SCI_GETLINECOUNT);
        const LRESULT line = std::clamp<LRESULT>(settings.getInt(key(), L"ViewLine", 0), 0, lines - 1);
        const LRESULT sub = std::clamp<LRESULT>(settings.getInt(key(), L"ViewSubLine", 0), 0, sci(_sci, SCI_WRAPCOUNT, line) - 1);
        sci(_sci, SCI_SETFIRSTVISIBLELINE, sci(_sci, SCI_VISIBLEFROMDOCLINE, line) + sub);
        if (!_wrap) sci(_sci, SCI_SETXOFFSET, std::max(0, settings.getInt(key(), L"ViewX", 0)));
        sci(_sci, SCI_CHOOSECARETX);
        return true;
    }

    void save()
    {
        if (_note) ::KillTimer(_note, TIMER_SAVE);
        if ((!_keepText && ownFile()) || !_sci || !_dirty || !_fileError.empty()) return;   // never overwrite a file we couldn't read
        LRESULT len = sci(_sci, SCI_GETLENGTH);
        std::string text(static_cast<size_t>(len) + 1, '\0');
        sci(_sci, SCI_GETTEXT, len + 1, reinterpret_cast<LPARAM>(&text[0]));
        text.resize(static_cast<size_t>(len));
        text = encode(text, _encoding);
        std::wstring dst = file(), tmp = dst + L".kex-tmp";
        HANDLE f = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) { log(L"quick note: can't write %ls (%lu)", tmp.c_str(), ::GetLastError()); return; }
        DWORD put = 0;
        bool ok = text.empty() || (::WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size());
        ::CloseHandle(f);
        if (ok && ::MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { _dirty = false; _fileTime = writeTime(dst); }
        else { ::DeleteFileW(tmp.c_str()); log(L"quick note: saving %ls failed (%lu)", dst.c_str(), ::GetLastError()); }
    }

    // changed by another program while the note was open (and nothing typed since): show the new text
    void reloadIfChanged()
    {
        if (!_sci || _closing || _dirty) return;
        const FILETIME t = writeTime(file());
        if (::CompareFileTime(&t, &_fileTime) != 0) load();
    }

    bool registerClass()
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        if (::GetClassInfoExW(hInst, NOTE_CLASS, &wc)) return true;
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = noteProc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hIcon = reinterpret_cast<HICON>(::GetClassLongPtrW(npp._nppHandle, GCLP_HICON));
        wc.hIconSm = reinterpret_cast<HICON>(::GetClassLongPtrW(npp._nppHandle, GCLP_HICONSM));
        wc.lpszClassName = NOTE_CLASS;
        if (::RegisterClassExW(&wc)) return true;
        log(L"quick note: RegisterClassEx failed (%lu)", ::GetLastError());
        return false;
    }

    static LRESULT CALLBACK noteProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_NCCREATE)
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* self = reinterpret_cast<QuickNote*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self || self->_note != h) return ::DefWindowProcW(h, m, w, l);
        switch (m)
        {
        case WM_SIZE: self->layout(); ::InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_NCHITTEST: return self->hitTest(h, l);
        case WM_MOUSEMOVE:
        {
            const POINT pt{ static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)) };
            const bool over = self->inPin(pt);
            if (over != self->_hover)
            {
                self->_hover = over;
                const RECT r = self->pinRect();
                ::InvalidateRect(h, &r, FALSE);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            ::TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (self->_hover && !self->_pressed) { self->_hover = false; const RECT r = self->pinRect(); ::InvalidateRect(h, &r, FALSE); }
            return 0;
        case WM_LBUTTONDOWN:
        {
            const POINT pt{ static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)) };
            if (self->inPin(pt)) { self->_pressed = true; ::SetCapture(h); const RECT r = self->pinRect(); ::InvalidateRect(h, &r, FALSE); }
            return 0;
        }
        case WM_LBUTTONUP:
        {
            const POINT pt{ static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)) };
            if (!self->_pressed) return 0;
            self->_pressed = false;
            ::ReleaseCapture();
            const bool over = self->inPin(pt);
            self->_hover = over;
            if (over) self->togglePin();
            const RECT r = self->pinRect(); ::InvalidateRect(h, &r, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (self->_pressed && reinterpret_cast<HWND>(l) != h) { self->_pressed = false; ::InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_ENTERSIZEMOVE: self->stopGlide(); return 0;
        case WM_EXITSIZEMOVE:
            // pinned: the new spot is the one it opens at
            if (self->_remember)
            {
                RECT r{}; ::GetWindowRect(h, &r);
                settings.setInt(self->key(), L"X", r.left);
                settings.setInt(self->key(), L"Y", r.top);
                self->_x = r.left; self->_y = r.top;
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCAPTION) { ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(h, &ps);
            self->paintFrame(dc);
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_GETMINMAXINFO:
        {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            mmi->ptMinTrackSize.x = ::MulDiv(200, dpiOf(h), 96);
            mmi->ptMinTrackSize.y = ::MulDiv(100, dpiOf(h), 96);
            return 0;
        }
        case WM_DPICHANGED:
        {
            // moved to a monitor with another scale: Windows' suggested size, and the edge to match
            const RECT* r = reinterpret_cast<const RECT*>(l);
            ::SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->layout();
            ::InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE && self->_sci) { self->reloadIfChanged(); ::SetFocus(self->_sci); }
            // clicked away: hide (another window took the focus, so it keeps it)
            else if (LOWORD(w) == WA_INACTIVE && self->_autoHide && !self->_closing && !selfTestMode) self->close(false);
            return 0;
        case WM_CLOSE: self->close(); return 0;
        case WM_COMMAND: if (LOWORD(w) == IDCANCEL) { self->close(); return 0; } break;
        case WM_TIMER:
            if (w == TIMER_FADE) self->fadeStep();
            else if (w == TIMER_SAVE) self->save();
            else if (w == TIMER_GLIDE) self->glideStep(false);
            return 0;
        case WM_NOTIFY:
        {
            auto* n = reinterpret_cast<SCNotification*>(l);
            if (n->nmhdr.hwndFrom == self->_sci && n->nmhdr.code == SCN_MODIFIED && !self->_loading
                && (n->modificationType & SC_MOD_BEFOREDELETE) && n->length > 0)
            {
                lettersFromNote(self->_sci, n->position, n->length, n->modificationType);
                flashFromNote(self->_sci, n->position, n->length, n->modificationType);
            }
            if (n->nmhdr.hwndFrom == self->_sci && n->nmhdr.code == SCN_CHARADDED) self->keepIndent(n->ch);
            if (n->nmhdr.hwndFrom == self->_sci && n->nmhdr.code == SCN_MODIFIED && !self->_loading
                && (n->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)))
            {
                self->_dirty = true;
                ::SetTimer(h, TIMER_SAVE, 800, nullptr);
                self->updateMargins();
            }
            return 0;
        }
        }
        return ::DefWindowProcW(h, m, w, l);
    }

    static LRESULT CALLBACK editorSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref)
    {
        auto* self = reinterpret_cast<QuickNote*>(ref);
        if (m == WM_KEYDOWN && w == VK_ESCAPE) { self->close(); return 0; }
        // typing (or a click) while it zooms in: the note is there at once
        if (self->_zoomOverlay && self->_zoomOpening && (m == WM_KEYDOWN || m == WM_CHAR || m == WM_LBUTTONDOWN || m == WM_MOUSEWHEEL))
            self->zoomEnd();
        if (m == WM_NCDESTROY) ::RemoveWindowSubclass(h, editorSubclass, 1);
        return ::DefSubclassProc(h, m, w, l);
    }

    UINT _mods = MOD_CONTROL | MOD_ALT, _vk = 'M';
    bool _keepText = true, _fade = true, _closing = false, _dirty = false, _loading = false, _layered = false;
    bool _lineNumbers = false, _wrap = true, _caretLine = false, _onTop = true, _remember = false;
    bool _smoothCaret = true, _autoHide = false, _zoomOpen = false, _zoomOpening = false;
    int _zoomOpenMs = 320;
    HWND _zoomOverlay = nullptr;
    Sprite _zoomPic;
    Surface _zoomFrame;
    POINT _zoomAt{};
    double _zoomT0 = 0, _zoomPeak = 1;
    int _opacity = 100, _encoding = ENC_UTF8;
    std::wstring _file, _fileError;
    FILETIME _fileTime{};
    int _w96 = 640, _h96 = 420, _x = INT_MIN, _y = INT_MIN, _zoom = INT_MIN;
    LRESULT _marginWidth = -1;
    HWND _note = nullptr, _sci = nullptr, _prevFg = nullptr, _tip = nullptr;
    int _alpha = 255, _alphaFrom = 0, _alphaTo = 255;
    double _fadeT0 = 0;
    bool _hover = false, _pressed = false;
    std::wstring _tipText;
    POINT _glideFrom{}, _glideTarget{};
    double _glideT0 = -1;
    HFONT _glyphFont = nullptr;
    int _glyphPx = 0;
};

QuickNote* s_mod = nullptr;

} // namespace

Module* createHotkeyDrop() { s_mod = new QuickNote(); return s_mod; }
HotkeyStats hotkeyStats() { return s_stats; }
void quickNoteZoomFreeze(double fraction) { s_zoomFreeze = fraction; }
void hotkeyPress() { if (s_mod && s_mod->isEnabled()) s_mod->press(); }
std::wstring hotkeyText(UINT mods, UINT vk) { return QuickNote::text(mods, vk); }
HWND quickNoteWindow() { return s_mod ? s_mod->noteWindow() : nullptr; }
HWND quickNoteEditor() { return s_mod ? s_mod->editor() : nullptr; }
std::wstring quickNoteFile() { return s_mod ? s_mod->file() : std::wstring(); }
std::wstring quickNoteFileError() { return s_mod ? s_mod->fileError() : std::wstring(); }
void quickNoteOpenFile() { if (s_mod) s_mod->openFileInNpp(); }
void quickNoteOptionsChanged() { if (s_mod) s_mod->loadOptions(); }
void quickNoteTogglePin() { if (s_mod) s_mod->togglePin(); }
RECT quickNotePinRect() { return s_mod ? s_mod->pinScreenRect() : RECT{}; }
bool quickNoteGliding() { return s_mod && s_mod->gliding(); }

// KEX > Quick Note > Open Note File in Notepad++ (Shortcut Mapper can give it a key)
void addNoteCommands(HMENU parent, int position)
{
    static int id = 0;
    if (id || !parent) return;
    const KexCommand cmd{ 102, L"Open Note File in Notepad++", quickNoteOpenFile, false, false, false, 0 };
    KexCommandList list{ &cmd, 1, &id, position };
    if (!nppMsg(NPPM_KEX_ADDCOMMANDS, reinterpret_cast<WPARAM>(parent), reinterpret_cast<LPARAM>(&list)) || !id)
        log(L"Quick note: Notepad++ did not take the menu command");
}
// Options dialog: store and (if the feature is on) re-register. Returns true when registered.
bool hotkeyApply(UINT mods, UINT vk)
{
    if (!s_mod) return false;
    settings.setInt(s_mod->key(), L"Modifiers", static_cast<int>(mods));
    settings.setInt(s_mod->key(), L"Key", static_cast<int>(vk));
    s_mod->loadOptions();
    if (!s_mod->isEnabled())
    {
        s_stats.text = QuickNote::text(mods, vk);
        return false;
    }
    return s_mod->registerKey();
}

} // namespace kex
