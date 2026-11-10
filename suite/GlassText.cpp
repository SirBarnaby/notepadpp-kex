// GlassText.cpp - "Glass text area": Windows' blur shows through the background of the text views,
// live (it is whatever is really behind Notepad++, so it changes as the window moves). The text, the
// menus, toolbar, tabs and status bar stay solid.
//
// How it works:
// - The window hosting the views (the main window, and any torn-out view window) becomes a colour-keyed
//   layered window. The views' background colour is swapped for that key, which is one step off the
//   theme's own background, so anything that is not keyed still looks the same.
// - Colour-keyed pixels show what is behind the window. Directly behind each host window sits a
//   "catcher" window the size of its client area that carries Windows' acrylic blur (the accent policy
//   behind the Windows 10 taskbar and Start), so the keyed pixels show a blurred, tinted backdrop.
// - Colour-keyed pixels also let the mouse through, which is what the catcher is for: clicks and the
//   wheel there go back to the view under them. kexsuite::windowFromPoint sees through it for the core.
// - Dragging something (files, text) over the glass turns it solid until the drag ends, so the views'
//   own drop handling works.
// - The text: the views draw onto a see-through layer of their own (SCI_KEX_GLASS, DirectComposition, in
//   the KEX Scintilla) that sits on top of the keyed window. The key colour is see-through there and text
//   edges are smoothed in grey against nothing, so letters keep clean edges over the blur. The views'
//   own windows show only the key, so all of the text area is glass.
// - Where the layer can't be made (no DirectComposition, as under Wine), a view draws as before:
//   Direct2D through a DC (a window render target cannot draw into a layered window), with text
//   unsmoothed, since smoothed edges blend into the key colour and would show as an outline.
// - Acrylic is slow to move on Windows 10 (every move of an acrylic window waits for the compositor).
//   With "Smooth moving", a second catcher with the plain blur sits under the acrylic one; while the
//   window is dragged or sized, only the plain one moves, and the acrylic one comes back where the
//   window ends up.
#include "KexSuite.h"
#include "KexScintilla.h"
#include <ole2.h>
#include <map>
#include <cwchar>

namespace kex {

namespace {

const wchar_t* const kCatcherClass = L"KexGlassCatcher";
const UINT_PTR kHostSubclassId = 0x4B454747;   // 'KEGG'
const UINT_PTR TIMER_SYNC = 1, TIMER_DRAG = 2, TIMER_BLURHIDE = 3;
const UINT kBlurHideMs = 200;   // the plain blur stays under the acrylic this long after a move, while the acrylic draws
const UINT WM_GLASS_REFRESH = WM_APP + 91;

enum Engine { E_BEHIND = 0, E_SELF, E_KEYONLY };
enum Edges { EDGES_SMOOTH = 0, EDGES_SHARP };
enum ViewMode { VIEW_NEW = 0, VIEW_LAYER, VIEW_FALLBACK };
enum { ACCENT_DISABLED = 0, ACCENT_BLURBEHIND = 3, ACCENT_ACRYLIC = 4 };

struct AccentPolicy { int state; int flags; DWORD colour; int animationId; };
struct CompositionData { int attribute; void* data; SIZE_T size; };
typedef BOOL(WINAPI* SetCompositionFn)(HWND, CompositionData*);

SetCompositionFn compositionFn()
{
    static SetCompositionFn fn = reinterpret_cast<SetCompositionFn>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    return fn;
}

DWORD windowsBuild()
{
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    static DWORD build = 0;
    if (!build)
    {
        RtlGetVersionFn fn = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof(v);
        if (fn && fn(&v) == 0) build = v.dwBuildNumber;
    }
    return build;
}

// Windows Settings > Personalisation > Colours > Transparency effects
bool transparencyEffectsOn()
{
    DWORD v = 1, size = sizeof(v);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return true;
    return v != 0;
}

bool isCatcher(HWND h)
{
    wchar_t cls[32] = {};
    return h && ::GetClassNameW(h, cls, 31) && ::lstrcmpW(cls, kCatcherClass) == 0;
}

// The deepest visible, enabled child of root under a screen point (what WindowFromPoint would give).
HWND deepestChildAt(HWND root, POINT pt)
{
    HWND cur = root;
    for (int guard = 0; guard < 32; ++guard)
    {
        POINT cp = pt;
        ::ScreenToClient(cur, &cp);
        HWND c = ::ChildWindowFromPointEx(cur, cp, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!c || c == cur) break;
        cur = c;
    }
    return cur;
}

// The key: the theme's background with the lowest bit of red and blue flipped. Not a grey, so the
// chrome's greys never match it, and nobody can see the difference where it is not keyed.
COLORREF keyFor(COLORREF bg) { return bg ^ 0x00010001; }

class GlassText;
GlassText* s_glass = nullptr;
GlassStats s_stats;

// Drops onto the glass: turn it solid, so the next mouse move finds the view's own drop target.
class DropSolid : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = static_cast<IDropTarget*>(this); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }    // static lifetime
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override { *effect = DROPEFFECT_NONE; return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override { *effect = DROPEFFECT_NONE; return S_OK; }
};
DropSolid s_dropSolid;

struct Host {
    HWND wnd = nullptr;
    HWND catcher = nullptr;
    HWND blur = nullptr;    // smooth moving: the plain blur that moves instead of the acrylic
    bool addedLayered = false;
    bool dropRegistered = false;
    bool moving = false;
    bool swapped = false;   // the acrylic is hidden and the plain blur moves with the window
    RECT insets{};          // client area relative to the window rect, for WM_MOVING/WM_SIZING
};

struct ViewGlass { ViewMode mode = VIEW_NEW; COLORREF key = 0; int edges = -1; };

class GlassText : public Module {
public:
    const wchar_t* key() const override { return L"Glass"; }
    const wchar_t* title() const override { return L"Glass text area"; }
    bool defaultOn() const override { return false; }

    void loadOptions() override
    {
        _tint = std::max(0, std::min(100, settings.getInt(key(), L"Tint", 40)));
        _margins = settings.getInt(key(), L"Margins", 1) != 0;
        _keepRenderer = settings.getInt(key(), L"KeepRenderer", 0) != 0;
        std::wstring e = settings.getStr(key(), L"Engine", L"auto");
        if (e == L"self") _engine = E_SELF;
        else if (e == L"keyonly") _engine = E_KEYONLY;
        else _engine = E_BEHIND;
        _acrylic = settings.getInt(key(), L"Style", 0) == 1 && windowsBuild() >= 17134;   // acrylic needs 1803
        _edges = std::max(0, std::min(1, settings.getInt(key(), L"Edges", EDGES_SMOOTH)));
        _smoothMove = settings.getInt(key(), L"SmoothMove", 1) != 0;
    }

    // the plain blur moves instead of the acrylic (catchers only: with the blur on the window itself there is nothing to swap)
    bool smoothMove() const { return _smoothMove && _acrylic && _engine != E_SELF; }

    bool selfCheck(std::wstring& why) override
    {
        if (_engine == E_KEYONLY) return true;   // the self-test: keying and the catcher, no blur
        if (!compositionFn() && !underWine) { why = L"needs Windows 10 or later"; return false; }
        return true;
    }

    bool enable() override
    {
        loadOptions();
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = catcherProc; wc.hInstance = hInst; wc.lpszClassName = kCatcherClass;
            wc.hCursor = ::LoadCursor(nullptr, IDC_IBEAM);
            wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));   // alpha 0: the accent shows through
            registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        if (!createMsgWindow()) return false;
        s_glass = this;
        s_stats = GlassStats();
        _active = false;
        evaluate();
        ::SetTimer(_msg, TIMER_SYNC, 250, nullptr);
        return true;
    }

    void disable() override
    {
        _disabling = true;
        if (_msg) { ::KillTimer(_msg, TIMER_SYNC); ::KillTimer(_msg, TIMER_DRAG); ::KillTimer(_msg, TIMER_BLURHIDE); }
        deactivate();
        if (_msg) { ::DestroyWindow(_msg); _msg = nullptr; }
        if (s_glass == this) s_glass = nullptr;
        s_stats = GlassStats();
        _disabling = false;
    }

    void onNotify(SCNotification* n) override
    {
        switch (n->nmhdr.code)
        {
        case NPPN_BUFFERACTIVATED:
        case NPPN_LANGCHANGED:
        case NPPN_WORDSTYLESUPDATED:
        case NPPN_DARKMODECHANGED:
            if (_active && !_disabling) refreshViews();
            break;
        }
    }

    // ---- state ---------------------------------------------------------------------------------

    // Decide whether the glass can be on now (transparency effects, the blur call), and switch.
    void evaluate()
    {
        std::wstring why;
        if (_engine != E_KEYONLY)
        {
            if (!transparencyEffectsOn()) why = L"waiting: Windows transparency effects are off";
            else if (_blurRefused) why = L"Windows refused the blur on this PC";
        }
        if (why.empty() && !_active) activate();
        else if (!why.empty() && _active) deactivate();
        if (!why.empty()) s_stats.state = why;
    }

    void activate()
    {
        HWND mainView = views.sci(0);
        if (!mainView) return;
        const COLORREF bg = static_cast<COLORREF>(sci(mainView, SCI_STYLEGETBACK, STYLE_DEFAULT));
        _bg = bg;
        _key = keyFor(bg);
        _oldKey = 0xFFFFFFFF;
        _active = true;
        syncHosts();
        if (_blurRefused && _engine != E_KEYONLY)
        {
            deactivate();
            s_stats.state = L"Windows refused the blur on this PC";
            return;
        }
        refreshViews();
        s_stats.active = true;
        s_stats.key = _key;
        s_stats.state = engineText();
        log(L"glass: on (%ls, key %06lx, tint %d%%)", s_stats.state.c_str(), static_cast<unsigned long>(_key), _tint);
    }

    void deactivate()
    {
        const bool was = _active;
        _active = false;
        _dragSolid = false;
        // the views' see-through layers go first, so they never sit over a window that is no longer keyed
        for (auto& vg : _viewGlass)
            if (vg.second.mode == VIEW_LAYER && ::IsWindow(vg.first)) sci(vg.first, SCI_KEX_GLASS, 0, 0);
        _viewGlass.clear();
        for (auto& t : _viewTech)
            if (::IsWindow(t.first) && sci(t.first, SCI_GETTECHNOLOGY) == SC_TECHNOLOGY_DIRECTWRITE)
                sci(t.first, SCI_SETTECHNOLOGY, t.second);
        _viewTech.clear();
        for (Host& h : _hosts) detach(h);
        _hosts.clear();
        for (auto& t : _tech)
            if (::IsWindow(t.first) && sci(t.first, SCI_GETTECHNOLOGY) == SC_TECHNOLOGY_DIRECTWRITEDC)
                sci(t.first, SCI_SETTECHNOLOGY, t.second);
        _tech.clear();
        for (auto& q : _quality)
            if (::IsWindow(q.first)) sci(q.first, SCI_SETFONTQUALITY, q.second);
        _quality.clear();
        _from.clear();
        if (was && npp._nppHandle)
        {
            // Notepad++ sets every view's colours from the theme again (Style Configurator's "apply")
            const UINT WM_UPDATESCINTILLAS_ = WM_USER + 5000 + 1;   // WordStyleDlg.h: WORDSTYLE_USER + 1
            ::SendMessageW(npp._nppHandle, WM_UPDATESCINTILLAS_, FALSE, 0);
        }
        s_stats.active = false;
        s_stats.hosts = 0;
        s_stats.layerViews = s_stats.fallbackViews = 0;
        if (was) log(L"glass: off");
    }

    std::wstring engineText() const
    {
        if (_engine == E_KEYONLY) return L"colour key only (test mode)";
        std::wstring s = _acrylic ? L"acrylic blur" : L"blur";
        if (_engine == E_SELF) s += L" on the window";
        return s;
    }

    // ---- views ---------------------------------------------------------------------------------

    void refreshViews()
    {
        if (!_active) return;
        // a new theme: new key
        HWND mainView = views.sci(0);
        if (mainView)
        {
            COLORREF def = static_cast<COLORREF>(sci(mainView, SCI_STYLEGETBACK, STYLE_DEFAULT));
            if (def != _key && keyFor(def) != _key)
            {
                _oldKey = _key;
                _bg = def;
                _key = keyFor(def);
                s_stats.key = _key;
                for (Host& h : _hosts) applyKey(h);
                for (Host& h : _hosts) applyAccent(h);
                log(L"glass: theme background %06lx, new key %06lx", static_cast<unsigned long>(def), static_cast<unsigned long>(_key));
            }
        }
        for (int v = 0; v < views.count(); ++v)
            if (HWND s = views.sci(v)) { remapView(s); glassView(s); }
        countViews();
        ++s_stats.remaps;
    }

    bool viewNeedsRemap(HWND s)
    {
        if (static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT)) != _key) return true;
        auto it = _from.find(s);
        if (it != _from.end() && static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, 0)) == it->second) return true;
        if (_margins && static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_LINENUMBER)) != _key) return true;
        auto g = _viewGlass.find(s);
        if (g == _viewGlass.end()) return true;
        if (g->second.mode == VIEW_LAYER && (sci(s, SCI_KEX_GETGLASS) != 1 || g->second.key != _key)) return true;   // Notepad++ switched its renderer, or the layer failed
        if (g->second.mode == VIEW_FALLBACK && sci(s, SCI_GETFONTQUALITY) != SC_EFF_QUALITY_NON_ANTIALIASED) return true;
        return false;
    }

    // The view's text on its own see-through layer; where this PC can't make one, drawn through a DC with sharp edges.
    void glassView(HWND s)
    {
        ViewGlass& g = _viewGlass[s];
        if (g.mode == VIEW_FALLBACK) return;
        if (g.mode == VIEW_LAYER)
        {
            const LRESULT state = sci(s, SCI_KEX_GETGLASS);
            if (state == 1 && g.key == _key && g.edges == _edges) return;
            if (state == 2) { fallbackView(s, g, L"the layer stopped working"); return; }
        }
        // the layer is drawn with Direct2D: a view on GDI moves to DirectWrite while the glass is on
        if (sci(s, SCI_GETTECHNOLOGY) == SC_TECHNOLOGY_DEFAULT)
        {
            if (_viewTech.find(s) == _viewTech.end()) _viewTech[s] = SC_TECHNOLOGY_DEFAULT;
            sci(s, SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITE);
        }
        const WPARAM flags = KEXGLASS_ON | (_edges == EDGES_SHARP ? KEXGLASS_SHARP : 0);
        if (sci(s, SCI_KEX_GLASS, flags, static_cast<LPARAM>(_key)) == 1)
        {
            if (g.mode != VIEW_LAYER) ++s_stats.layerStarts;
            g.mode = VIEW_LAYER;
            g.key = _key;
            g.edges = _edges;
            return;
        }
        fallbackView(s, g, L"no see-through layer on this system");
    }

    void fallbackView(HWND s, ViewGlass& g, const wchar_t* why)
    {
        KexGlassInfo info{};
        sci(s, SCI_KEX_GETGLASS, 0, reinterpret_cast<LPARAM>(&info));
        sci(s, SCI_KEX_GLASS, 0, 0);
        g.mode = VIEW_FALLBACK;
        s_stats.layerError = static_cast<unsigned long>(info.lastError);
        auto t = _viewTech.find(s);
        if (t != _viewTech.end())
        {
            if (sci(s, SCI_GETTECHNOLOGY) == SC_TECHNOLOGY_DIRECTWRITE) sci(s, SCI_SETTECHNOLOGY, t->second);
            _viewTech.erase(t);
        }
        fixRenderer(s);
        const int q = static_cast<int>(sci(s, SCI_GETFONTQUALITY));
        if (q != SC_EFF_QUALITY_NON_ANTIALIASED)
        {
            if (_quality.find(s) == _quality.end()) _quality[s] = q;
            sci(s, SCI_SETFONTQUALITY, SC_EFF_QUALITY_NON_ANTIALIASED);
        }
        log(L"glass: view %d draws without a see-through layer (%ls, error 0x%08lx): sharp text through a DC", views.viewOf(s) + 1, why,
            static_cast<unsigned long>(info.lastError));
    }

    void countViews()
    {
        s_stats.layerViews = s_stats.fallbackViews = 0;
        for (auto& vg : _viewGlass)
        {
            if (vg.second.mode == VIEW_LAYER) ++s_stats.layerViews;
            if (vg.second.mode == VIEW_FALLBACK) ++s_stats.fallbackViews;
        }
    }

    void remapView(HWND s)
    {
        const COLORREF def = static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT));
        COLORREF from;
        if (def != _key && def != _oldKey) { from = def; _from[s] = def; }
        else
        {
            auto it = _from.find(s);
            from = it != _from.end() ? it->second : _bg;
        }
        for (int st = 0; st < 256; ++st)
        {
            if (st == STYLE_CALLTIP) continue;   // calltips are their own popup window
            const COLORREF b = static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, st));
            if (st == STYLE_LINENUMBER)
            {
                if (_margins && b != _key) sci(s, SCI_STYLESETBACK, st, _key);
                continue;
            }
            if (b == from || (b == _oldKey && _oldKey != _key)) sci(s, SCI_STYLESETBACK, st, _key);
        }
        if (_margins)
        {
            sci(s, SCI_SETFOLDMARGINCOLOUR, 1, _key);
            sci(s, SCI_SETFOLDMARGINHICOLOUR, 1, _key);
        }
    }

    // Scintilla windows inside the hosts: Direct2D window targets cannot draw into a layered window. The views
    // draw onto their own layer instead (glassView); only those without one are switched here.
    void fixRenderers(HWND host)
    {
        struct Ctx { GlassText* g; } ctx{ this };
        ::EnumChildWindows(host, [](HWND c, LPARAM p) -> BOOL {
            GlassText* g = reinterpret_cast<Ctx*>(p)->g;
            wchar_t cls[16] = {};
            if (::GetClassNameW(c, cls, 15) && ::lstrcmpW(cls, L"Scintilla") == 0)
            {
                auto vg = g->_viewGlass.find(c);
                const bool view = views.viewOf(c) >= 0;
                if (!view || (vg != g->_viewGlass.end() && vg->second.mode == VIEW_FALLBACK)) g->fixRenderer(c);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    }

    void fixRenderer(HWND c)
    {
        if (_keepRenderer || _tech.find(c) != _tech.end()) return;
        const int t = static_cast<int>(sci(c, SCI_GETTECHNOLOGY));
        if (t == SC_TECHNOLOGY_DIRECTWRITE || t == SC_TECHNOLOGY_DIRECTWRITERETAIN || t == SC_TECHNOLOGY_DIRECT_WRITE_1)
        {
            _tech[c] = t;
            sci(c, SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITEDC);
            ++s_stats.rendererSwitches;
        }
    }

    // ---- hosts ---------------------------------------------------------------------------------

    void syncHosts()
    {
        std::vector<HWND> want;
        if (npp._nppHandle) want.push_back(npp._nppHandle);
        for (HWND h : views.hostWindows()) want.push_back(h);
        for (size_t i = 0; i < _hosts.size();)
        {
            if (!::IsWindow(_hosts[i].wnd) || std::find(want.begin(), want.end(), _hosts[i].wnd) == want.end())
            {
                detach(_hosts[i]);
                _hosts.erase(_hosts.begin() + i);
            }
            else ++i;
        }
        for (HWND w : want)
        {
            Host* h = find(w);
            const bool added = !h;
            if (!h) { _hosts.push_back(Host()); h = &_hosts.back(); h->wnd = w; attach(*h); }
            if (added || _ticks % 8 == 0) fixRenderers(w);
        }
        s_stats.hosts = static_cast<int>(_hosts.size());
    }

    Host* find(HWND w)
    {
        for (Host& h : _hosts) if (h.wnd == w) return &h;
        return nullptr;
    }

    HWND makeCatcher(Host& h, const wchar_t* title)
    {
        const bool behind = _engine == E_BEHIND;
        // behind: a plain window carries the blur; otherwise an invisible layered one only catches input
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (behind ? 0 : WS_EX_LAYERED);
        HWND c = ::CreateWindowExW(ex, kCatcherClass, title, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
        if (c)
        {
            ::SetWindowLongPtrW(c, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(h.wnd));
            if (!behind) ::SetLayeredWindowAttributes(c, 0, 1, LWA_ALPHA);
        }
        return c;
    }

    void attach(Host& h)
    {
        ::SetWindowSubclass(h.wnd, hostSubclass, kHostSubclassId, reinterpret_cast<DWORD_PTR>(this));
        h.catcher = makeCatcher(h, L"KEX glass");
        if (h.catcher) h.dropRegistered = ::RegisterDragDrop(h.catcher, &s_dropSolid) == S_OK;
        if (smoothMove()) h.blur = makeCatcher(h, L"KEX glass (moving)");
        applyAccent(h);
        if (!_blurRefused || _engine == E_KEYONLY) applyKey(h);
        placeCatcher(h);
    }

    void detach(Host& h)
    {
        if (::IsWindow(h.wnd))
        {
            ::RemoveWindowSubclass(h.wnd, hostSubclass, kHostSubclassId);
            if (_engine == E_SELF) setAccent(h.wnd, ACCENT_DISABLED);
            if (h.addedLayered)
            {
                ::SetWindowLongPtrW(h.wnd, GWL_EXSTYLE, ::GetWindowLongPtrW(h.wnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
                ::RedrawWindow(h.wnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
            }
        }
        if (h.catcher)
        {
            if (h.dropRegistered) ::RevokeDragDrop(h.catcher);
            ::DestroyWindow(h.catcher);
        }
        if (h.blur) ::DestroyWindow(h.blur);
        h.catcher = nullptr;
        h.blur = nullptr;
        h.swapped = false;
        h.addedLayered = false;
    }

    void applyKey(Host& h)
    {
        LONG_PTR ex = ::GetWindowLongPtrW(h.wnd, GWL_EXSTYLE);
        const bool fresh = !(ex & WS_EX_LAYERED);
        if (fresh)
        {
            ::SetWindowLongPtrW(h.wnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
            h.addedLayered = true;
        }
        if (_dragSolid) ::SetLayeredWindowAttributes(h.wnd, 0, 255, LWA_ALPHA);
        else ::SetLayeredWindowAttributes(h.wnd, _key, 255, LWA_COLORKEY);
        if (fresh) ::RedrawWindow(h.wnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    bool setAccent(HWND w, int state)
    {
        SetCompositionFn fn = compositionFn();
        if (!fn) return false;
        DWORD a = static_cast<DWORD>(_tint * 255 / 100);
        if (state == ACCENT_ACRYLIC && a == 0) a = 1;   // acrylic with no tint at all draws nothing
        AccentPolicy p{ state, state == ACCENT_DISABLED ? 0 : 2, (a << 24) | (_bg & 0xFFFFFF), 0 };
        CompositionData d{ 19 /* WCA_ACCENT_POLICY */, &p, sizeof(p) };
        return fn(w, &d) != FALSE;
    }

    void applyAccent(Host& h)
    {
        if (_engine == E_KEYONLY) return;
        HWND w = _engine == E_BEHIND ? h.catcher : h.wnd;
        if (!w) return;
        const int state = _acrylic ? ACCENT_ACRYLIC : ACCENT_BLURBEHIND;
        if (h.blur && !setAccent(h.blur, ACCENT_BLURBEHIND)) ++s_stats.accentFailures;
        if (!setAccent(w, state))
        {
            ++s_stats.accentFailures;
            if (!underWine)
            {
                _blurRefused = true;
                log(L"glass: SetWindowCompositionAttribute failed (error %lu)", ::GetLastError());
                ::PostMessageW(_msg, WM_GLASS_REFRESH, 0, 0);
            }
        }
        else s_stats.accent = state;
    }

    void insetsOf(Host& h)
    {
        RECT wr, cr;
        ::GetWindowRect(h.wnd, &wr);
        ::GetClientRect(h.wnd, &cr);
        ::MapWindowPoints(h.wnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        h.insets = RECT{ cr.left - wr.left, cr.top - wr.top, wr.right - cr.right, wr.bottom - cr.bottom };
    }

    // the catcher that follows the window now: the plain blur while a move has swapped them
    static HWND activeCatcher(const Host& h) { return h.swapped && h.blur ? h.blur : h.catcher; }

    static void hideWindow(HWND w)
    {
        if (w && ::IsWindowVisible(w))
            ::SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    RECT catcherRect(Host& h, const RECT* windowRect)
    {
        RECT rc;
        if (windowRect)
            rc = RECT{ windowRect->left + h.insets.left, windowRect->top + h.insets.top, windowRect->right - h.insets.right, windowRect->bottom - h.insets.bottom };
        else
        {
            insetsOf(h);
            ::GetClientRect(h.wnd, &rc);
            ::MapWindowPoints(h.wnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        }
        return rc;
    }

    // cat goes right under after, over rc, on the host's topmost layer
    void putCatcher(Host& h, HWND cat, HWND after, const RECT& rc)
    {
        const bool hostTop = (::GetWindowLongPtrW(h.wnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const bool catTop = (::GetWindowLongPtrW(cat, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const UINT f = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;
        if (hostTop != catTop)
            ::SetWindowPos(cat, hostTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, f | SWP_NOMOVE | SWP_NOSIZE);
        ::SetWindowPos(cat, after, rc.left, rc.top, std::max(1L, rc.right - rc.left), std::max(1L, rc.bottom - rc.top), f | SWP_SHOWWINDOW);
    }

    void placeCatcher(Host& h, const RECT* windowRect = nullptr)
    {
        if (!h.catcher) return;
        const bool show = ::IsWindowVisible(h.wnd) && !::IsIconic(h.wnd);
        if (!show)
        {
            hideWindow(h.catcher);
            hideWindow(h.blur);
            return;
        }
        const RECT rc = catcherRect(h, windowRect);
        HWND cat = activeCatcher(h);
        if (!h.swapped && h.blur && ::IsWindowVisible(h.blur))
        {
            // left under the acrylic after a move: it must not show anywhere the window no longer is
            RECT br;
            ::GetWindowRect(h.blur, &br);
            if (!::EqualRect(&br, &rc)) hideWindow(h.blur);
        }
        putCatcher(h, cat, h.wnd, rc);
        ++s_stats.placements;
    }

    // The catcher must stay directly beneath its host, on the same virtual desktop.
    void checkPlacement(Host& h, bool desktop)
    {
        HWND cat = activeCatcher(h);
        if (!cat || !::IsWindowVisible(h.wnd) || ::IsIconic(h.wnd))
        {
            if ((h.catcher && ::IsWindowVisible(h.catcher)) || (h.blur && ::IsWindowVisible(h.blur))) placeCatcher(h);
            return;
        }
        HWND n = ::GetWindow(h.wnd, GW_HWNDNEXT);
        while (n && n != cat && !::IsWindowVisible(n)) n = ::GetWindow(n, GW_HWNDNEXT);
        RECT cr, kr;
        ::GetClientRect(h.wnd, &cr);
        ::MapWindowPoints(h.wnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        ::GetWindowRect(cat, &kr);
        if (n != cat || !::EqualRect(&cr, &kr) || !::IsWindowVisible(cat)) placeCatcher(h);
        // which desktop a window is on is a call into Explorer: once a second, and when the window is activated
        DesktopProvider* dp = desktop ? desktopProvider() : nullptr;
        if (dp && dp->ok())
        {
            GUID a{}, b{};
            if (dp->windowDesktop(h.wnd, &a))
                for (HWND c : { h.catcher, h.blur })
                    if (c && dp->windowDesktop(c, &b) && !::IsEqualGUID(a, b)) dp->moveWindow(c, a);
        }
    }

    void onHostMoving(HWND w, const RECT* proposed)
    {
        Host* h = find(w);
        if (!h || !proposed) return;
        if (h->moving && h->blur && !h->swapped && smoothMove())
        {
            // the first step of a drag: the plain blur (already drawn under the acrylic) takes over, the acrylic hides
            h->swapped = true;
            putCatcher(*h, h->blur, h->wnd, catcherRect(*h, proposed));
            hideWindow(h->catcher);
            ++s_stats.swaps;
            return;
        }
        placeCatcher(*h, proposed);   // move it with the window, not a frame later
    }

    void onSizeMove(HWND w, bool start)
    {
        Host* h = find(w);
        if (!h) return;
        h->moving = start;
        if (start)
        {
            insetsOf(*h);
            // smooth moving: the plain blur goes under the acrylic now, so it is drawn by the time the window moves
            if (h->blur && smoothMove() && h->catcher && ::IsWindowVisible(h->catcher))
            {
                ::KillTimer(_msg, TIMER_BLURHIDE);
                putCatcher(*h, h->blur, h->catcher, catcherRect(*h, nullptr));
            }
            return;
        }
        if (h->swapped)
        {
            // the acrylic comes back where the window ended up; the plain blur goes once the acrylic is drawn
            h->swapped = false;
            putCatcher(*h, h->catcher, h->wnd, catcherRect(*h, nullptr));
            ::SetTimer(_msg, TIMER_BLURHIDE, kBlurHideMs, nullptr);
            ++s_stats.swapsBack;
        }
        else hideWindow(h->blur);   // the window didn't move after all
    }

    void blurHideTick()
    {
        ::KillTimer(_msg, TIMER_BLURHIDE);
        for (Host& h : _hosts)
            if (!h.swapped) hideWindow(h.blur);
    }

    void onHostPosChanged(HWND w)
    {
        Host* h = find(w);
        if (h) placeCatcher(*h);
    }

    // ---- drag and drop over the glass ----------------------------------------------------------

    void dragSolid()
    {
        if (!_active || _dragSolid) return;
        _dragSolid = true;
        ++s_stats.dragSolids;
        for (Host& h : _hosts) applyKey(h);
        ::SetTimer(_msg, TIMER_DRAG, 100, nullptr);
        _dragSince = nowMs();
    }

    void dragTick()
    {
        const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) || (::GetAsyncKeyState(VK_MBUTTON) & 0x8000);
        if (down && nowMs() - _dragSince < 120000) return;
        ::KillTimer(_msg, TIMER_DRAG);
        _dragSolid = false;
        for (Host& h : _hosts) applyKey(h);
    }

    // ---- input on the catcher ------------------------------------------------------------------

    static LRESULT forwardMouse(HWND catcher, UINT m, WPARAM w, LPARAM l)
    {
        HWND host = reinterpret_cast<HWND>(::GetWindowLongPtrW(catcher, GWLP_USERDATA));
        if (!host || !::IsWindow(host)) return 0;
        POINT pt{ static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)) };
        ::ClientToScreen(catcher, &pt);
        HWND t = deepestChildAt(host, pt);
        if (!t) return 0;
        const bool down = m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_XBUTTONDOWN;
        if (down)
        {
            HWND root = ::GetAncestor(host, GA_ROOT);
            if (::GetForegroundWindow() != root) ::SetForegroundWindow(root);
            ++s_stats.clicks;
            s_stats.lastTarget = t;
        }
        POINT c = pt;
        ::ScreenToClient(t, &c);
        return ::SendMessageW(t, m, w, MAKELPARAM(c.x, c.y));
    }

    static LRESULT CALLBACK catcherProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        static int depth = 0;
        try
        {
            switch (m)
            {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_SETCURSOR:
            {
                HWND host = reinterpret_cast<HWND>(::GetWindowLongPtrW(h, GWLP_USERDATA));
                POINT pt; ::GetCursorPos(&pt);
                HWND t = host ? deepestChildAt(host, pt) : nullptr;
                if (t && t != h && depth == 0)
                {
                    ++depth;
                    LRESULT r = ::SendMessageW(t, WM_SETCURSOR, reinterpret_cast<WPARAM>(t), MAKELPARAM(HTCLIENT, HIWORD(l)));
                    --depth;
                    if (r) return TRUE;
                }
                break;
            }
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            {
                if (depth) return 0;
                ++depth;
                LRESULT r = forwardMouse(h, m, w, l);
                --depth;
                return (m == WM_XBUTTONDOWN || m == WM_XBUTTONUP || m == WM_XBUTTONDBLCLK) ? TRUE : r;
            }
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            {
                if (depth) return 0;
                HWND host = reinterpret_cast<HWND>(::GetWindowLongPtrW(h, GWLP_USERDATA));
                POINT pt{ static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)) };
                HWND t = host ? deepestChildAt(host, pt) : nullptr;
                if (!t || t == h) return 0;
                ++depth;
                ++s_stats.wheels;
                ::SendMessageW(t, m, w, l);
                --depth;
                return 0;
            }
            case WM_CONTEXTMENU:
                return 0;   // the view got the button-up and opens its own menu
            case WM_PAINT:
            {
                PAINTSTRUCT ps; ::BeginPaint(h, &ps); ::EndPaint(h, &ps);
                return 0;
            }
            }
        }
        catch (...) { log(L"glass: exception in the click catcher (message 0x%x)", m); }
        return ::DefWindowProcW(h, m, w, l);
    }

    static LRESULT CALLBACK hostSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref)
    {
        GlassText* g = reinterpret_cast<GlassText*>(ref);
        try
        {
            switch (m)
            {
            case WM_MOVING:
            case WM_SIZING:
            {
                LRESULT r = ::DefSubclassProc(h, m, w, l);
                if (g == s_glass) g->onHostMoving(h, reinterpret_cast<RECT*>(l));
                return r;
            }
            case WM_WINDOWPOSCHANGED:
            case WM_SHOWWINDOW:
            case WM_ACTIVATE:
            {
                LRESULT r = ::DefSubclassProc(h, m, w, l);
                if (g == s_glass) g->onHostPosChanged(h);
                if (g == s_glass && m == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE)
                    if (Host* host = g->find(h)) g->checkPlacement(*host, true);   // it may have been moved to another desktop
                return r;
            }
            case WM_ENTERSIZEMOVE:
                if (g == s_glass) g->onSizeMove(h, true);
                break;
            case WM_EXITSIZEMOVE:
                if (g == s_glass) g->onSizeMove(h, false);
                break;
            case WM_SETTINGCHANGE:
            case WM_DWMCOMPOSITIONCHANGED:
            case WM_THEMECHANGED:
                if (g == s_glass && g->_msg) ::PostMessageW(g->_msg, WM_GLASS_REFRESH, 0, 0);
                break;
            case WM_NCDESTROY:
                ::RemoveWindowSubclass(h, hostSubclass, kHostSubclassId);
                break;
            }
        }
        catch (...) { log(L"glass: exception in the host window hook (message 0x%x)", m); }
        return ::DefSubclassProc(h, m, w, l);
    }

    // ---- housekeeping --------------------------------------------------------------------------

    bool createMsgWindow()
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSW wc{};
            wc.lpfnWndProc = msgProc; wc.hInstance = hInst; wc.lpszClassName = L"KexGlassMsg";
            registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }
        _msg = ::CreateWindowExW(0, L"KexGlassMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
        return _msg != nullptr;
    }

    void tick()
    {
        ++_ticks;
        if (!_active) { evaluate(); return; }
        syncHosts();
        for (Host& h : _hosts) checkPlacement(h, _ticks % 4 == 0);
        bool stale = false;
        for (int v = 0; v < views.count() && !stale; ++v)
            if (HWND s = views.sci(v)) stale = viewNeedsRemap(s);
        if (stale) refreshViews();
    }

    static LRESULT CALLBACK msgProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        GlassText* g = s_glass;
        if (g && h == g->_msg)
        {
            try
            {
                if (m == WM_TIMER && w == TIMER_SYNC) { g->tick(); return 0; }
                if (m == WM_TIMER && w == TIMER_DRAG) { g->dragTick(); return 0; }
                if (m == WM_TIMER && w == TIMER_BLURHIDE) { g->blurHideTick(); return 0; }
                if (m == WM_GLASS_REFRESH) { g->evaluate(); refreshPrefPages(); return 0; }
            }
            catch (...) { log(L"glass: exception in its timer"); }
        }
        return ::DefWindowProcW(h, m, w, l);
    }

    HWND _msg = nullptr;
    std::vector<Host> _hosts;
    std::map<HWND, int> _tech;          // Scintilla windows switched to DirectWriteDC: their own technology
    std::map<HWND, COLORREF> _from;     // each view's own background before the key replaced it
    std::map<HWND, int> _quality;       // views whose text smoothing was switched: their own setting
    std::map<HWND, ViewGlass> _viewGlass;   // each view: on its see-through layer, or drawing as before
    std::map<HWND, int> _viewTech;      // views moved from GDI to DirectWrite for the layer: their own technology
    COLORREF _bg = 0, _key = 0, _oldKey = 0xFFFFFFFF;
    int _tint = 40, _edges = EDGES_SMOOTH;
    bool _margins = true, _keepRenderer = false, _acrylic = true, _smoothMove = true;
    Engine _engine = E_BEHIND;
    bool _active = false, _disabling = false, _blurRefused = false, _dragSolid = false;
    double _dragSince = 0;
    unsigned _ticks = 0;
};

HRESULT DropSolid::DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect)
{
    *effect = DROPEFFECT_NONE;
    if (s_glass) { try { s_glass->dragSolid(); } catch (...) {} }
    return S_OK;
}

} // namespace

Module* createGlassText() { return new GlassText(); }

GlassStats glassStats()
{
    GlassStats st = s_stats;
    if (s_glass) st.dragSolidNow = s_glass->_dragSolid;
    return st;
}

void glassTestDragEnter()
{
    DWORD effect = DROPEFFECT_COPY;
    POINTL pt{ 0, 0 };
    s_dropSolid.DragEnter(nullptr, 0, pt, &effect);
}

std::wstring glassInfo()
{
    Module* m = modules[M_GLASS];
    if (!m || !m->isEnabled()) return L"";
    if (!s_stats.active) return s_stats.state;
    std::wstring s = L"On: " + s_stats.state + L". Clicks on the glass go to the text under them.";
    const int edges = settings.getInt(m->key(), L"Edges", EDGES_SMOOTH);
    if (s_stats.layerViews > 0 && s_stats.fallbackViews == 0)
        s += edges == EDGES_SHARP ? L" Text sits on its own see-through layer, drawn without smoothing."
                                  : L" Text sits on its own see-through layer, smoothed in grey, so letters have clean edges.";
    else if (s_stats.fallbackViews > 0)
        s += L" This PC can't give the text a see-through layer, so it is drawn without smoothing (smoothed edges would show an outline).";
    return s;
}

} // namespace kex

namespace kexsuite {

HWND windowFromPoint(POINT screenPoint)
{
    HWND h = ::WindowFromPoint(screenPoint);
    if (kex::isCatcher(h))
    {
        HWND host = reinterpret_cast<HWND>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (host && ::IsWindow(host)) return kex::deepestChildAt(host, screenPoint);
    }
    return h;
}

} // namespace kexsuite
