// Util.cpp - logging, settings, message windows and the view abstraction.
#include <atomic>
#include <cmath>
#include "KexSuite.h"
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <dwmapi.h>

namespace kex {

Views views;
Settings settings;

// ---------------------------------------------------------------------------------------------
// small shared helpers

SciDirect::SciDirect(HWND w) : h(w)
{
    if (!h) return;
    fn = reinterpret_cast<SciFnDirect>(::SendMessageW(h, SCI_GETDIRECTFUNCTION, 0, 0));
    p = static_cast<sptr_t>(::SendMessageW(h, SCI_GETDIRECTPOINTER, 0, 0));
}

int dpiOf(HWND h)
{
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static const GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && h) { const UINT d = fn(h); if (d) return static_cast<int>(d); }
    HDC dc = ::GetDC(h);
    const int d = dc ? ::GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ::ReleaseDC(h, dc);
    return d > 0 ? d : 96;
}

int dpiAt(POINT pt)
{
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    static HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    static const GetDpiForMonitorFn fn = shcore ? reinterpret_cast<GetDpiForMonitorFn>(::GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
    UINT x = 0, y = 0;
    if (fn && SUCCEEDED(fn(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && y) return static_cast<int>(y);
    return dpiOf(nullptr);
}

COLORREF mixColour(COLORREF a, COLORREF b, double t)
{
    return RGB(static_cast<int>(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               static_cast<int>(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               static_cast<int>(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

bool isCloaked(HWND h)
{
    DWORD c = 0;
    return h && SUCCEEDED(::DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0;
}

// {AA509086-5CA9-4C25-8F95-589D3C07B48A}, {A5CD92FF-29BE-454C-8D04-D82879FB3F1B}
const CLSID kCLSID_VirtualDesktopManager = { 0xaa509086, 0x5ca9, 0x4c25, { 0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a } };
const IID kIID_IVirtualDesktopManager = { 0xa5cd92ff, 0x29be, 0x454c, { 0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b } };

// ---------------------------------------------------------------------------------------------
// logging

static FILE* g_log = nullptr;
extern FILE* selfTestLogFile();   // SelfTest.cpp: non-null while the self-test runs

void log(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 2047, fmt, ap); va_end(ap); buf[2047] = 0;
    if (FILE* t = selfTestLogFile())
    {
        fwprintf(t, L"  [kex] %ls\n", buf); fflush(t);
        return;
    }
    if (!g_log && !settings.dir().empty())
    {
        std::wstring p = settings.dir() + L"\\" + logFileName();   // kex.log, or the other desktop's own
        g_log = _wfopen(p.c_str(), L"w, ccs=UTF-8");
    }
    if (g_log)
    {
        SYSTEMTIME st; ::GetLocalTime(&st);
        fwprintf(g_log, L"%02d:%02d:%02d.%03d %ls\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(g_log);
    }
}

// ---------------------------------------------------------------------------------------------
// helpers

std::wstring bufferPath(intptr_t bufferId)
{
    if (!bufferId) return L"";
    LRESULT n = nppMsg(NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferId), 0);
    if (n <= 0) return L"";
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    nppMsg(NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferId), reinterpret_cast<LPARAM>(&s[0]));
    s.resize(wcslen(s.c_str()));
    return s;
}

std::wstring fileNameOf(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

double nowMs()
{
    static LARGE_INTEGER f{};
    if (!f.QuadPart) ::QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; ::QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

bool rectsIntersect(const RECT& a, const RECT& b)
{
    RECT r; return ::IntersectRect(&r, &a, &b) != FALSE;
}

// ---------------------------------------------------------------------------------------------
// MsgWindow

static const wchar_t* kMsgClass = L"KexSuiteMsgWindow";

bool MsgWindow::createMsgWindow(const wchar_t* name)
{
    if (_hwnd) return true;
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = hInst;
        wc.lpszClassName = kMsgClass;
        registered = ::RegisterClassW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    _hwnd = ::CreateWindowExW(0, kMsgClass, name, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, this);
    return _hwnd != nullptr;
}

void MsgWindow::destroyMsgWindow()
{
    if (_hwnd)
    {
        ::SetWindowLongPtrW(_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
}

LRESULT CALLBACK MsgWindow::proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        auto* self = static_cast<MsgWindow*>(cs->lpCreateParams);
        self->_hwnd = h;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MsgWindow*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self)
    {
        try { return self->onMessage(m, w, l); }
        catch (...) { log(L"exception in message handler (msg 0x%x)", m); return 0; }
    }
    return ::DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Animation speed

namespace {
std::atomic<int> g_animSpeed{ -1 };   // read on the effects' own thread too
}

int animSpeedAll()
{
    int v = g_animSpeed.load(std::memory_order_relaxed);
    if (v < 0)
    {
        v = std::clamp(settings.getInt(L"General", L"AnimationSpeed", 100), kSpeedMin, kSpeedMax);
        g_animSpeed.store(v, std::memory_order_relaxed);
    }
    return v;
}

void setAnimSpeedAll(int percent)
{
    percent = std::clamp(percent, kSpeedMin, kSpeedMax);
    settings.setInt(L"General", L"AnimationSpeed", percent);
    g_animSpeed.store(percent, std::memory_order_relaxed);
}

int animSpeed(const wchar_t* sec, const SpeedDef& s)
{
    int pct = settings.getInt(sec, s.key, -1);
    if (pct < 0 && s.oldMsKey)
    {
        // a time set before speeds were percentages: the same time (0, which switched some off, is the fastest)
        const int old = settings.getInt(sec, s.oldMsKey, -1);
        if (old > 0) pct = static_cast<int>(std::lround(s.baseMs * 100.0 / old));
        else if (old == 0) pct = kSpeedMax;
    }
    return std::clamp(pct < 0 ? s.defPercent : pct, kSpeedMin, kSpeedMax);
}

double animStretch(int percent)
{
    return 100.0 / std::clamp(percent, kSpeedMin, kSpeedMax) * 100.0 / animSpeedAll();
}

int animMs(double baseMs, int percent)
{
    return std::max(1, static_cast<int>(std::lround(baseMs * animStretch(percent))));
}

// ---------------------------------------------------------------------------------------------
// Settings

void Settings::init()
{
    // Notepad++'s own settings folder (%APPDATA%\\Notepad++, or the program folder when portable)
    wchar_t dir[MAX_PATH * 2] = {};
    nppMsg(NPPM_GETNPPSETTINGSDIRPATH, MAX_PATH * 2 - 1, reinterpret_cast<LPARAM>(dir));
    _dir = dir;
    if (!_dir.empty() && _dir.back() == L'\\') _dir.pop_back();
    if (_dir.empty())
    {
        wchar_t tmp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tmp); _dir = tmp;
        if (!_dir.empty() && _dir.back() == L'\\') _dir.pop_back();
    }
    ::CreateDirectoryW(_dir.c_str(), nullptr);
    _path = _dir + L"\\kex.ini";

    // the suite used to be the KexSuite plugin: take over its settings and quick note once
    wchar_t old[MAX_PATH * 2] = {};
    nppMsg(NPPM_GETPLUGINSCONFIGDIR, MAX_PATH * 2 - 1, reinterpret_cast<LPARAM>(old));
    if (old[0])
    {
        std::wstring o = old;
        if (::GetFileAttributesW(_path.c_str()) == INVALID_FILE_ATTRIBUTES)
            ::CopyFileW((o + L"\\KexSuite.ini").c_str(), _path.c_str(), TRUE);
        std::wstring note = _dir + L"\\kex-quicknote.txt";
        if (::GetFileAttributesW(note.c_str()) == INVALID_FILE_ATTRIBUTES)
            ::CopyFileW((o + L"\\KexQuickNote.txt").c_str(), note.c_str(), TRUE);
    }
}

int Settings::getInt(const wchar_t* sec, const wchar_t* key, int def) const
{
    return static_cast<int>(::GetPrivateProfileIntW(sec, key, def, _path.c_str()));
}

void Settings::setInt(const wchar_t* sec, const wchar_t* key, int v) const
{
    wchar_t b[32]; swprintf(b, 32, L"%d", v);
    ::WritePrivateProfileStringW(sec, key, b, _path.c_str());
}

std::wstring Settings::getStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) const
{
    std::vector<wchar_t> b(4096);
    DWORD n = ::GetPrivateProfileStringW(sec, key, def, b.data(), static_cast<DWORD>(b.size()), _path.c_str());
    return std::wstring(b.data(), n);
}

void Settings::setStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) const
{
    ::WritePrivateProfileStringW(sec, key, v.c_str(), _path.c_str());
}

void Settings::deleteSection(const wchar_t* sec) const
{
    ::WritePrivateProfileStringW(sec, nullptr, nullptr, _path.c_str());
}

std::vector<std::wstring> Settings::sectionNames() const
{
    std::vector<wchar_t> b(65536);
    DWORD n = ::GetPrivateProfileSectionNamesW(b.data(), static_cast<DWORD>(b.size()), _path.c_str());
    std::vector<std::wstring> out;
    for (DWORD i = 0; i < n && b[i];)
    {
        std::wstring s(&b[i]);
        out.push_back(s);
        i += static_cast<DWORD>(s.size()) + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Views

void Views::init()
{
    // the suite is built into the KEX Notepad++, so the four-view messages are always there
    LRESULT r = nppMsg(NPPM_KEX_GETINFO);
    _patched = r != 0;
    _count = std::max(2, std::min(4, static_cast<int>(LOWORD(r))));
    _patchVersion = HIWORD(r);
}

HWND Views::sci(int v) const
{
    if (v < 0 || v >= _count) return nullptr;
    return reinterpret_cast<HWND>(nppMsg(NPPM_KEX_GETVIEWSCINTILLA, v));
}

int Views::current() const { return static_cast<int>(nppMsg(NPPM_KEX_GETCURRENTVIEW)); }

bool Views::visible(int v) const
{
    return v >= 0 && v < _count && nppMsg(NPPM_KEX_ISVIEWVISIBLE, v) != 0;
}

int Views::nbFiles(int v) const
{
    return (v < 0 || v >= _count) ? 0 : static_cast<int>(nppMsg(NPPM_KEX_GETNBVIEWFILES, v));
}

intptr_t Views::bufferId(int v, int idx) const
{
    return (v < 0 || v >= _count || idx < 0) ? 0 : static_cast<intptr_t>(nppMsg(NPPM_KEX_GETVIEWBUFFERID, v, idx));
}

int Views::currentIndex(int v) const
{
    return (v < 0 || v >= _count) ? -1 : static_cast<int>(nppMsg(NPPM_KEX_GETVIEWCURRENTINDEX, v));
}

void Views::activate(int v, int idx) const
{
    if (v >= 0 && v < _count && idx >= 0) nppMsg(NPPM_KEX_ACTIVATEDOC, v, idx);
}

HWND Views::host(int v) const
{
    if (HWND h = reinterpret_cast<HWND>(nppMsg(NPPM_KEX_GETVIEWHOSTWINDOW, v))) return h;
    if (HWND s = sci(v)) return ::GetAncestor(s, GA_ROOT);
    return npp._nppHandle;
}

int Views::viewOf(HWND h) const
{
    if (!h) return -1;
    for (int v = 0; v < _count; ++v)
        if (sci(v) == h) return v;
    return -1;
}

std::vector<HWND> Views::hostWindows() const
{
    std::vector<HWND> out;
    for (int v = 0; v < _count; ++v)
    {
        HWND h = host(v);
        if (h && h != npp._nppHandle && std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
    }
    return out;
}

HWND Views::tabBar(int v) const
{
    return (v < 0 || v >= _count) ? nullptr : reinterpret_cast<HWND>(nppMsg(NPPM_KEX_GETVIEWTABBAR, v));
}

} // namespace kex
