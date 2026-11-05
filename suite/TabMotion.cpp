// TabMotion.cpp - tabs that fold in and out, and a glint across a tab when its file is saved.
//
// Tab close and open: Notepad++ adds and removes tabs with TCM_INSERTITEM and TCM_DELETEITEM. Just before
// one of them the tab bar is copied, and a layered window showing that copy is put over the bar at once,
// so the bar changing underneath can't be seen. Once Notepad++ has finished (a posted message), the bar
// is copied again, and the window plays the change: a closed tab sinks below the bar's lower edge, whole
// and upright (never squeezed, never faded), like a folder pushed down into its drawer, and a beat later
// the tabs on its right glide over to close the gap; a new tab's gap opens first and the tab rises into it.
// The glide is a critically damped spring (Apple's default: no bounce), the sink accelerates away as
// leaving things do. Then the window goes and the real bar shows. Several tabs at once (Close All,
// opening a session) just change. A bar with more tabs than room (scroll arrows) plays it too: the tabs
// glide from where they were to where they are now on both sides of the change (the bar may scroll by it),
// and the arrows stay as they are.
// Only the tab moves: the document area changes at once, as in every editor (no slide, no fade).
//
// Save glint: a soft diagonal band of light sweeps once across the tab of the file just saved, in a
// click-through layered window over the bar, wherever the tab is (one row or several, scroll arrows or
// not), and a thin line of light runs along the top of the view showing that file, timed as one light: the
// line's band passes under the tab just as the tab's band crosses the tab's middle. With the tab out of sight
// (the tab bar hidden, or scrolled away from it) only the line runs. Save All staggers the glints along the tabs.
#include "Overlay.h"
#include "KexScintilla.h"
#include <cmath>
#include <cstring>
#include <map>

namespace kex {

namespace {

const UINT_PTR kBarSubclassId = 0x4B45544D;   // 'KETM'
const UINT WM_TM_SETTLED = WM_APP + 61;       // wParam: the bar whose change Notepad++ has finished
const UINT_PTR TIMER_FRAME = 1;
const double kGlintStagger = 40, kMaxHoldMs = 1500;

class TabFold;
class SaveGlint;
TabFold* s_fold = nullptr;
SaveGlint* s_glint = nullptr;
TabMotionStats s_stats;
double s_freeze = -1;   // self-test: animations stop at this fraction

double easeOutCubic(double t) { t = std::clamp(t, 0.0, 1.0); return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t); }
double easeInQuad(double t) { t = std::clamp(t, 0.0, 1.0); return t * t; }
// a critically damped spring from rest, settled (to 0.5 %) at u = 1
double springOut(double u)
{
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    const double k = 7.43;
    return 1.0 - (1.0 + k * u) * std::exp(-k * u);
}
double easeInOutCubic(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

bool animationsAllowed(bool respect)
{
    BOOL anim = TRUE;
    return !respect || !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) || anim;
}

// How much of a tab bar can be seen, from its top: 0 when none. Notepad++ sizes the tab control to the whole
// pane and lays the view over its lower part, or over all of it with the tab bar hidden (and in F12 post-it
// mode). One row of tabs only (scroll arrows or not).
int barStrip(HWND bar)
{
    if (!bar || !::IsWindowVisible(bar)) return 0;
    HWND root = ::GetAncestor(bar, GA_ROOT);
    if (!root || ::IsIconic(root) || isCloaked(root)) return 0;
    const LONG_PTR style = ::GetWindowLongPtrW(bar, GWL_STYLE);
    if (style & (TCS_MULTILINE | TCS_VERTICAL)) return 0;
    RECT rc; ::GetClientRect(bar, &rc);
    if (rc.right < 8 || rc.bottom < 8) return 0;
    int strip = rc.bottom;
    RECT it{};
    if (::SendMessageW(bar, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&it)) && it.bottom > 0) strip = std::min<int>(strip, it.bottom + 3);
    POINT org{ 0, 0 };
    ::ClientToScreen(bar, &org);
    for (int v = 0; v < views.count(); ++v)
    {
        if (views.tabBar(v) != bar) continue;
        HWND s = views.sci(v);
        RECT sr{};
        if (!s || !::IsWindowVisible(s) || !::GetWindowRect(s, &sr)) continue;
        if (sr.left < org.x + rc.right && sr.right > org.x && sr.bottom > org.y) strip = std::min<int>(strip, sr.top - org.y);
    }
    return strip >= 8 ? strip : 0;
}

// Where a tab bar's scroll arrows start (shown when there are more tabs than room), or w without them.
int arrowsLeft(HWND bar, int w)
{
    for (HWND c = ::GetWindow(bar, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT))
    {
        wchar_t cls[32] = {};
        if (!::IsWindowVisible(c) || !::GetClassNameW(c, cls, 31) || ::lstrcmpiW(cls, UPDOWN_CLASSW) != 0) continue;
        RECT ur{};
        ::GetWindowRect(c, &ur);
        ::MapWindowPoints(nullptr, bar, reinterpret_cast<POINT*>(&ur), 2);
        return std::clamp<int>(ur.left, 0, w);
    }
    return w;
}

// The part of a tab bar that can be seen, any layout (several rows, scroll arrows, tabs down the side):
// the client area less what the views lie over. Empty when the bar is hidden.
RECT barVisible(HWND bar)
{
    RECT none{};
    if (!bar || !::IsWindowVisible(bar)) return none;
    HWND root = ::GetAncestor(bar, GA_ROOT);
    if (!root || ::IsIconic(root) || isCloaked(root)) return none;
    const bool vertical = (::GetWindowLongPtrW(bar, GWL_STYLE) & TCS_VERTICAL) != 0;
    RECT rc; ::GetClientRect(bar, &rc);
    POINT org{ 0, 0 };
    ::ClientToScreen(bar, &org);
    for (int v = 0; v < views.count(); ++v)
    {
        if (views.tabBar(v) != bar) continue;
        HWND s = views.sci(v);
        RECT sr{};
        if (!s || !::IsWindowVisible(s) || !::GetWindowRect(s, &sr)) continue;
        if (sr.left >= org.x + rc.right || sr.right <= org.x || sr.top >= org.y + rc.bottom || sr.bottom <= org.y) continue;
        if (vertical) rc.right = std::min<LONG>(rc.right, sr.left - org.x);
        else rc.bottom = std::min<LONG>(rc.bottom, sr.top - org.y);
    }
    return (rc.right >= 8 && rc.bottom >= 8) ? rc : none;
}

// Where tab idx of a bar can be seen (clipped to the visible bar, short of the scroll arrows); empty when it can't.
RECT tabShows(HWND bar, int idx, const RECT& vis)
{
    RECT it{}, none{};
    if (!::SendMessageW(bar, TCM_GETITEMRECT, idx, reinterpret_cast<LPARAM>(&it))) return none;
    RECT clip = vis;
    for (HWND c = ::GetWindow(bar, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT))
    {
        wchar_t cls[32] = {};
        if (!::IsWindowVisible(c) || !::GetClassNameW(c, cls, 31) || ::lstrcmpiW(cls, UPDOWN_CLASSW) != 0) continue;
        RECT ur{};
        ::GetWindowRect(c, &ur);
        ::MapWindowPoints(nullptr, bar, reinterpret_cast<POINT*>(&ur), 2);
        if (ur.top < it.bottom && ur.bottom > it.top) clip.right = std::min(clip.right, ur.left);
    }
    RECT r{};
    if (!::IntersectRect(&r, &it, &clip) || r.right - r.left < 4 || r.bottom - r.top < 4) return none;
    return r;
}

// copy what a window shows (its own pixels, whatever covers it on screen) into s, opaque
bool grab(HWND w, Surface& s, int width, int height)
{
    if (!s.ensure(width, height)) return false;
    HDC dc = ::GetDC(w);
    if (!dc) return false;
    ::BitBlt(s.dc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    ::ReleaseDC(w, dc);
    ::GdiFlush();
    for (int y = 0; y < height; ++y)
    {
        uint32_t* row = s.px + static_cast<size_t>(y) * s.stride;
        for (int x = 0; x < width; ++x) row[x] |= 0xFF000000u;
    }
    return true;
}

inline uint32_t* rowOf(Surface& s, int y) { return s.px + static_cast<size_t>(y) * s.stride; }

// premultiplied over with weight k (0..256)
inline void over(uint32_t& d, uint32_t s, uint32_t k)
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
    d = s + (rb | (ag << 8));
}

// ---------------------------------------------------------------------------------------------
// one tab bar (or, for a glint with the bar hidden, the top edge of a view): its overlay and what it plays

struct Glint { RECT r; double start; COLORREF colour; float peak; };

struct BarFx {
    HWND target = nullptr;          // the tab bar, or the view for a glint along its top
    HWND overlay = nullptr;
    int w = 0, h = 0;
    Surface before, after, frame;
    // fold. Tabs show left of rightB in the bar as it was (before) and of rightA as it is now (after): the scroll
    // arrows start there.
    bool holding = false;           // the overlay shows the bar as it was; Notepad++ is still changing it
    int ops = 0;                    // tab changes since the overlay went up
    bool closing = true;
    int index = -1, dx = 0;         // the tab, and the room it takes (to the next tab's start)
    int x0b = 0, x0a = 0;           // where the tabs on its left end: before, and now
    int rightOfB = 0, rightOfA = 0; // where the tabs on its right start: before, and now
    int rightB = 0, rightA = 0;
    int tabW = 0, tabTop = 0;       // the tab itself: its width, and where it starts from the bar's top
    bool head = false, tail = false;// tabs on its left / right, which glide (without any, there is only background)
    std::vector<uint32_t> back;     // one column of the bar's background, top to bottom
    double holdStart = 0, start = 0;
    bool folding = false;
    // glints
    std::vector<Glint> glints;
    bool shown = false;
};

std::map<HWND, BarFx> s_bars;

// the background column a bar showed last (a bar full of tabs, with scroll arrows, shows none)
struct BackSeen { int h = 0; bool dark = false; std::vector<uint32_t> px; };
std::map<HWND, BackSeen> s_backSeen;

// A column of the bar's own background: halfway between the last tab and where the tabs end (the bar's end or
// its scroll arrows), clear of the selected tab's raised edge (drawn wider than its rectangle) and of the
// control's border. -1: no room.
int backgroundColumn(HWND bar, int right)
{
    const int n = static_cast<int>(::SendMessageW(bar, TCM_GETITEMCOUNT, 0, 0));
    if (n == 0) return right / 2;
    RECT last{};
    if (!::SendMessageW(bar, TCM_GETITEMRECT, n - 1, reinterpret_cast<LPARAM>(&last)) || last.right + 12 >= right) return -1;
    return (last.right + right) / 2;
}

// One column of b's bar background, top to bottom (keeps the bar's lines and shading), from a picture of the
// bar when it has room after its last tab. false when it has none (the tabs reach its end or its scroll arrows).
bool backFromPicture(BarFx& b, HWND bar, Surface& pic, int right)
{
    const int x = backgroundColumn(bar, right);
    if (x < 0 || x >= pic.w || pic.h < b.h) return false;
    b.back.resize(static_cast<size_t>(b.h));
    for (int y = 0; y < b.h; ++y) b.back[y] = rowOf(pic, y)[x] | 0xFF000000u;
    BackSeen& seen = s_backSeen[bar];
    seen.h = b.h; seen.dark = nppMsg(NPPM_ISDARKMODEENABLED, 0, 0) != 0; seen.px = b.back;
    return true;
}

// The bar's background for a fold: from the bar as it is now, else as it was (taken before the change), else the
// column it showed last, else the bar paints its background for it (Notepad++'s dark mode fills it with its own
// colour; a light bar that paints none there shows the face colour). Never a capture of a control asked to print
// itself: Notepad++'s dark bar prints no background then (white).
void takeBackground(BarFx& b, HWND bar, Surface& pic, int right)
{
    if (backFromPicture(b, bar, pic, right) || static_cast<int>(b.back.size()) == b.h) return;
    const bool dark = nppMsg(NPPM_ISDARKMODEENABLED, 0, 0) != 0;
    const BackSeen& seen = s_backSeen[bar];
    if (seen.h == b.h && seen.dark == dark && static_cast<int>(seen.px.size()) == b.h) { b.back = seen.px; return; }
    const COLORREF face = ::GetSysColor(COLOR_BTNFACE);
    const uint32_t facepx = 0xFF000000u | (GetRValue(face) << 16) | (GetGValue(face) << 8) | GetBValue(face);
    b.back.assign(static_cast<size_t>(b.h), facepx);
    Surface s;
    if (!s.ensure(4, b.h)) return;
    const uint32_t mark = 0x00FE01FDu;   // a colour no bar has: still there = nothing was painted
    for (int y = 0; y < b.h; ++y) for (int i = 0; i < 4; ++i) rowOf(s, y)[i] = mark;
    ::GdiFlush();
    ::SendMessageW(bar, WM_ERASEBKGND, reinterpret_cast<WPARAM>(s.dc), 0);
    ::GdiFlush();
    for (int y = 0; y < b.h; ++y)
    {
        const uint32_t c = rowOf(s, y)[2] & 0xFFFFFFu;
        if (c != mark) b.back[y] = c | 0xFF000000u;
    }
}

inline void fillColumns(Surface& dst, const std::vector<uint32_t>& col, int x0, int x1)
{
    x0 = std::max(0, x0); x1 = std::min(x1, dst.w);
    for (int y = 0; y < dst.h && y < static_cast<int>(col.size()); ++y)
    {
        uint32_t* row = rowOf(dst, y);
        for (int x = x0; x < x1; ++x) row[x] = col[y];
    }
}

// columns [s0, s1) of src drawn with their left at dx + s0, kept inside [0, clip)
void blitColumns(Surface& dst, Surface& src, int s0, int s1, int dx, int clip, int h, int y0 = 0, int dy = 0)
{
    s0 = std::max(s0, 0); s1 = std::min(s1, src.w);
    int d0 = s0 + dx, d1 = s1 + dx;
    if (d0 < 0) { s0 -= d0; d0 = 0; }
    d1 = std::min(d1, std::min(clip, dst.w));
    if (d1 <= d0) return;
    for (int y = std::max(0, y0 + dy); y < h && y < dst.h; ++y)
    {
        const int sy = y - dy;
        if (sy < 0 || sy >= src.h) continue;
        memcpy(rowOf(dst, y) + d0, rowOf(src, sy) + s0, static_cast<size_t>(d1 - d0) * 4);
    }
}

void hideOverlay(BarFx& b)
{
    if (b.overlay && b.shown) ::ShowWindow(b.overlay, SW_HIDE);
    b.shown = false;
}

bool ensureOverlay(BarFx& b)
{
    if (b.overlay && ::IsWindow(b.overlay)) return true;
    HWND root = ::GetAncestor(b.target, GA_ROOT);
    b.overlay = createOverlayWindow(root, false, L"KEX tab motion");
    b.shown = false;
    return b.overlay != nullptr;
}

POINT screenOrigin(HWND w) { POINT p{ 0, 0 }; ::ClientToScreen(w, &p); return p; }

void present(BarFx& b, Surface& s)
{
    if (!ensureOverlay(b)) return;
    POINT pos = screenOrigin(b.target);
    SIZE sz{ b.w, b.h };
    pushLayered(b.overlay, s, pos, sz);
    if (!b.shown)
    {
        ::SetWindowPos(b.overlay, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        b.shown = true;
    }
}

// ---------------------------------------------------------------------------------------------
// the shared frame clock (one timer for every bar)

class Motion : public MsgWindow {
public:
    bool start()
    {
        if (++_users > 1) return true;
        return createMsgWindow(L"KexTabMotion");
    }
    void stop()
    {
        if (_users > 0 && --_users > 0) return;
        if (hwnd()) ::KillTimer(hwnd(), TIMER_FRAME);
        destroyMsgWindow();
        _ticking = false;
        for (auto& e : s_bars) if (e.second.overlay && ::IsWindow(e.second.overlay)) ::DestroyWindow(e.second.overlay);
        s_bars.clear();
        s_stats.overlayShown = false;
    }
    void tick()
    {
        if (!_ticking && hwnd()) { _ticking = true; ::SetTimer(hwnd(), TIMER_FRAME, 8, nullptr); }
        frame();
    }
    void post(HWND bar) { if (hwnd()) ::PostMessageW(hwnd(), WM_TM_SETTLED, reinterpret_cast<WPARAM>(bar), 0); }
    void frame();
    void settled(HWND bar);

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        try
        {
            if (m == WM_TIMER && w == TIMER_FRAME) { frame(); return 0; }
            if (m == WM_TM_SETTLED) { settled(reinterpret_cast<HWND>(w)); return 0; }
        }
        catch (...) { log(L"tab motion: exception (message 0x%x)", m); }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    int _users = 0;
    bool _ticking = false;
};

Motion s_motion;

// ---------------------------------------------------------------------------------------------
// Tab close and open

// a tab change that just happens, with why in kex.log (the first few: enough to see what keeps one from playing)
void foldSkipped(const wchar_t* why, int a = 0, int b = 0)
{
    static int logged = 0;
    ++s_stats.foldSkipped;
    if (logged < 20) { ++logged; log(L"tab close/open not played: %ls (%d, %d)", why, a, b); }
}

bool itemRect(HWND bar, int i, RECT& r)
{
    return i >= 0 && ::SendMessageW(bar, TCM_GETITEMRECT, i, reinterpret_cast<LPARAM>(&r)) != 0;
}

// Tabs that glide by shift as p goes from 0 to 1: drawn as they were (columns [b0, b1) of the bar before) and,
// over that, as they are (columns [a0, a1) now), each where the glide has them. Where the bar scrolls by the
// change, tabs leave on one side as the ones the bar shows now come in on the other. A scroll further than the
// bar is wide glides one bar's width: the tabs between are never seen.
void glide(BarFx& b, int b0, int b1, int a0, int a1, int shift, double p, int clip)
{
    const int span = std::max(1, std::min(b.rightA, b.rightB));
    shift = std::clamp(shift, -span, span);
    const int offB = static_cast<int>(std::lround(shift * p)), offA = offB - shift;
    blitColumns(b.frame, b.before, b0, std::min(b1, b.rightB), offB, clip, b.h);
    blitColumns(b.frame, b.after, a0, std::min(a1, b.rightA), offA, clip, b.h);
}

class TabFold : public Module {
public:
    const wchar_t* key() const override { return L"TabFold"; }
    const wchar_t* title() const override { return L"Tab close and open"; }

    void loadOptions() override
    {
        _ms = std::min(3840, animMs(240, animSpeed(key(), L"Speed", 240, L"DurationMs")));
        _respect = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
    }

    bool selfCheck(std::wstring& why) override
    {
        if (!views.tabBar(0)) { why = L"the main tab bar was not found"; return false; }
        wchar_t cls[32] = {};
        ::GetClassNameW(views.tabBar(0), cls, 31);
        if (::lstrcmpW(cls, WC_TABCONTROLW) != 0) { why = L"the tab bar is not a Windows tab control"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!s_motion.start()) return false;
        for (int v = 0; v < views.count(); ++v)
            if (HWND bar = views.tabBar(v))
                if (::SetWindowSubclass(bar, barProc, kBarSubclassId, 0)) _bars.push_back(bar);
        s_fold = this;
        return true;
    }

    void disable() override
    {
        for (HWND bar : _bars) if (::IsWindow(bar)) ::RemoveWindowSubclass(bar, barProc, kBarSubclassId);
        _bars.clear();
        for (auto& e : s_bars) { e.second.holding = e.second.folding = false; }
        s_fold = nullptr;
        s_motion.frame();   // what is left (glints) goes on, the folds are dropped
        s_motion.stop();
    }

    // a tab is about to go (closing) or come (index of the new one)
    void beforeChange(HWND bar, int index, bool closing)
    {
        BarFx& b = s_bars[bar];
        b.target = bar;
        if (b.holding) { ++b.ops; return; }   // a second change in the same go: several tabs at once
        const int strip = animationsAllowed(_respect) ? barStrip(bar) : 0;
        if (!strip) { foldSkipped(animationsAllowed(_respect) ? L"the tab bar can't be seen (or has several rows)" : L"Windows animation effects are off"); return; }
        RECT rc; ::GetClientRect(bar, &rc);
        if (b.folding) { b.folding = false; hideOverlay(b); }   // the last one jumps to its end
        b.w = rc.right; b.h = strip;
        b.rightB = arrowsLeft(bar, b.w);
        b.closing = closing;
        b.index = index;
        b.dx = b.tabW = b.tabTop = 0;
        b.head = index > 0;
        const int n = static_cast<int>(::SendMessageW(bar, TCM_GETITEMCOUNT, 0, 0));
        RECT it{};
        if (closing)
        {
            // a tab out of sight (scrolled away behind the arrows) just goes
            if (!itemRect(bar, index, it) || it.right <= 0 || it.left >= b.rightB) { foldSkipped(L"the closed tab is scrolled out of sight", it.left, b.rightB); return; }
            RECT next{};
            b.x0b = it.left;
            b.dx = (itemRect(bar, index + 1, next) && next.left > it.left) ? next.left - it.left : it.right - it.left;
            b.tabW = it.right - it.left;
            b.tabTop = std::max(0, static_cast<int>(it.top) - 2);
            b.rightOfB = b.x0b + b.dx;
        }
        else
        {
            if (index < n && itemRect(bar, index, it)) b.x0b = it.left;
            else b.x0b = (n > 0 && itemRect(bar, n - 1, it)) ? it.right : 0;
            b.rightOfB = b.x0b;
        }
        if (!grab(bar, b.before, b.w, b.h)) return;
        b.back.clear();
        backFromPicture(b, bar, b.before, b.rightB);   // the bar may have no room left for it once the change is made
        b.ops = 1;
        // the bar as it was, over the bar, at once: the change underneath can't be seen
        present(b, b.before);
        b.holding = true;
        b.holdStart = nowMs();
        s_motion.post(bar);
    }

    void settled(HWND bar)
    {
        auto f = s_bars.find(bar);
        if (f == s_bars.end() || !f->second.holding) return;
        BarFx& b = f->second;
        b.holding = false;
        RECT rc; ::GetClientRect(bar, &rc);
        const bool bulk = b.ops > 1;
        if (bulk || barStrip(bar) != b.h || rc.right != b.w)
        {
            if (bulk) ++s_stats.foldBulk; else foldSkipped(L"the tab bar changed size", rc.right - b.w, barStrip(bar) - b.h);
            hideOverlay(b);
            return;
        }
        ::UpdateWindow(bar);
        if (!grab(bar, b.after, b.w, b.h)) { hideOverlay(b); return; }
        const int n = static_cast<int>(::SendMessageW(bar, TCM_GETITEMCOUNT, 0, 0));
        b.rightA = arrowsLeft(bar, b.w);
        // Where the tabs on the left end now (they may have moved: the bar may have scrolled by the change). Measured at
        // the change itself, never at a tab left of it: a tab scrolled away to the left has no place (TCM_GETITEMRECT
        // fails for it).
        bool ok = true;
        RECT it{};
        if (b.closing)
        {
            b.tail = b.index < n;
            ok = b.tail ? itemRect(bar, b.index, it) : itemRect(bar, b.index - 1, it);
            b.x0a = b.tail ? it.left : it.right;
            b.rightOfA = b.x0a;
        }
        else
        {
            RECT next{};
            ok = itemRect(bar, b.index, it);
            b.x0a = it.left;
            b.dx = (itemRect(bar, b.index + 1, next) && next.left > it.left) ? next.left - it.left : it.right - it.left;
            b.tabW = it.right - it.left;
            b.tabTop = std::max(0, static_cast<int>(it.top) - 2);
            b.tail = b.index + 1 < n;
            b.rightOfA = b.x0a + b.dx;
            ok = ok && b.x0a + b.tabW > 0 && b.x0a < b.rightA;   // a new tab out of sight: the bar just changes
        }
        if (!ok || b.dx <= 0 || b.tabW <= 0 || b.rightA <= 0 || b.rightB <= 0)
        {
            foldSkipped(b.closing ? L"the tabs can't be found after the close" : L"the new tab is out of sight", b.x0a, b.rightA);
            hideOverlay(b);
            return;
        }
        takeBackground(b, bar, b.after, b.rightA);
        b.folding = true;
        b.start = nowMs();
        if (b.closing) ++s_stats.folds; else ++s_stats.unfolds;
        s_stats.lastDx = b.dx;
        s_motion.tick();
    }

    // one frame of a fold into b.frame; false when it has ended
    bool drawFold(BarFx& b, double now)
    {
        double t = (now - b.start) / _ms;
        if (s_freeze >= 0) t = std::min(t, s_freeze);
        if (t >= 1.0) { b.folding = false; return false; }
        s_stats.lastFraction = static_cast<float>(t);
        if (!b.frame.ensure(b.w, b.h)) return false;
        const int clip = b.rightA;   // the tabs show left of the scroll arrows, which stay as they are now
        fillColumns(b.frame, b.back, 0, clip);
        blitColumns(b.frame, b.after, clip, b.w, 0, b.w, b.h);
        // the room the tab takes: closing, it starts to close once the tab is on its way down; opening, it
        // opens first and the tab rises into it. Both sides of it glide on the same spring.
        const double p = b.closing ? springOut((t - 0.2) / 0.8) : springOut(t / 0.7);
        s_stats.lastWidth = static_cast<float>(b.dx * (b.closing ? 1.0 - p : p));
        const int span = std::max(1, std::min(b.rightA, b.rightB));
        const int shiftL = std::clamp(b.x0a - b.x0b, -span, span);
        const int offL = static_cast<int>(std::lround(shiftL * p));
        // the tab, whole and upright, moved down: sinking below the bar's edge (accelerating away), or rising
        // from below it (settling softly), and carried along with the tabs on its left
        const double down = b.closing ? easeInQuad(t / 0.55) : 1.0 - easeOutCubic((t - 0.15) / 0.85);
        const int travel = b.h - b.tabTop + 1;
        const int drop = static_cast<int>(std::lround(down * travel));
        s_stats.lastDrop = drop;
        if (drop < travel)
        {
            // a new tab is cut off where its room ends; a closing one goes under its neighbours as they come
            if (b.closing) blitColumns(b.frame, b.before, b.x0b, b.x0b + b.tabW, offL, clip, b.h, b.tabTop, drop);
            else blitColumns(b.frame, b.after, b.x0a, b.x0a + std::min(b.tabW, static_cast<int>(std::lround(b.dx * p))), offL - shiftL, clip, b.h, b.tabTop, drop);
        }
        // the tabs on either side, gliding (whole pixels: text never blurs); with none on the right, the background
        // is already in place (copying it would slide the selected tab's edge along)
        if (b.head) glide(b, 0, b.x0b, 0, b.x0a, b.x0a - b.x0b, p, clip);
        else blitColumns(b.frame, b.after, 0, b.x0a, 0, clip, b.h);   // the bar's margin
        if (b.tail) glide(b, b.rightOfB, b.rightB, b.rightOfA, b.rightA, b.rightOfA - b.rightOfB, p, clip);
        ++s_stats.foldFrames;
        return true;
    }

private:
    static LRESULT CALLBACK barProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
    {
        try
        {
            if (s_fold && (m == TCM_DELETEITEM || m == TCM_INSERTITEMW || m == TCM_INSERTITEMA))
            {
                const int n = static_cast<int>(::DefSubclassProc(h, TCM_GETITEMCOUNT, 0, 0));
                const int idx = static_cast<int>(w);
                if (m == TCM_DELETEITEM ? (idx >= 0 && idx < n) : idx >= 0)
                    s_fold->beforeChange(h, m == TCM_DELETEITEM ? idx : std::min(idx, n), m == TCM_DELETEITEM);
            }
            else if (m == TCM_DELETEALLITEMS && s_fold)
            {
                auto f = s_bars.find(h);
                if (f != s_bars.end() && f->second.holding) ++f->second.ops;
            }
            else if (m == WM_NCDESTROY) ::RemoveWindowSubclass(h, barProc, kBarSubclassId);
        }
        catch (...) { log(L"tab motion: exception in the tab bar hook"); }
        return ::DefSubclassProc(h, m, w, l);
    }

    std::vector<HWND> _bars;
    int _ms = 180;
    bool _respect = true;
};

// ---------------------------------------------------------------------------------------------
// Save glint

class SaveGlint : public Module {
public:
    const wchar_t* key() const override { return L"SaveGlint"; }
    const wchar_t* title() const override { return L"Save glint"; }

    void loadOptions() override
    {
        _respect = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
        _ms = std::min(6720, animMs(420, animSpeed(key(), L"Speed", 420, L"DurationMs")));
        s_stats.glintMs = static_cast<int>(_ms);
    }

    bool selfCheck(std::wstring& why) override
    {
        if (!views.tabBar(0)) { why = L"the main tab bar was not found"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!s_motion.start()) return false;
        s_glint = this;
        return true;
    }

    void disable() override
    {
        for (auto& e : s_bars) e.second.glints.clear();
        s_glint = nullptr;
        s_motion.frame();
        s_motion.stop();
    }

    void onNotify(SCNotification* n) override
    {
        if (n->nmhdr.code != NPPN_FILESAVED) return;
        saved(static_cast<intptr_t>(n->nmhdr.idFrom));
    }

    void saved(intptr_t buffer)
    {
        ++s_stats.saves;
        if (!animationsAllowed(_respect)) { ++s_stats.glintSkipped; return; }
        const bool dark = nppMsg(NPPM_ISDARKMODEENABLED, 0, 0) != 0;
        const double now = nowMs();
        double start = std::max(now, _lastStart + kGlintStagger);
        if (start - now > 2000) start = now;   // a huge Save All: the rest shine together
        bool any = false;
        for (int v = 0; v < views.count(); ++v)
        {
            if (!views.visible(v)) continue;
            int idx = -1;
            for (int i = 0, n = views.nbFiles(v); i < n; ++i) if (views.bufferId(v, i) == buffer) { idx = i; break; }
            if (idx < 0) continue;
            HWND bar = views.tabBar(v);
            auto add = [&](HWND target, const Glint& g) {
                BarFx& b = s_bars[target];
                b.target = target;
                b.glints.push_back(g);
                any = true;
                ++s_stats.glints;
                if (target != bar) ++s_stats.glintLines;
            };
            // its tab, wherever it can be seen (one row or several, scroll arrows or not)
            const RECT vis = barVisible(bar);
            const RECT tab = ::IsRectEmpty(&vis) ? RECT{} : tabShows(bar, idx, vis);
            Glint tabGlint{}, lineGlint{};
            const bool hasTab = !::IsRectEmpty(&tab);
            if (hasTab)
            {
                tabGlint.start = start;
                tabGlint.r = tab;
                tabGlint.colour = dark ? RGB(255, 255, 255) : mixColour(::GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 0.35);
                tabGlint.peak = dark ? 0.30f : 0.42f;
            }
            // and a thin line of light along the top of the view, when the view shows that document: the two read
            // as one answer, whichever the eye is on
            HWND s = views.sci(v);
            const bool hasLine = s && ::IsWindowVisible(s) && views.bufferId(v, views.currentIndex(v)) == buffer;
            if (hasLine)
            {
                RECT cr; ::GetClientRect(s, &cr);
                lineGlint.start = start;
                lineGlint.r = RECT{ 0, 0, cr.right, std::min<LONG>(3, cr.bottom) };
                lineGlint.colour = mixColour(::GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), dark ? 0.5 : 0.1);
                lineGlint.peak = 0.9f;
            }
            if (hasTab && hasLine)
            {
                // one light: the line's band passes under the tab at the moment the tab's band crosses its middle
                POINT mid{ (tab.left + tab.right) / 2, (tab.top + tab.bottom) / 2 };
                ::ClientToScreen(bar, &mid);
                ::ScreenToClient(s, &mid);
                double band = 0;
                const double travel = bandTravel(lineGlint.r, &band);
                const double along = std::clamp((mid.x - lineGlint.r.left + band * 3) / travel, 0.02, 0.98);
                const double lineU = invEaseInOutCubic(along);   // when (0..1 of its time) the line's band is there
                if (lineU >= 0.5) tabGlint.start = start + (lineU - 0.5) * _ms;
                else lineGlint.start = start + (0.5 - lineU) * _ms;
                const double uLine = (tabGlint.start + 0.5 * _ms - lineGlint.start) / _ms;
                s_stats.tabMiddle = static_cast<float>(mid.x);
                s_stats.lineAtCross = static_cast<float>(lineGlint.r.left - band * 3 + travel * easeInOutCubic(uLine));
            }
            if (hasTab) { add(bar, tabGlint); s_stats.tabGlintDelay = tabGlint.start - now; }
            if (hasLine) { add(s, lineGlint); s_stats.lineGlintDelay = lineGlint.start - now; }
        }
        if (any) { _lastStart = start; s_motion.tick(); }
    }

    // the glints of a bar over b.frame (which holds the fold, or nothing); false when none is left
    bool drawGlints(BarFx& b, double now, bool overFold)
    {
        bool live = false;
        if (!overFold && b.frame.ensure(b.w, b.h)) b.frame.clear();
        for (auto it = b.glints.begin(); it != b.glints.end();)
        {
            double t = (now - it->start) / _ms;
            if (s_freeze >= 0) t = std::min(t, s_freeze);
            if (t >= 1.0) { it = b.glints.erase(it); continue; }
            live = true;
            if (t >= 0) drawBand(b, *it, easeInOutCubic(t));
            ++it;
        }
        if (live) ++s_stats.glintFrames;
        return live;
    }

private:
    // how far a band goes across r (its width, the slant and the soft sides), and the band's width
    static double bandTravel(const RECT& r, double* bandW)
    {
        const double w = r.right - r.left, h = r.bottom - r.top;
        const double slant = h > 6 ? 0.6 : 0.0;   // a line along the view has no slant
        *bandW = std::max(5.0, h > 6 ? std::min<double>(h, w) * 0.5 : w * 0.06);
        return w + h * slant + *bandW * 6;
    }

    // the time (0..1) at which easeInOutCubic reaches e
    static double invEaseInOutCubic(double e)
    {
        double lo = 0, hi = 1;
        for (int i = 0; i < 40; ++i) { const double m = (lo + hi) / 2; (easeInOutCubic(m) < e ? lo : hi) = m; }
        return (lo + hi) / 2;
    }

    // a diagonal band, soft on both sides, across g.r at progress e
    void drawBand(BarFx& b, const Glint& g, double e)
    {
        const int l = std::max<LONG>(0, g.r.left), r = std::min<LONG>(b.w, g.r.right);
        const int t = std::max<LONG>(0, g.r.top), btm = std::min<LONG>(b.h, g.r.bottom);
        if (r <= l || btm <= t) return;
        const double h = btm - t;
        const double slant = h > 6 ? 0.6 : 0.0;
        double bandW = 0;
        const double travel = bandTravel(RECT{ l, t, r, btm }, &bandW);
        const double pos = -bandW * 3 + travel * e;
        const uint32_t cr = GetRValue(g.colour), cg = GetGValue(g.colour), cb = GetBValue(g.colour);
        for (int y = t; y < btm; ++y)
        {
            uint32_t* row = rowOf(b.frame, y);
            for (int x = l; x < r; ++x)
            {
                // the band leans like "/": lower rows meet it further left
                const double u = (x - l) + (y - t) * slant - pos;
                if (u < -bandW * 3 || u > bandW * 3) continue;
                const double a = g.peak * std::exp(-(u / bandW) * (u / bandW));
                const uint32_t ai = static_cast<uint32_t>(a * 255.0 + 0.5);
                if (!ai) continue;
                const uint32_t px = (ai << 24) | ((cr * ai / 255) << 16) | ((cg * ai / 255) << 8) | (cb * ai / 255);
                over(row[x], px, 256);
            }
        }
    }

    bool _respect = true;
    double _lastStart = 0, _ms = 420;
};

void Motion::settled(HWND bar)
{
    if (s_fold) s_fold->settled(bar);
    else { auto f = s_bars.find(bar); if (f != s_bars.end()) { f->second.holding = false; hideOverlay(f->second); } }
    frame();
}

void Motion::frame()
{
    const double now = nowMs();
    bool any = false;
    for (auto it = s_bars.begin(); it != s_bars.end();)
    {
        BarFx& b = it->second;
        if (!::IsWindow(b.target))
        {
            if (b.overlay) ::DestroyWindow(b.overlay);
            it = s_bars.erase(it);
            continue;
        }
        if (b.holding)
        {
            // Notepad++ never got back to its message loop in time: show the bar as it is
            if (now - b.holdStart > kMaxHoldMs) { b.holding = false; hideOverlay(b); }
            else { any = true; ++it; continue; }
        }
        if (b.folding && !s_fold) b.folding = false;
        if (!b.glints.empty() && !s_glint) b.glints.clear();
        if (!b.folding && b.glints.empty()) { hideOverlay(b); ++it; continue; }
        RECT rc; ::GetClientRect(b.target, &rc);
        // a view's glint is only its top edge; a bar's, the part of the bar that shows
        if (!b.folding)
        {
            if (views.viewOf(b.target) >= 0) { b.w = rc.right; b.h = std::min<LONG>(3, rc.bottom); }
            else { const RECT vis = barVisible(b.target); b.w = vis.right; b.h = vis.bottom; }
            if (b.h <= 0 || b.w <= 0) { b.glints.clear(); hideOverlay(b); ++it; continue; }
        }
        bool folded = b.folding && s_fold && s_fold->drawFold(b, now);
        bool glinted = !b.glints.empty() && s_glint && s_glint->drawGlints(b, now, folded);
        if (folded || glinted) { present(b, b.frame); any = true; }
        else hideOverlay(b);
        ++it;
    }
    s_stats.overlayShown = false;
    for (auto& e : s_bars) if (e.second.shown) s_stats.overlayShown = true;
    if (!any && _ticking && hwnd()) { ::KillTimer(hwnd(), TIMER_FRAME); _ticking = false; }
}

} // namespace

Module* createTabFold() { return new TabFold(); }
Module* createSaveGlint() { return new SaveGlint(); }

TabMotionStats tabMotionStats() { return s_stats; }
void tabMotionTestFreeze(double fraction) { s_freeze = fraction; }

// self-test: what the overlay over a bar (or view) shows now, over the window itself, as a .bmp
bool tabMotionTestSave(HWND target, const std::wstring& file)
{
    auto f = s_bars.find(target);
    if (f == s_bars.end() || !f->second.shown || f->second.frame.w <= 0) return false;
    BarFx& b = f->second;
    Surface pic;
    const int w = std::min(b.w, b.frame.w), h = std::min(b.h, b.frame.h);
    if (!grab(target, pic, w, h)) return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) over(rowOf(pic, y)[x], rowOf(b.frame, y)[x], 256);
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w) * h * 4;
    HANDLE out = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = ::WriteFile(out, &fh, sizeof(fh), &n, nullptr) && ::WriteFile(out, &ih, sizeof(ih), &n, nullptr);
    for (int y = 0; y < h && ok; ++y) ok = ::WriteFile(out, rowOf(pic, y), static_cast<DWORD>(w) * 4, &n, nullptr) != 0;
    ::CloseHandle(out);
    return ok;
}

} // namespace kex
