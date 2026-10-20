// Core.cpp - the suite's entry points (KexSuiteHost.h), module lifecycle, startup self-check,
// the shared main-window subclass and the non-modal note window.
#include "KexSuite.h"
#include <cwchar>
#include <shlobj.h>

namespace kex {

NppData npp{};
HINSTANCE hInst = nullptr;
bool selfTestMode = false;
bool underWine = false;
Module* modules[M_COUNT] = {};
MainHooks mainHooks;

static std::wstring g_cmdlineArgs;
static bool g_ready = false, g_shutdown = false, g_subclassed = false;
static const UINT_PTR kSubclassId = 0x4B455853;   // 'KEXS'

// ---------------------------------------------------------------------------------------------
// switching features on and off (Settings > Preferences > the KEX pages)

bool setModuleEnabled(int id, bool on, bool interactive)
{
    Module* m = modules[id];
    if (!m) return false;
    bool result = true;
    if (on && !m->isEnabled())
    {
        std::wstring why;
        bool ok = false;
        try { m->loadOptions(); ok = m->selfCheck(why); }
        catch (...) { ok = false; why = L"self-check failed with an exception"; }
        m->_checkOk = ok;
        m->_checkMsg = ok ? L"OK" : why;
        if (ok)
        {
            try { ok = m->enable(); }
            catch (...) { ok = false; }
            if (!ok)
            {
                try { m->disable(); } catch (...) {}
                m->_checkMsg = L"could not start";
            }
        }
        m->_enabled = ok;
        if (!ok)
        {
            log(L"%ls: not enabled (%ls)", m->title(), m->_checkMsg.c_str());
            if (interactive) showNote(std::wstring(L"KEX: ") + m->title() + L" is unavailable: " + m->_checkMsg, 7000);
            result = false;
        }
        else log(L"%ls: enabled", m->title());
    }
    else if (!on && m->isEnabled())
    {
        try { m->disable(); } catch (...) { log(L"%ls: exception while disabling", m->title()); }
        m->_enabled = false;
        log(L"%ls: disabled", m->title());
    }
    if (interactive && result) settings.setInt(m->key(), L"Enabled", on ? 1 : 0);
    refreshPrefPages();
    return result;
}

// ---------------------------------------------------------------------------------------------
// post-it detection. Notepad++ has no query message; post-it mode (Notepad_plus::postItToggle)
// sets the main window style to WS_POPUP and hides the tab bar and the status bar.

bool isPostItMode()
{
    LONG_PTR st = ::GetWindowLongPtrW(npp._nppHandle, GWL_STYLE);
    bool captionless = (st & WS_CAPTION) != WS_CAPTION;
    return captionless && nppMsg(NPPM_ISTABBARHIDDEN) != 0 && nppMsg(NPPM_ISSTATUSBARHIDDEN) != 0;
}

// ---------------------------------------------------------------------------------------------
// non-modal note window

static HWND g_note = nullptr;
static std::wstring g_noteText;

static LRESULT CALLBACK noteProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONUP:
    case WM_TIMER:
        ::KillTimer(h, 1);
        ::ShowWindow(h, SW_HIDE);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps; HDC dc = ::BeginPaint(h, &ps);
        RECT rc; ::GetClientRect(h, &rc);
        HBRUSH bg = ::CreateSolidBrush(RGB(255, 251, 214));
        ::FillRect(dc, &rc, bg); ::DeleteObject(bg);
        ::FrameRect(dc, &rc, static_cast<HBRUSH>(::GetStockObject(GRAY_BRUSH)));
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(40, 40, 40));
        HGDIOBJ of = ::SelectObject(dc, ::GetStockObject(DEFAULT_GUI_FONT));
        RECT tr = rc; ::InflateRect(&tr, -10, -8);
        ::DrawTextW(dc, g_noteText.c_str(), -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        ::SelectObject(dc, of);
        ::EndPaint(h, &ps);
        return 0;
    }
    }
    return ::DefWindowProcW(h, m, w, l);
}

void showNote(const std::wstring& text, int ms)
{
    if (g_shutdown || !npp._nppHandle) return;
    if (!g_note)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = noteProc; wc.hInstance = hInst; wc.lpszClassName = L"KexSuiteNote";
        wc.hCursor = ::LoadCursor(nullptr, IDC_HAND);
        ::RegisterClassW(&wc);
        g_note = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"KexSuiteNote", L"KEX",
            WS_POPUP, 0, 0, 10, 10, npp._nppHandle, nullptr, hInst, nullptr);
        if (!g_note) return;
    }
    g_noteText = text;
    HDC dc = ::GetDC(g_note);
    HGDIOBJ of = ::SelectObject(dc, ::GetStockObject(DEFAULT_GUI_FONT));
    RECT tr{ 0, 0, 400, 0 };
    ::DrawTextW(dc, text.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    ::SelectObject(dc, of); ::ReleaseDC(g_note, dc);
    int w = tr.right + 20, h = tr.bottom + 16;
    RECT nr; ::GetWindowRect(npp._nppHandle, &nr);
    ::SetWindowPos(g_note, HWND_TOPMOST, nr.right - w - 24, nr.bottom - h - 40, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(g_note, nullptr, TRUE);
    ::SetTimer(g_note, 1, ms, nullptr);
    log(L"note: %ls", text.c_str());
}

// ---------------------------------------------------------------------------------------------
// main window subclass, shared by the modules through mainHooks

static LRESULT CALLBACK mainSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    switch (m)
    {
    case WM_COMMAND:
        // the KEX group in the Settings menu (SettingsMenu.cpp)
        if (l == 0 && HIWORD(w) <= 1)
        {
            try { if (settingsMenuCommand(LOWORD(w))) return 0; }
            catch (...) { log(L"exception in a KEX Settings menu command"); return 0; }
        }
        break;
    case WM_INITMENUPOPUP:
        try { settingsMenuPopup(reinterpret_cast<HMENU>(w)); } catch (...) {}
        break;
    case WM_COPYDATA:
    {
        // a new Notepad++ on another desktop asks for a copy of the tabs (DesktopSync.cpp)
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (cds && cds->dwData == 0x4B455854)   // 'KEXT'
        {
            try { return desktopServeCopy(reinterpret_cast<HWND>(w), cds); }
            catch (...) { log(L"exception while copying the tabs for a new window"); return 0; }
        }
        break;
    }
    case WM_WINDOWPOSCHANGED:
    case WM_ACTIVATEAPP:
    case WM_SIZE:
    {
        LRESULT r = ::DefSubclassProc(h, m, w, l);
        if (mainHooks.onMainMoved) { try { mainHooks.onMainMoved(); } catch (...) {} }
        return r;
    }
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(h, mainSubclass, kSubclassId);
        g_subclassed = false;
        break;
    }
    return ::DefSubclassProc(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// startup

class CoreWindow : public MsgWindow {
protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_TIMER && w == 1)
        {
            ::KillTimer(hwnd(), 1);
            int ok = 0; std::wstring failed;
            for (int i = 0; i < M_COUNT; ++i)
            {
                if (modules[i]->_checkOk) ++ok;
                else failed += std::wstring(L"\n- ") + modules[i]->title() + L": " + modules[i]->_checkMsg;
            }
            wchar_t s[128]; swprintf(s, 128, L"KEX: %d/%d features OK", ok, M_COUNT);
            nppMsg(NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE, reinterpret_cast<LPARAM>(s));
            if (!failed.empty() || settings.getInt(L"General", L"AlwaysShowSummary", 0))
                showNote(std::wstring(s) + (failed.empty() ? L"" : L" (switched off:)") + failed, 9000);
            return 0;
        }
        return MsgWindow::onMessage(m, w, l);
    }
};
static CoreWindow g_core;

// After an update Windows can keep showing the old app icon (Start menu, taskbar pins) from its icon
// cache. The first time a new notepad++.exe runs, it tells the shell its icon may have changed.
static void refreshShellIconOnce()
{
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!::GetFileAttributesExW(exe, GetFileExInfoStandard, &fa)) return;
    wchar_t stamp[24];
    swprintf(stamp, 24, L"%08lx%08lx", fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime);
    if (settings.getStr(L"General", L"IconRefreshedFor") == stamp) return;
    settings.setStr(L"General", L"IconRefreshedFor", stamp);
    ::SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, exe, nullptr);
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    log(L"new notepad++.exe: asked the shell to refresh its icon");
}

static void onReady()
{
    g_ready = true;
    views.init();
    log(L"Notepad++ version word 0x%08lx, KEX views: %d (patch v%d), Wine: %ls",
        static_cast<long>(nppMsg(NPPM_GETNPPVERSION, 1, 0)), views.count(), views.patchVersion(), underWine ? L"yes" : L"no");
    g_core.createMsgWindow(L"KexSuiteCore");
    if (::SetWindowSubclass(npp._nppHandle, mainSubclass, kSubclassId, 0)) g_subclassed = true;
    else log(L"could not subclass the Notepad++ main window");
    buildSettingsMenu();
    refreshShellIconOnce();

    if (selfTestMode) useFakeDesktops(true);
    try { desktopLoadPendingCopy(); }   // a new window on its own desktop: the first window's tabs, before the features start
    catch (...) { log(L"exception while opening the copied tabs"); }

    // Startup self-check: every module verifies what it relies on, then the enabled ones start.
    for (int i = 0; i < M_COUNT; ++i)
    {
        Module* m = modules[i];
        std::wstring why;
        bool ok = false;
        try { m->loadOptions(); ok = m->selfCheck(why); }
        catch (...) { ok = false; why = L"self-check failed with an exception"; }
        m->_checkOk = ok;
        m->_checkMsg = ok ? L"OK" : why;
        log(L"self-check %-34ls %ls", m->title(), ok ? L"OK" : why.c_str());
    }
    for (int i = 0; i < M_COUNT; ++i)
    {
        Module* m = modules[i];
        if (settings.getInt(m->key(), L"Enabled", m->defaultOn() ? 1 : 0) && m->_checkOk)
            setModuleEnabled(i, true, false);
    }
    refreshPrefPages();
    ::SetTimer(g_core.hwnd(), 1, 600, nullptr);   // status bar summary once the first buffer is shown
    if (selfTestMode) selfTestStart(g_cmdlineArgs);
}

static void onShutdown()
{
    if (g_shutdown) return;
    g_shutdown = true;
    for (int i = M_COUNT - 1; i >= 0; --i)
        if (modules[i] && modules[i]->isEnabled())
        {
            try { modules[i]->disable(); } catch (...) {}
            modules[i]->_enabled = false;   // the INI keeps the user's choice
        }
    if (g_subclassed) { ::RemoveWindowSubclass(npp._nppHandle, mainSubclass, kSubclassId); g_subclassed = false; }
    if (g_note) { ::DestroyWindow(g_note); g_note = nullptr; }
    g_core.destroyMsgWindow();
}

} // namespace kex

// ---------------------------------------------------------------------------------------------
// entry points (KexSuiteHost.h)

namespace kexsuite {

using namespace kex;

void setInfo(HINSTANCE inst, const NppData& d)
{
    hInst = inst;
    underWine = ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    npp = d;
    try
    {
        settings.init();
        modules[M_LETTERS] = createFallingLetters();
        modules[M_CARET] = createSmoothCaret();
        modules[M_HOTKEY] = createHotkeyDrop();
        modules[M_TABS] = createHoverTabs();
        modules[M_DESKTOP] = createDesktopSync();
        modules[M_SNAP] = createTabSnap();
        modules[M_KEYS] = createJetBrainsKeys();
        modules[M_GLASS] = createGlassText();
        modules[M_TOKENS] = createTokenStyles();
        modules[M_FLASH] = createEditFlash();
        modules[M_SLIDE] = createTabSlide();
        modules[M_SPLASH] = createPaintSplash();
        modules[M_SWIPE] = createTabSwipe();
    }
    catch (...) { for (Module*& m : modules) m = nullptr; }
}

void notify(SCNotification* n)
{
    if (!modules[0]) return;
    try
    {
        switch (n->nmhdr.code)
        {
        case NPPN_CMDLINEPLUGINMSG:
        {
            const wchar_t* s = reinterpret_cast<const wchar_t*>(n->nmhdr.idFrom);
            if (s && wcsstr(s, L"KexSuite_selftest=1")) { selfTestMode = true; g_cmdlineArgs = s; }
            return;
        }
        case NPPN_READY:
            onReady();
            return;
        case NPPN_SHUTDOWN:
            for (int i = 0; i < M_COUNT; ++i)
                if (modules[i] && modules[i]->isEnabled()) { try { modules[i]->onNotify(n); } catch (...) {} }
            onShutdown();
            return;
        }
    }
    catch (...) { log(L"exception in notification %u", n->nmhdr.code); return; }
    if (!g_ready || g_shutdown) return;
    for (int i = 0; i < M_COUNT; ++i)
    {
        Module* m = modules[i];
        if (m && m->isEnabled())
        {
            try { m->onNotify(n); }
            catch (...) { log(L"%ls: exception in notification %u, switching it off", m->title(), n->nmhdr.code); setModuleEnabled(i, false, false); }
        }
    }
    if (selfTestMode) { try { selfTestOnNotify(n); } catch (...) {} }
}

} // namespace kexsuite
