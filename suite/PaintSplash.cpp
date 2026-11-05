// PaintSplash.cpp - giving text a colour splashes paint: the newly coloured tokens light up in their
// colour, which then settles into the mark, and drops of that colour fly off them and fall.
//
// It answers every way of colouring text with Notepad++'s five token styles (Style All Occurrences of
// Token, Style One Token, the KEX token menu): while one of those commands runs, the visible runs of
// the five colour indicators are compared before and after, and only the runs that are new splash.
// Tokens that get their colour while being typed (KEX live token styles) don't splash.
// Drawn by the shared effects renderer (Effects.h).
#include "Effects.h"
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace kex {

namespace {

const UINT_PTR kSubclassId = 0x4B455053;   // 'KEPS'
const int kIndicators[5] = { 25, 24, 23, 22, 21 };   // Mark Style 1..5 (SCE_UNIVERSAL_FOUND_STYLE_EXT1..5)

typedef std::tuple<int, Sci_Position, Sci_Position> Run;   // indicator, start, end

class PaintSplash;
PaintSplash* s_self = nullptr;

class PaintSplash : public Module {
public:
    const wchar_t* key() const override { return L"PaintSplash"; }
    const wchar_t* title() const override { return L"Paint splash"; }

    void loadOptions() override
    {
        _drops = std::max(0, std::min(24, settings.getInt(key(), L"DropsPerToken", 7)));
        _respectAnim = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
        fx::setSpeed(fx::K_DROP, animSpeed(key(), L"Speed"));
    }

    bool selfCheck(std::wstring& why) override
    {
        if (!views.sci(0)) { why = L"main Scintilla view not found"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!fx::acquire()) return false;
        if (!::SetWindowSubclass(npp._nppHandle, mainProc, kSubclassId, 0)) { fx::release(); return false; }
        s_self = this;
        return true;
    }

    void disable() override
    {
        ::RemoveWindowSubclass(npp._nppHandle, mainProc, kSubclassId);
        if (s_self == this)
        {
            fx::clear(fx::K_DROP);
            fx::release();
            s_self = nullptr;
        }
        _depth = 0;
    }

    // a command that may colour text starts / ends (they can nest: a menu command that runs another)
    void begin()
    {
        if (_depth++ > 0) return;
        _view = views.sci(views.current());
        _doc = _view ? sci(_view, SCI_GETDOCPOINTER) : 0;
        _before.clear();
        if (_view && ::IsWindowVisible(_view)) visibleRuns(_view, _before);
    }

    void end()
    {
        if (_depth <= 0 || --_depth > 0) return;
        HWND s = _view;
        _view = nullptr;
        if (!s || !::IsWindow(s) || !::IsWindowVisible(s) || views.sci(views.current()) != s) return;
        if (sci(s, SCI_GETDOCPOINTER) != _doc) return;   // another document came up: nothing was coloured here
        std::set<Run> after;
        visibleRuns(s, after);
        std::vector<Run> fresh;
        for (const Run& r : after)
            if (!_before.count(r)) fresh.push_back(r);
        _before.clear();
        if (fresh.empty()) return;
        ++_splashes;
        _lastTokens = static_cast<int>(fresh.size());
        if (_respectAnim && !fx::animationsOn()) return;
        splash(s, fresh);
    }

    int _splashes = 0, _lastTokens = 0;

private:
    static void visibleRuns(HWND s, std::set<Run>& out)
    {
        const SciDirect ds(s);
        const Sci_Position firstVis = ds(SCI_GETFIRSTVISIBLELINE);
        const Sci_Position firstDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis);
        const Sci_Position lastDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis + ds(SCI_LINESONSCREEN) + 1);
        const Sci_Position a = ds(SCI_POSITIONFROMLINE, firstDoc);
        const Sci_Position b = ds(SCI_GETLINEENDPOSITION, lastDoc);
        for (int ind : kIndicators)
        {
            Sci_Position pos = a;
            int guard = 0;
            while (pos < b && guard++ < 4000)
            {
                const Sci_Position end = ds(SCI_INDICATOREND, ind, pos);
                if (end <= pos) break;
                if (ds(SCI_INDICATORVALUEAT, ind, pos)) out.insert(Run{ ind, pos, std::min(end, b) });
                pos = end;
            }
        }
    }

    float rnd() { _seed = _seed * 1664525u + 1013904223u; return static_cast<float>((_seed >> 8) & 0xFFFF) / 65535.0f; }

    void splash(HWND s, const std::vector<Run>& runs)
    {
        HMONITOR mon = ::MonitorFromWindow(s, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; ::GetMonitorInfoW(mon, &mi);
        const int dpi = dpiOf(s);
        const float scale = dpi / 96.0f;
        std::vector<fx::Item> items;
        int drops = 0;
        for (const Run& r : runs)
        {
            const COLORREF c = static_cast<COLORREF>(sci(s, SCI_INDICGETFORE, std::get<0>(r)));
            std::vector<RECT> rects = rangeRects(s, std::get<1>(r), std::get<2>(r), false);
            for (const RECT& rc : rects)
            {
                if (items.size() > 600) break;
                // the wash: the token lights up in its colour and settles into the mark
                fx::Item wash;
                wash.rect = true;
                wash.rw = static_cast<float>(rc.right - rc.left) + 4 * scale;
                wash.rh = static_cast<float>(rc.bottom - rc.top) + 2 * scale;
                wash.x0 = (rc.left + rc.right) * 0.5f;
                wash.y0 = (rc.top + rc.bottom) * 0.5f;
                wash.color = c;
                wash.keys = { { 0, 0 }, { 0.03f, 0.85f }, { 0.12f, 0.6f }, { 0.42f, 0 } };
                wash.life = 0.42f;
                wash.monitor = mi.rcMonitor;
                wash.kind = fx::K_DROP;
                items.push_back(std::move(wash));
                // the drops fly off the token and fall
                const float w = static_cast<float>(rc.right - rc.left), h = static_cast<float>(rc.bottom - rc.top);
                for (int i = 0; i < _drops && drops < 400; ++i, ++drops)
                {
                    fx::Item d;
                    const float radius = (1.3f + 2.2f * rnd() * rnd()) * scale;
                    d.sprite = dot(radius, c);
                    d.x0 = rc.left + w * (0.15f + 0.7f * rnd());
                    d.y0 = rc.top + h * (0.3f + 0.4f * rnd());
                    const float ang = -3.14159265f * (0.1f + 0.8f * rnd());   // upwards, to either side
                    const float speed = (140.0f + 260.0f * rnd()) * scale;
                    d.vx = std::cos(ang) * speed;
                    d.vy = std::sin(ang) * speed;
                    d.g = 1500.0f * scale;
                    d.delay = 0.02f * rnd();
                    const float life = 0.45f + 0.35f * rnd();
                    d.keys = { { 0, 1 }, { life * 0.6f, 0.9f }, { life, 0 } };
                    d.life = life;
                    d.monitor = mi.rcMonitor;
                    d.kind = fx::K_DROP;
                    items.push_back(std::move(d));
                }
            }
        }
        fx::submit(std::move(items));
    }

    std::shared_ptr<const Sprite> dot(float radius, COLORREF c)
    {
        // a handful of sizes per colour is plenty: reuse them
        const int key = static_cast<int>(radius * 2 + 0.5f);
        auto it = _dots.find({ c, key });
        if (it != _dots.end()) return it->second;
        if (_dots.size() > 200) _dots.clear();
        auto sp = fx::dotSprite(key * 0.5f, c);
        _dots[{ c, key }] = sp;
        return sp;
    }

    static LRESULT CALLBACK mainProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
    {
        // Notepad++'s commands that colour tokens (menus, context menu, keys)
        const UINT id = LOWORD(w);
        if (m == WM_COMMAND && s_self && ((id >= IDM_SEARCH_MARKALLEXT1 && id <= IDM_SEARCH_UNMARKALLEXT5 && (id - IDM_SEARCH_MARKALLEXT1) % 2 == 0) ||
                                          (id >= IDM_SEARCH_MARKONEEXT1 && id <= IDM_SEARCH_MARKONEEXT5)))
        {
            s_self->begin();
            LRESULT r = ::DefSubclassProc(h, m, w, l);
            if (s_self) s_self->end();
            return r;
        }
        if (m == WM_NCDESTROY) ::RemoveWindowSubclass(h, mainProc, kSubclassId);
        return ::DefSubclassProc(h, m, w, l);
    }

    int _drops = 7;
    bool _respectAnim = true;
    int _depth = 0;
    HWND _view = nullptr;
    LRESULT _doc = 0;
    std::set<Run> _before;
    std::map<std::pair<COLORREF, int>, std::shared_ptr<const Sprite>> _dots;
    uint32_t _seed = 0x51ab3e7u;
};

} // namespace

Module* createPaintSplash() { return new PaintSplash(); }

void paintSplashBegin() { if (s_self) s_self->begin(); }
void paintSplashEnd() { if (s_self) s_self->end(); }

SplashStats splashStats()
{
    SplashStats st;
    if (s_self) { st.splashes = s_self->_splashes; st.lastTokens = s_self->_lastTokens; }
    const fx::Stats fs = fx::stats();
    st.live = fs.live[fx::K_DROP];
    st.spawned = fs.spawned[fx::K_DROP];
    return st;
}

} // namespace kex
