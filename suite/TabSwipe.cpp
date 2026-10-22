// TabSwipe.cpp - "Touchpad tab swipe": two fingers swiped sideways on the touchpad move to the tab beside
// the current one, the way four fingers move between Windows 10's desktops. The documents follow the
// fingers: half a swipe shows half of the next tab, resting the fingers holds it there, and letting go
// finishes the switch (past half way, or after a quick flick) or slides back to where it was.
//
// Windows turns a two-finger sideways swipe into WM_MOUSEHWHEEL for the window under the pointer; the
// suite takes those from the views and their tab bars. Where the text itself can scroll sideways (a long
// line, no word wrap) the swipe scrolls it as before until it reaches the edge, as browsers do with
// their back and forward swipe; over the tab bar it always switches. Whether the fingers are still on the
// pad comes from the precision touchpad itself (raw input, HID digitizer page): wheel messages stop when
// the fingers stop moving, the touchpad's reports only when they are lifted. Without a precision touchpad
// (an older touchpad driver, or Wine) a pause in the wheel messages counts as letting go. A mouse's
// tilt wheel (whole notches) moves one tab per tilt.
//
// The picture is Scintilla's tab slide held by hand (SCI_KEX_SLIDE with no length, then SCI_KEX_SLIDEPOS,
// KexScintilla.h): the tab beside is really activated when the swipe starts, and the view shows both
// documents at whatever position the fingers give it. Sliding back puts the old tab back while the view
// still shows only its picture, so nothing flickers, and gives the tabs their Ctrl+Tab order back.
#include "KexSuite.h"
#include "KexScintilla.h"
#include <hidusage.h>
#include <hidpi.h>
#include <cmath>
#include <map>

namespace kex {

namespace {

const UINT_PTR TIMER_TICK = 1;     // letting go and sliding into place, while a swipe is under way
const UINT TICK_MS = 10;
const double QUIET_MS = 140;       // no touchpad: no wheel message for this long is letting go
const double SILENT_MS = 250;      // the touchpad sent nothing for this long: no fingers on it
const double LOCK_MS = 260;        // after a swipe, wheel messages (its last ones, inertia) do nothing for this long
const double DEADZONE = 0.04;      // of a whole swipe, before the tab beside starts to come in
const double FLICK = 2.0;          // whole swipes a second: letting go this fast finishes the switch
const double FULL_SWIPE = 800;     // wheel units (WHEEL_DELTA = 120) of a whole swipe at 100 % sensitivity
const UINT_PTR kSubclassId = 0x4B455357;   // 'KESW'
const USAGE kPageDigitizer = 0x0D, kUsageTouchPad = 0x05, kUsageTip = 0x42, kUsageContactCount = 0x54;

SwipeStats s_stats;
int s_logged = 0;
void trace(const wchar_t* what) { s_stats.trace += what; if (s_stats.trace.size() > 400) s_stats.trace.erase(0, s_stats.trace.size() - 400); }

// ---- the precision touchpad: how many fingers are on it ----------------------------------------
using GetCapsFn = NTSTATUS (NTAPI*)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
using GetButtonCapsFn = NTSTATUS (NTAPI*)(HIDP_REPORT_TYPE, PHIDP_BUTTON_CAPS, PUSHORT, PHIDP_PREPARSED_DATA);
using GetUsagesFn = NTSTATUS (NTAPI*)(HIDP_REPORT_TYPE, USAGE, USHORT, PUSAGE, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
using GetUsageValueFn = NTSTATUS (NTAPI*)(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);

class TouchPad {
public:
    bool start(HWND target)
    {
        if (!loadHid()) return false;
        RAWINPUTDEVICE rid{ kPageDigitizer, kUsageTouchPad, RIDEV_INPUTSINK, target };
        _registered = ::RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
        if (!_registered) log(L"Tab swipe: the touchpad's reports are not available (error %lu); letting go is a pause instead", ::GetLastError());
        return _registered;
    }

    void stop()
    {
        if (_registered)
        {
            RAWINPUTDEVICE rid{ kPageDigitizer, kUsageTouchPad, RIDEV_REMOVE, nullptr };
            ::RegisterRawInputDevices(&rid, 1, sizeof(rid));
            _registered = false;
        }
        _devices.clear();
        _lastReport = 0;
        _fingers = 0;
    }

    bool registered() const { return _registered; }

    // fingers on the pad: -1 when no precision touchpad has reported anything (the wheel is all there is)
    int fingers(double now) const
    {
        if (_fake >= 0) return _fake;
        if (_lastReport <= 0) return -1;
        if (now - _lastReport > SILENT_MS) return 0;
        return _fingers;
    }
    int touches() const { return _touches; }   // how many times two or more fingers went down
    void fake(int n)
    {
        const int before = fingers(nowMs());
        _fake = n;
        if (n >= 2 && before < 2) ++_touches;
    }

    // WM_INPUT. Returns true when the number of fingers changed.
    bool onInput(HRAWINPUT h)
    {
        // only while Notepad++ is in front: the reports come in from every program otherwise
        DWORD pid = 0;
        ::GetWindowThreadProcessId(::GetForegroundWindow(), &pid);
        if (pid != ::GetCurrentProcessId()) return false;
        UINT size = 0;
        if (::GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) return false;
        if (_buf.size() < size) _buf.resize(size);
        if (::GetRawInputData(h, RID_INPUT, _buf.data(), &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return false;
        const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(_buf.data());
        if (ri->header.dwType != RIM_TYPEHID) return false;
        Device* d = device(ri->header.hDevice);
        if (!d) return false;
        const int before = fingers(nowMs());
        const BYTE* report = ri->data.hid.bRawData;
        for (DWORD i = 0; i < ri->data.hid.dwCount; ++i, report += ri->data.hid.dwSizeHid)
            readReport(*d, const_cast<PCHAR>(reinterpret_cast<const char*>(report)), ri->data.hid.dwSizeHid);
        const int after = fingers(nowMs());
        if (after >= 2 && before < 2) ++_touches;
        return after != before;
    }

private:
    struct Device {
        std::vector<BYTE> pp;               // the device's preparsed report descriptor
        std::vector<USHORT> contacts;       // the link collections that hold a contact (a tip switch each)
        ULONG frameCount = 0, frameTips = 0, frameSlots = 0;   // the frame being read (hybrid devices split one over reports)
    };

    bool loadHid()
    {
        if (_getCaps) return true;
        HMODULE hid = ::LoadLibraryW(L"hid.dll");
        if (!hid) return false;
        _getCaps = reinterpret_cast<GetCapsFn>(::GetProcAddress(hid, "HidP_GetCaps"));
        _getButtonCaps = reinterpret_cast<GetButtonCapsFn>(::GetProcAddress(hid, "HidP_GetButtonCaps"));
        _getUsages = reinterpret_cast<GetUsagesFn>(::GetProcAddress(hid, "HidP_GetUsages"));
        _getUsageValue = reinterpret_cast<GetUsageValueFn>(::GetProcAddress(hid, "HidP_GetUsageValue"));
        if (_getCaps && _getButtonCaps && _getUsages && _getUsageValue) return true;
        _getCaps = nullptr;
        return false;
    }

    Device* device(HANDLE h)
    {
        auto it = _devices.find(h);
        if (it != _devices.end()) return it->second.contacts.empty() ? nullptr : &it->second;
        Device& d = _devices[h];   // a device we can't read stays in the map, empty, so it is asked once
        UINT size = 0;
        if (::GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, nullptr, &size) != 0 || size == 0) return nullptr;
        d.pp.resize(size);
        if (::GetRawInputDeviceInfoW(h, RIDI_PREPARSEDDATA, d.pp.data(), &size) == static_cast<UINT>(-1)) return nullptr;
        PHIDP_PREPARSED_DATA pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(d.pp.data());
        HIDP_CAPS caps{};
        if (_getCaps(pp, &caps) != HIDP_STATUS_SUCCESS || caps.NumberInputButtonCaps == 0) return nullptr;
        std::vector<HIDP_BUTTON_CAPS> bc(caps.NumberInputButtonCaps);
        USHORT n = caps.NumberInputButtonCaps;
        if (_getButtonCaps(HidP_Input, bc.data(), &n, pp) != HIDP_STATUS_SUCCESS) return nullptr;
        for (USHORT i = 0; i < n; ++i)
        {
            const HIDP_BUTTON_CAPS& c = bc[i];
            if (c.UsagePage != kPageDigitizer) continue;
            const bool tip = c.IsRange ? (c.Range.UsageMin <= kUsageTip && kUsageTip <= c.Range.UsageMax) : c.NotRange.Usage == kUsageTip;
            if (tip && std::find(d.contacts.begin(), d.contacts.end(), c.LinkCollection) == d.contacts.end())
                d.contacts.push_back(c.LinkCollection);
        }
        if (d.contacts.empty()) return nullptr;
        log(L"Tab swipe: precision touchpad found (%d contacts per report)", static_cast<int>(d.contacts.size()));
        return &d;
    }

    void readReport(Device& d, PCHAR report, ULONG len)
    {
        PHIDP_PREPARSED_DATA pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(d.pp.data());
        ULONG count = 0;
        if (_getUsageValue(HidP_Input, kPageDigitizer, 0, kUsageContactCount, &count, pp, report, len) != HIDP_STATUS_SUCCESS)
            return;   // not a contact report (the buttons, a configuration report)
        ULONG tips = 0;
        for (USHORT lc : d.contacts)
        {
            USAGE u[16];
            ULONG n = 16;
            if (_getUsages(HidP_Input, kPageDigitizer, lc, u, &n, pp, report, len) != HIDP_STATUS_SUCCESS) continue;
            for (ULONG i = 0; i < n; ++i) if (u[i] == kUsageTip) ++tips;
        }
        _lastReport = nowMs();
        if (count > 0)   // a new frame: Contact Count says how many contacts it holds, over one report or several
        {
            d.frameCount = count;
            d.frameTips = tips;
            d.frameSlots = static_cast<ULONG>(d.contacts.size());
        }
        else if (d.frameCount > 0)
        {
            d.frameTips += tips;
            d.frameSlots += static_cast<ULONG>(d.contacts.size());
        }
        else return;
        if (d.frameSlots >= d.frameCount)
        {
            _fingers = static_cast<int>(d.frameTips);
            d.frameCount = 0;
        }
    }

    GetCapsFn _getCaps = nullptr;
    GetButtonCapsFn _getButtonCaps = nullptr;
    GetUsagesFn _getUsages = nullptr;
    GetUsageValueFn _getUsageValue = nullptr;
    std::map<HANDLE, Device> _devices;
    std::vector<BYTE> _buf;
    bool _registered = false;
    double _lastReport = 0;
    int _fingers = 0, _touches = 0, _fake = -1;
};

// Windows' touchpad setting "Scrolling direction": by default ("Down motion scrolls up") the content
// follows the fingers, so fingers moving left scroll right (a positive WM_MOUSEHWHEEL).
bool contentFollowsFingers()
{
    DWORD v = 0, size = sizeof(v);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad", L"ScrollDirection",
                       RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return true;
    return v == 0;
}

class TabSwipe;
TabSwipe* s_self = nullptr;

class TabSwipe : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"TabSwipe"; }
    const wchar_t* title() const override { return L"Touchpad tab swipe"; }

    void loadOptions() override
    {
        _sensitivity = std::max(25, std::min(400, settings.getInt(key(), L"Sensitivity", 100)));
        _reverse = settings.getInt(key(), L"Reverse", 0) != 0;
        _settleMs = std::max(60, std::min(800, settings.getInt(key(), L"DurationMs", 220)));
    }

    bool selfCheck(std::wstring& why) override
    {
        HWND s = views.sci(0);
        if (!s) { why = L"main Scintilla view not found"; return false; }
        KexSlideInfo info{};
        info.held = -1;
        ::SendMessageW(s, SCI_KEX_GETSLIDEINFO, 0, reinterpret_cast<LPARAM>(&info));
        if (info.held < 0) { why = L"this Scintilla cannot hold a slide (not a KEX build)"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexTabSwipe")) return false;
        for (int v = 0; v < views.count(); ++v)
        {
            if (HWND s = views.sci(v)) hook(s, v, false);
            if (HWND t = views.tabBar(v)) hook(t, v, true);
        }
        _pad.start(hwnd());
        s_stats.rawRegistered = _pad.registered();
        s_self = this;
        return true;
    }

    void disable() override
    {
        finishNow();
        s_self = nullptr;
        for (HWND h : _hooked) if (::IsWindow(h)) ::RemoveWindowSubclass(h, wndProc, kSubclassId);
        _hooked.clear();
        _pad.stop();
        if (hwnd()) ::KillTimer(hwnd(), TIMER_TICK);
        destroyMsgWindow();
        _state = St::idle;
        s_stats.rawRegistered = false;
        s_stats.active = false;
    }

    void onNotify(SCNotification* n) override
    {
        switch (n->nmhdr.code)
        {
        case NPPN_FILEBEFORECLOSE: case NPPN_DOCORDERCHANGED: case NPPN_SHUTDOWN:
            finishNow();   // the tabs are changing under the swipe: stop where it is
            break;
        }
    }

    // ScintillaEditView::activateBuffer, before the view shows the other document (TabSlide.cpp's hook)
    bool switching(HWND view)
    {
        if (view == _claimSci)
        {
            _slideOk = ::SendMessageW(view, SCI_KEX_SLIDE, static_cast<WPARAM>(_claimDir), 0) != 0;
            return true;
        }
        if (view == _quietSci) return true;   // sliding back: the view keeps showing the old picture
        if (view == _sci && (_state == St::swiping || _state == St::settling))
        {
            // something else switches tabs in the middle of a swipe (a key, a click): the swipe gives way
            ::SendMessageW(view, SCI_KEX_SLIDE, 0, 0);
            ::KillTimer(hwnd(), TIMER_TICK);
            lock(nowMs());
        }
        return false;
    }

    TouchPad _pad;
    bool busy() const { return _state == St::pending || _state == St::swiping || _state == St::settling; }
    bool holding() const { return _state == St::swiping; }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_INPUT)
        {
            if (_pad.onInput(reinterpret_cast<HRAWINPUT>(l)) && (_state == St::pending || _state == St::swiping))
                checkRelease(nowMs());
            return MsgWindow::onMessage(m, w, l);   // DefWindowProc frees the input
        }
        if (m == WM_TIMER && w == TIMER_TICK) { tick(); return 0; }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    enum class St { idle, pending, text, swiping, settling, locked };

    void hook(HWND h, int view, bool tabBar)
    {
        if (::SetWindowSubclass(h, wndProc, kSubclassId, static_cast<DWORD_PTR>(view) | (tabBar ? 0x100 : 0)))
            _hooked.push_back(h);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref)
    {
        if ((m == WM_MOUSEHWHEEL || m == WM_MOUSEWHEEL) && s_self)
        {
            try
            {
                POINT pt{ static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)) };
                if (s_self->wheel(h, static_cast<int>(ref & 0xFF), (ref & 0x100) != 0, m == WM_MOUSEHWHEEL,
                                  GET_WHEEL_DELTA_WPARAM(w), GET_KEYSTATE_WPARAM(w), pt))
                    return m == WM_MOUSEHWHEEL ? 1 : 0;   // 1: handled, for mouse drivers that ask (as Scintilla answers)
            }
            catch (...) { log(L"Tab swipe: exception in a wheel message"); }
        }
        if (m == WM_NCDESTROY) ::RemoveWindowSubclass(h, wndProc, kSubclassId);
        return ::DefSubclassProc(h, m, w, l);
    }

    // the swipe so far, in whole swipes: + toward the tab on the right
    double position() const { return _acc / (FULL_SWIPE * 100.0 / _sensitivity); }

    // A wheel message for a view (or its tab bar). true = the swipe took it.
    bool wheel(HWND h, int view, bool tabBar, bool horizontal, int delta, int keys, POINT pt)
    {
        const double now = nowMs();
        // Notepad++ hands a wheel message on to the window under the pointer: act there, not here
        HWND under = kexsuite::windowFromPoint(pt);
        const bool mine = under == h || (under && ::IsChild(h, under));
        const bool inGesture = busy();
        if (!mine && !inGesture) { if (horizontal) trace(L" not-under"); return false; }
        const int fingers = _pad.fingers(now);
        const bool touch = fingers >= 2;

        if (!horizontal)
        {
            // the up-and-down part of the same swipe: the document stays put while it slides
            if (inGesture && view == _view)
            {
                if (_state == St::pending) _accV += std::abs(delta);
                if (_state == St::pending && _accV > std::abs(_accH) && !_started) { _state = St::text; return false; }
                return true;
            }
            return false;
        }
        if (keys & (MK_CONTROL | MK_SHIFT | MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) return inGesture;
        if (::GetCapture() && !inGesture) return false;   // a selection or a tab being dragged

        if (_state == St::locked)
        {
            const bool newTouch = touch && _pad.touches() != _lockTouches;
            if (!newTouch && now < _lockUntil) { _lockUntil = now + LOCK_MS; trace(L" locked"); return true; }
            _state = St::idle;
        }
        if (_state == St::settling) { _lastWheel = now; return true; }
        if (_state == St::text)
        {
            const bool over = touch ? _pad.touches() != _gestureTouches : now - _lastWheel > QUIET_MS;
            if (!over && view == _view) { _lastWheel = now; return false; }
            _state = St::idle;
        }
        if (_state == St::idle) { trace(touch ? L" begin(touch)" : L" begin(wheel)"); return begin(view, tabBar, delta, touch, now); }

        // pending or swiping: this gesture goes on
        if (view != _view) return true;
        _lastWheel = now;
        _accH += delta;
        _acc += fingerUnits(delta);
        follow(now);
        return true;
    }

    // the wheel's delta as finger movement toward the tab on the right (the user's direction setting applied)
    double fingerUnits(int delta) const
    {
        const double fingersRight = _follows ? -delta : delta;
        return _reverse ? -fingersRight : fingersRight;
    }

    bool begin(int view, bool tabBar, int delta, bool touch, double now)
    {
        HWND s = views.sci(view);
        if (!s || !::IsWindowVisible(s) || ::IsIconic(views.host(view))) return false;
        _view = view;
        _sci = s;
        _touch = touch;
        _gestureTouches = _pad.touches();
        _lastWheel = now;
        _acc = 0; _accH = 0; _accV = 0;
        _started = false;
        _history.clear();
        ++s_stats.gestures;
        // a mouse's tilt wheel: whole notches, one tab per tilt
        if (!touch && delta % WHEEL_DELTA == 0)
        {
            if (!tabBar && textCanScroll(s, delta > 0)) { _state = St::text; ++s_stats.textScrolls; return false; }
            const int t = delta > 0 ? 1 : -1;
            const int idx = views.currentIndex(view), target = idx + t;
            ++s_stats.notches;
            if (target >= 0 && target < views.nbFiles(view)) { views.activate(view, target); s_stats.lastDir = t; }
            else ++s_stats.edges;
            lock(now);
            return true;
        }
        if (!tabBar && textCanScroll(s, delta > 0)) { _state = St::text; ++s_stats.textScrolls; trace(L" text"); return false; }
        _follows = contentFollowsFingers();
        _state = St::pending;
        _accH = delta;
        _acc = fingerUnits(delta);
        _lastTick = _quietFrom = now;
        ::SetTimer(hwnd(), TIMER_TICK, TICK_MS, nullptr);
        follow(now);
        return true;
    }

    // Where the text can go on scrolling sideways, the swipe scrolls it.
    static bool textCanScroll(HWND s, bool right)
    {
        if (sci(s, SCI_GETWRAPMODE) != SC_WRAP_NONE) return false;
        if (!right && sci(s, SCI_GETXOFFSET) > 0) return true;
        SCROLLINFO si{}; si.cbSize = sizeof(si); si.fMask = SIF_ALL;
        if (!::GetScrollInfo(s, SB_HORZ, &si) || si.nPage == 0 || static_cast<int>(si.nPage) > si.nMax - si.nMin) return false;
        return right ? si.nPos < si.nMax - static_cast<int>(si.nPage) + 1 : si.nPos > si.nMin;
    }

    void follow(double now)
    {
        const double p = position();
        _history.push_back({ now, p });
        while (_history.size() > 2 && now - _history.front().t > 100) _history.erase(_history.begin());
        if (_state == St::swiping && p * _dir <= 0)
        {
            // back past where it started: the old tab returns, and the swipe may go on to the other side
            slideBack(false);
            _state = St::pending;
        }
        if (_state == St::pending)
        {
            if (std::abs(p) < DEADZONE || std::abs(_accH) < 1.5 * _accV) return;
            if (!start(p > 0 ? 1 : -1)) return;
        }
        if (_state == St::swiping) setPos(std::min(1.0, std::abs(p)));
    }

    bool start(int dir)
    {
        const int idx = views.currentIndex(_view), target = idx + dir;
        if (target < 0 || target >= views.nbFiles(_view))
        {
            if (!_edge) ++s_stats.edges;
            _edge = true;   // no tab on that side: nothing moves
            return false;
        }
        _edge = false;
        _origBuf = views.bufferId(_view, idx);
        _newBuf = views.bufferId(_view, target);
        rememberOrder();
        _claimSci = _sci;
        _claimDir = dir;
        _slideOk = false;
        views.activate(_view, target);
        _claimSci = nullptr;
        if (views.currentIndex(_view) != target) { trace(L" activate-failed"); lock(nowMs()); return false; }
        _started = true;
        trace(L" start");
        ++s_stats.slides;
        s_stats.lastDir = dir;
        if (!_slideOk)
        {
            // the view couldn't take its picture (hidden, no size): the tab is switched, without the slide
            ++s_stats.commits;
            lock(nowMs());
            return false;
        }
        _dir = dir;
        _state = St::swiping;
        return true;
    }

    void setPos(double f)
    {
        _pos = std::max(0.0, std::min(1.0, f));
        s_stats.lastPos = _pos;
        ::SendMessageW(_sci, SCI_KEX_SLIDEPOS, static_cast<WPARAM>(std::lround(_pos * 10000)), 0);
    }

    // speed toward the tab coming in, in whole swipes a second, over the last moments of the swipe
    double speed(double now) const
    {
        if (_history.size() < 2 || now - _history.back().t > 80) return 0;
        const Sample& a = _history.front();
        const Sample& b = _history.back();
        return b.t - a.t < 5 ? 0 : (b.p - a.p) * 1000.0 / (b.t - a.t) * _dir;
    }

    void checkRelease(double now)
    {
        // a pause only counts while Notepad++ keeps up: after a long paint the wheel messages may still be queued
        if (now - _lastTick > 50) _quietFrom = now;
        _lastTick = now;
        bool released;
        if (_touch) released = _pad.fingers(now) < 2;
        else released = now - std::max(_lastWheel, _quietFrom) > QUIET_MS;
        if (!released) return;
        if (_state == St::pending) { trace(L" let-go-early"); lock(now); return; }
        const double v = speed(now);
        const bool finish = v > FLICK || (_pos >= 0.5 && v > -FLICK);
        logGesture(finish, v);
        trace(finish ? L" finish" : L" back");
        settleTo(finish ? 1.0 : 0.0, now);
    }

    void settleTo(double target, double now)
    {
        _state = St::settling;
        _from = _pos;
        _to = target;
        _t0 = now;
        _ms = std::max(60.0, _settleMs * std::abs(_to - _from));
        ::SetTimer(hwnd(), TIMER_TICK, TICK_MS, nullptr);
    }

    void tick()
    {
        const double now = nowMs();
        if (_state == St::pending || _state == St::swiping) { checkRelease(now); return; }
        if (_state != St::settling) { ::KillTimer(hwnd(), TIMER_TICK); return; }
        const double k = std::min(1.0, (now - _t0) / _ms);
        const double e = 1.0 - (1.0 - k) * (1.0 - k) * (1.0 - k);
        setPos(_from + (_to - _from) * e);
        if (k < 1.0) return;
        ::KillTimer(hwnd(), TIMER_TICK);
        if (_to >= 1.0)
        {
            ::SendMessageW(_sci, SCI_KEX_SLIDEPOS, 10000, 1);   // in place: the view paints the new tab as usual again
            ++s_stats.commits;
        }
        else slideBack(true);
        lock(now);
    }

    // the old tab back, while the view still shows only its picture
    void slideBack(bool counted)
    {
        int idx = -1;
        for (int i = 0, n = views.nbFiles(_view); i < n; ++i)
            if (views.bufferId(_view, i) == _origBuf) { idx = i; break; }
        setPos(0);
        if (idx >= 0)
        {
            _quietSci = _sci;
            views.activate(_view, idx);
            _quietSci = nullptr;
            restoreOrder();
        }
        ::SendMessageW(_sci, SCI_KEX_SLIDEPOS, 0, 1);
        if (counted) ++s_stats.cancels;
        _dir = 0;
        _started = false;
    }

    // The tabs' Ctrl+Tab order (Notepad++'s "recent" tags): a swipe that slides back leaves it as it was.
    void rememberOrder()
    {
        _order.clear();
        for (int v = 0; v < views.count(); ++v)
            for (int i = 0, n = views.nbFiles(v); i < n; ++i)
            {
                const intptr_t b = views.bufferId(v, i);
                _order.push_back({ b, static_cast<long>(nppMsg(NPPM_KEX_RECENTTAG, static_cast<WPARAM>(b), 0)) });
            }
    }

    void restoreOrder()
    {
        long newTag = -1;
        for (const auto& o : _order) if (o.first == _newBuf) newTag = o.second;
        if (newTag < 0) return;
        // every tab that was more recent than the one swiped to becomes so again, oldest first
        std::vector<std::pair<intptr_t, long>> ahead;
        for (const auto& o : _order) if (o.second > newTag && o.first != _newBuf) ahead.push_back(o);
        std::sort(ahead.begin(), ahead.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        for (const auto& o : ahead) nppMsg(NPPM_KEX_RECENTTAG, static_cast<WPARAM>(o.first), 1);
    }

    void lock(double now)
    {
        _state = St::locked;
        _lockUntil = now + LOCK_MS;
        _lockTouches = _pad.touches();
        _dir = 0;
        _edge = false;
    }

    // stop at once (the feature goes off, a tab closes): the tab the view shows now stays
    void finishNow()
    {
        if ((_state == St::swiping || _state == St::settling) && _sci && ::IsWindow(_sci))
            ::SendMessageW(_sci, SCI_KEX_SLIDEPOS, 10000, 1);
        if (hwnd()) ::KillTimer(hwnd(), TIMER_TICK);
        if (_state != St::idle) lock(nowMs());
    }

    void logGesture(bool finish, double v)
    {
        if (s_logged >= 40) return;   // enough to tune the sensitivity by; kex.log stays small
        ++s_logged;
        log(L"Tab swipe: %ls, %.0f wheel units, let go at %.2f (%.1f swipes/s) -> %ls", _touch ? L"touchpad" : L"wheel",
            std::abs(_accH), _pos, v, finish ? L"switched" : L"slid back");
    }

    struct Sample { double t, p; };
    St _state = St::idle;
    std::vector<HWND> _hooked;
    HWND _sci = nullptr, _claimSci = nullptr, _quietSci = nullptr;
    int _view = 0, _dir = 0, _claimDir = 0;
    bool _slideOk = false, _touch = false, _follows = true, _started = false, _edge = false;
    double _acc = 0, _accH = 0, _accV = 0, _lastWheel = 0, _lockUntil = 0, _pos = 0, _lastTick = 0, _quietFrom = 0;
    double _from = 0, _to = 0, _t0 = 0, _ms = 0;
    int _gestureTouches = 0, _lockTouches = 0;
    intptr_t _origBuf = 0, _newBuf = 0;
    std::vector<std::pair<intptr_t, long>> _order;
    std::vector<Sample> _history;
    int _sensitivity = 100, _settleMs = 220;
    bool _reverse = false;
};

} // namespace

Module* createTabSwipe() { return new TabSwipe(); }

bool swipeSwitching(HWND view) { return s_self && s_self->switching(view); }

SwipeStats swipeStats()
{
    SwipeStats st = s_stats;
    st.active = s_self && s_self->busy();
    st.holding = s_self && s_self->holding();
    st.fingers = s_self ? s_self->_pad.fingers(nowMs()) : -1;
    return st;
}

void swipeTestFingers(int n) { if (s_self) s_self->_pad.fake(n); }

} // namespace kex
