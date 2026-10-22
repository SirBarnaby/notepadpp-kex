// TabMotion.cpp - tabs that fold in and out, and a glint across a tab when its file is saved.
//
// Tab close and open: Notepad++ adds and removes tabs with TCM_INSERTITEM and TCM_DELETEITEM. Just before
// one of them the tab bar is copied, and a layered window showing that copy is put over the bar at once,
// so the bar changing underneath can't be seen. Once Notepad++ has finished (a posted message), the bar
// is copied again, and the window plays the change the way Chrome and Firefox do: a closed tab collapses
// (its picture is never squeezed: its right edge moves in over it while it fades out quickly) and the tabs
// on its right slide over to close the gap; a new tab opens out the same way, fading in. Then the window
// goes and the real bar shows. Several tabs at once (Close All, opening a session) just change.
// The document area changes at once, as in every editor; with Tab slide on, the document that takes a
// closed one's place slides in from the side its tab came from (TabSlide.cpp).
//
// Save glint: a soft diagonal band of light sweeps once across the tab of the file just saved, in a
// click-through layered window over the bar, wherever the tab is (one row or several, scroll arrows or
// not). Only when the file's tab can't be seen at all (the tab bar hidden, or scrolled away from it) does
// the band run along the top of the view instead. Save All staggers the glints along the tabs.
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
const double kGlintMs = 420, kGlintStagger = 40, kMaxHoldMs = 1500;

class TabFold;
class SaveGlint;
TabFold* s_fold = nullptr;
SaveGlint* s_glint = nullptr;
TabMotionStats s_stats;
double s_freeze = -1;   // self-test: animations stop at this fraction

double easeOutCubic(double t) { t = std::clamp(t, 0.0, 1.0); return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t); }
double easeOutQuad(double t) { t = std::clamp(t, 0.0, 1.0); return 1.0 - (1.0 - t) * (1.0 - t); }
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
// mode). One row of tabs only, without scroll arrows.
int barStrip(HWND bar)
{
    if (!bar || !::IsWindowVisible(bar)) return 0;
    HWND root = ::GetAncestor(bar, GA_ROOT);
    if (!root || ::IsIconic(root) || isCloaked(root)) return 0;
    const LONG_PTR style = ::GetWindowLongPtrW(bar, GWL_STYLE);
    if (style & (TCS_MULTILINE | TCS_VERTICAL)) return 0;
    RECT rc; ::GetClientRect(bar, &rc);
    if (rc.right < 8 || rc.bottom < 8) return 0;
    for (HWND c = ::GetWindow(bar, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT))
    {
        wchar_t cls[32] = {};
        if (::IsWindowVisible(c) && ::GetClassNameW(c, cls, 31) && ::lstrcmpiW(cls, UPDOWN_CLASSW) == 0) return 0;   // more tabs than room
    }
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

// the first `width` columns from sx of src drawn unscaled at dx with opacity k (0..256); the last column
// partly, so an edge that moves by fractions of a pixel moves smoothly
void copyFaded(Surface& dst, Surface& src, int sx, int dx, double width, double k)
{
    if (width <= 0.01 || k <= 0) return;
    const int n = std::min(static_cast<int>(std::ceil(width)), std::min(src.w - sx, dst.w - dx));
    for (int i = 0; i < n; ++i)
    {
        const uint32_t kk = static_cast<uint32_t>(std::clamp(k * std::min(1.0, width - i), 0.0, 256.0));
        if (!kk || dx + i < 0) continue;
        for (int y = 0; y < dst.h && y < src.h; ++y) over(rowOf(dst, y)[dx + i], rowOf(src, y)[sx + i], kk);
    }
}

// columns [sx, sx + sw) of src copied straight to dx
void copyColumns(Surface& dst, Surface& src, int sx, int sw, int dx, int h)
{
    sw = std::min(sw, src.w - sx);
    const int i0 = std::max(0, -dx), i1 = std::min(sw, dst.w - dx);
    if (sx < 0 || i1 <= i0) return;
    for (int y = 0; y < h; ++y)
        memcpy(rowOf(dst, y) + dx + i0, rowOf(src, y) + sx + i0, static_cast<size_t>(i1 - i0) * 4);
}

// ---------------------------------------------------------------------------------------------
// one tab bar (or, for a glint with the bar hidden, the top edge of a view): its overlay and what it plays

struct Glint { RECT r; double start; COLORREF colour; float peak; };

struct BarFx {
    HWND target = nullptr;          // the tab bar, or the view for a glint along its top
    HWND overlay = nullptr;
    int w = 0, h = 0;
    Surface before, after, frame;
    // fold
    bool holding = false;           // the overlay shows the bar as it was; Notepad++ is still changing it
    int ops = 0;                    // tab changes since the overlay went up
    bool closing = true;
    int index = -1, x0 = 0, dx = 0, backX = -1;
    double holdStart = 0, start = 0;
    bool folding = false;
    // glints
    std::vector<Glint> glints;
    bool shown = false;
};

std::map<HWND, BarFx> s_bars;

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

class TabFold : public Module {
public:
    const wchar_t* key() const override { return L"TabFold"; }
    const wchar_t* title() const override { return L"Tab close and open"; }

    void loadOptions() override
    {
        _ms = std::max(60, std::min(800, settings.getInt(key(), L"DurationMs", 180)));
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
        if (!strip) { ++s_stats.foldSkipped; return; }
        RECT rc; ::GetClientRect(bar, &rc);
        if (b.folding) { b.folding = false; hideOverlay(b); }   // the last one jumps to its end
        b.w = rc.right; b.h = strip;
        if (!grab(bar, b.before, b.w, b.h)) return;
        b.closing = closing;
        b.index = index;
        b.ops = 1;
        b.x0 = b.dx = 0;
        b.backX = -1;
        const int n = static_cast<int>(::SendMessageW(bar, TCM_GETITEMCOUNT, 0, 0));
        if (closing)
        {
            RECT it{};
            if (!::SendMessageW(bar, TCM_GETITEMRECT, index, reinterpret_cast<LPARAM>(&it))) return;
            RECT next{};
            b.x0 = it.left;
            b.dx = (index + 1 < n && ::SendMessageW(bar, TCM_GETITEMRECT, index + 1, reinterpret_cast<LPARAM>(&next)) && next.left > it.left)
                   ? next.left - it.left : it.right - it.left;
        }
        else
        {
            // the bar's background: right of the last tab, before the new one comes
            RECT last{};
            if (n > 0 && ::SendMessageW(bar, TCM_GETITEMRECT, n - 1, reinterpret_cast<LPARAM>(&last)) && last.right + 3 < b.w) b.backX = last.right + 2;
            else if (n == 0) b.backX = b.w - 2;
        }
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
            if (bulk) ++s_stats.foldBulk; else ++s_stats.foldSkipped;
            hideOverlay(b);
            return;
        }
        ::UpdateWindow(bar);
        if (!grab(bar, b.after, b.w, b.h)) { hideOverlay(b); return; }
        const int n = static_cast<int>(::SendMessageW(bar, TCM_GETITEMCOUNT, 0, 0));
        if (b.closing)
        {
            RECT last{};
            if (n > 0 && ::SendMessageW(bar, TCM_GETITEMRECT, n - 1, reinterpret_cast<LPARAM>(&last)) && last.right + 3 < b.w) b.backX = last.right + 2;
            else if (n == 0) b.backX = b.w - 2;
        }
        else
        {
            RECT it{}, next{};
            if (b.index < 0 || b.index >= n || !::SendMessageW(bar, TCM_GETITEMRECT, b.index, reinterpret_cast<LPARAM>(&it))) { hideOverlay(b); return; }
            b.x0 = it.left;
            b.dx = (b.index + 1 < n && ::SendMessageW(bar, TCM_GETITEMRECT, b.index + 1, reinterpret_cast<LPARAM>(&next)) && next.left > it.left)
                   ? next.left - it.left : it.right - it.left;
        }
        if (b.dx <= 0 || b.x0 < 0 || b.x0 >= b.w || b.backX < 0) { ++s_stats.foldSkipped; hideOverlay(b); return; }
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
        Surface& bg = b.closing ? b.after : b.before;   // where the bar's empty background can be seen
        // left of the tab: as it is now
        copyColumns(b.frame, b.after, 0, b.x0, 0, b.h);
        // the rest starts as background, one column of it repeated (keeps the bar's lines and shading)
        for (int y = 0; y < b.h; ++y)
        {
            const uint32_t c = rowOf(bg, y)[std::min(b.backX, b.w - 1)];
            uint32_t* row = rowOf(b.frame, y);
            for (int x = b.x0; x < b.w; ++x) row[x] = c;
        }
        // the room the tab takes: shrinking to nothing, or growing from it (fast first, settling softly)
        const double e = easeOutCubic(t);
        const double width = b.closing ? b.dx * (1.0 - e) : b.dx * e;
        s_stats.lastWidth = static_cast<float>(width);
        // the tab itself, upright and whole, cut off where its room ends: a closing one fades out within the
        // first half, a new one fades in once it has some room
        const double alpha = b.closing ? 1.0 - easeOutQuad(t / 0.5) : easeOutQuad((t - 0.15) / 0.6);
        copyFaded(b.frame, b.closing ? b.before : b.after, b.x0, b.x0, std::min<double>(width, b.w - b.x0), alpha * 256.0);
        // the tabs on its right, sliding (whole pixels: text never blurs)
        const int from = b.closing ? b.x0 : b.x0 + b.dx;
        copyColumns(b.frame, b.after, from, b.w - from, b.x0 + static_cast<int>(std::lround(width)), b.h);
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
            Glint g{};
            g.start = start;
            HWND target = nullptr;
            const RECT vis = barVisible(bar);
            const RECT tab = ::IsRectEmpty(&vis) ? RECT{} : tabShows(bar, idx, vis);
            if (!::IsRectEmpty(&tab))
            {
                target = bar;
                g.r = tab;
                g.colour = dark ? RGB(255, 255, 255) : mixColour(::GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), 0.35);
                g.peak = dark ? 0.30f : 0.42f;
            }
            else
            {
                // its tab can't be seen (the bar hidden, or scrolled away from it): a thin line of light along the
                // top of the view, for the document it shows
                HWND s = views.sci(v);
                if (!s || !::IsWindowVisible(s) || views.bufferId(v, views.currentIndex(v)) != buffer) continue;
                RECT cr; ::GetClientRect(s, &cr);
                target = s;
                g.r = RECT{ 0, 0, cr.right, std::min<LONG>(3, cr.bottom) };
                g.colour = mixColour(::GetSysColor(COLOR_HIGHLIGHT), RGB(255, 255, 255), dark ? 0.5 : 0.1);
                g.peak = 0.9f;
            }
            BarFx& b = s_bars[target];
            b.target = target;
            b.glints.push_back(g);
            any = true;
            ++s_stats.glints;
            if (target != bar) ++s_stats.glintLines;
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
            double t = (now - it->start) / kGlintMs;
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
    // a diagonal band, soft on both sides, across g.r at progress e
    void drawBand(BarFx& b, const Glint& g, double e)
    {
        const int l = std::max<LONG>(0, g.r.left), r = std::min<LONG>(b.w, g.r.right);
        const int t = std::max<LONG>(0, g.r.top), btm = std::min<LONG>(b.h, g.r.bottom);
        if (r <= l || btm <= t) return;
        const double h = btm - t;
        const double slant = h > 6 ? 0.6 : 0.0;   // a line along the view has no slant
        const double bandW = std::max(5.0, h > 6 ? std::min<double>(h, r - l) * 0.5 : (r - l) * 0.06);
        const double travel = (r - l) + h * slant + bandW * 6;
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
    double _lastStart = 0;
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
