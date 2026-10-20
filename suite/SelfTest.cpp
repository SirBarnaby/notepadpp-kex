// SelfTest.cpp - scripted self-test, run only with -pluginMessage="KexSuite_selftest=1".
//
// Timer-driven steps (like the feasibility spike). Every result is logged as PASS/FAIL/INFO to
// <testdir>\selftest-log.txt. "SHOT n name" lines ask the harness (test/run-selftest.sh) for a
// screenshot; the step machine waits for <testdir>\ackN before going on.
// Optional argument: KexSuite_testdir=C:\path (default: <plugin config dir>\KexSuite-selftest).
#include "Overlay.h"
#include "KexSuiteRes.h"
#include "KexScintilla.h"
#include "preference_rc.h"
#include "SciLexer.h"
#include <functional>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cmath>

namespace kex {

namespace {

FILE* g_tlog = nullptr;
std::wstring g_dir;
int g_pass = 0, g_fail = 0, g_shotN = 0, g_waitAck = 0;
std::vector<std::function<int()>> g_steps;
size_t g_idx = 0;
double g_nextAt = 0;
bool g_inStep = false, g_done = false;
std::function<bool()> g_until;
double g_untilDeadline = 0;
bool g_untilOk = false;
int g_globalModified = 0;

HWND g_testWnd = nullptr;        // helper window: "another program" hotkey owner, menu tracking
PROCESS_INFORMATION g_other{};    // Wine's notepad.exe, the "previously active window"
HWND g_otherWnd = nullptr;
PROCESS_INFORMATION g_second{};   // second notepad++.exe
const UINT WM_TEST_TRACKMENU = WM_APP + 501;
int g_origCaretWidth = 1;

void tlog(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 4095, fmt, ap); va_end(ap); buf[4095] = 0;
    if (g_tlog) { fwprintf(g_tlog, L"%ls\n", buf); fflush(g_tlog); }
}

void check(bool ok, const wchar_t* id, const wchar_t* fmt, ...)
{
    wchar_t buf[3000];
    va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 2999, fmt, ap); va_end(ap); buf[2999] = 0;
    if (ok) ++g_pass; else ++g_fail;
    tlog(L"%ls %ls %ls", ok ? L"PASS" : L"FAIL", id, buf);
}

int shot(const wchar_t* name)
{
    g_waitAck = ++g_shotN;
    tlog(L"SHOT %d %ls", g_shotN, name);
    return 0;
}

int until(std::function<bool()> cond, int timeoutMs)
{
    g_until = cond;
    g_untilDeadline = nowMs() + timeoutMs;
    g_untilOk = false;
    return 0;
}

void pump(int ms)
{
    double end = nowMs() + ms;
    while (nowMs() < end)
    {
        MSG m;
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
            // as Notepad++'s own loop: a dialog's keys go to the dialog, the rest through Notepad++'s shortcuts
            HWND active = ::GetActiveWindow();
            const bool dialog = active && active != npp._nppHandle && ::IsDialogMessageW(active, &m);
            if (!dialog && !nppMsg(NPPM_KEX_TRANSLATEACCEL, 0, reinterpret_cast<LPARAM>(&m)))
            {
                ::TranslateMessage(&m);
                ::DispatchMessageW(&m);
            }
        }
        ::Sleep(2);
    }
}
// waits inside a step, letting messages through (until() is for a step's end)
bool waitFor(std::function<bool()> cond, int timeoutMs)
{
    const double end = nowMs() + timeoutMs;
    while (!cond())
    {
        if (nowMs() >= end) return false;
        pump(15);
    }
    return true;
}

HWND curSci() { return views.sci(views.current()); }

void key(WORD vk, bool down)
{
    INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    switch (vk) { case VK_DELETE: case VK_INSERT: case VK_HOME: case VK_END: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                  case VK_PRIOR: case VK_NEXT: in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY; break; }
    ::SendInput(1, &in, sizeof(in));
}
void tap(WORD vk) { key(vk, true); key(vk, false); }
void chord(std::initializer_list<WORD> mods, WORD vk)
{
    for (WORD m : mods) key(m, true);
    tap(vk);
    for (auto it = mods.end(); it != mods.begin();) key(*--it, false);
}
void clickAt(int x, int y)
{
    ::SetCursorPos(x, y);
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ::SendInput(2, in, sizeof(INPUT));
}

void writeFile(const std::wstring& p, const std::string& text)
{
    FILE* f = _wfopen(p.c_str(), L"wb");
    if (f) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
}
bool fileExists(const std::wstring& p) { return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring path(const wchar_t* name) { return g_dir + L"\\" + name; }
void open(const wchar_t* name) { nppMsg(NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path(name).c_str())); }
void switchTo(const wchar_t* name) { nppMsg(NPPM_SWITCHTOFILE, 0, reinterpret_cast<LPARAM>(path(name).c_str())); }
std::wstring curName() { return fileNameOf(bufferPath(static_cast<intptr_t>(nppMsg(NPPM_GETCURRENTBUFFERID)))); }

void focusEditor()
{
    ::SetForegroundWindow(npp._nppHandle);
    HWND s = curSci();
    if (s) ::SetFocus(s);
}

intptr_t posOf(HWND s, int line, int col) { return sci(s, SCI_FINDCOLUMN, line, col); }

int visibleNonBlank(HWND s, intptr_t a, intptr_t b)
{
    // independent count of what should fall: visible, non-blank characters in [a, b)
    RECT rc; ::GetClientRect(s, &rc);
    int margins = 0;
    for (int i = 0; i < static_cast<int>(sci(s, SCI_GETMARGINS)); ++i) margins += static_cast<int>(sci(s, SCI_GETMARGINWIDTHN, i));
    int n = 0;
    for (intptr_t p = a; p < b; p = sci(s, SCI_POSITIONAFTER, p))
    {
        int c = static_cast<int>(sci(s, SCI_GETCHARAT, p)) & 0xFF;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        int x = static_cast<int>(sci(s, SCI_POINTXFROMPOSITION, 0, p)), y = static_cast<int>(sci(s, SCI_POINTYFROMPOSITION, 0, p));
        if (x >= margins - 2 && x < rc.right && y >= -40 && y < rc.bottom) ++n;
    }
    return n;
}

std::wstring rectStr(const RECT& r)
{
    wchar_t b[96]; swprintf(b, 96, L"(%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom); return b;
}

int g_caretStyle = -1;   // Notepad++'s caret style in view 1 when the caret tests start

struct EnumCtx { DWORD pid; HWND found; };
BOOL CALLBACK findTop(HWND h, LPARAM p)
{
    auto* c = reinterpret_cast<EnumCtx*>(p);
    DWORD pid = 0; ::GetWindowThreadProcessId(h, &pid);
    if (pid == c->pid && ::IsWindowVisible(h) && !::GetWindow(h, GW_OWNER)) { c->found = h; return FALSE; }
    return TRUE;
}

int countNppWindows()
{
    int n = 0; HWND h = nullptr;
    while ((h = ::FindWindowExW(nullptr, h, L"Notepad++", nullptr)) != nullptr) ++n;
    return n;
}

HWND findReplaceDlg()
{
    HWND h = nullptr;
    while ((h = ::FindWindowExW(nullptr, h, L"#32770", nullptr)) != nullptr)
    {
        DWORD pid = 0; ::GetWindowThreadProcessId(h, &pid);
        if (pid == ::GetCurrentProcessId() && ::GetDlgItem(h, 1609) && ::IsWindowVisible(h)) return h;   // IDREPLACEALL
    }
    return nullptr;
}

// ---- a Notepad++ for each desktop ------------------------------------------------------------------
const char* const kDeskNote = "desk note\r\nsecond line";
std::wstring g_deskNote;               // the untitled tab's name
intptr_t g_deskNoteBuf = 0;
int g_deskNoteView = -1, g_deskNoteIndex = -1;   // where the copy should have it
int g_deskTabs[4] = {}, g_deskActive[4] = {}, g_deskView = 0;
std::vector<std::wstring> g_deskNames[4];
intptr_t g_deskCaret = 0;
HWND g_secondWnd = nullptr;

std::wstring windowTitle(HWND w)
{
    wchar_t t[512] = {};
    if (w) ::GetWindowTextW(w, t, 511);
    return t;
}

// ---- the app's name and icon -----------------------------------------------------------------
std::wstring exeVersionString(const wchar_t* name)
{
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD unused = 0, size = ::GetFileVersionInfoSizeW(exe, &unused);
    std::vector<BYTE> buf(size);
    if (!size || !::GetFileVersionInfoW(exe, 0, size, buf.data())) return L"";
    wchar_t* value = nullptr; UINT len = 0;
    std::wstring key = std::wstring(L"\\StringFileInfo\\040904b0\\") + name;
    if (!::VerQueryValueW(buf.data(), key.c_str(), reinterpret_cast<void**>(&value), &len) || !value) return L"";
    return value;
}

// the colour of the class icon at 32 px, at (x, y), drawn on white
COLORREF iconPixel(HICON icon, int x, int y)
{
    HDC screen = ::GetDC(nullptr), dc = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, 32, 32);
    HGDIOBJ old = ::SelectObject(dc, bmp);
    RECT r{ 0, 0, 32, 32 };
    ::FillRect(dc, &r, static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
    ::DrawIconEx(dc, 0, 0, icon, 32, 32, 0, nullptr, DI_NORMAL);
    COLORREF c = ::GetPixel(dc, x, y);
    ::SelectObject(dc, old); ::DeleteObject(bmp); ::DeleteDC(dc); ::ReleaseDC(nullptr, screen);
    return c;
}

const int kAboutBitnessId = 1707;   // IDC_VERSION_BIT in Notepad++'s resource.h: the About box's bitness label
HWND g_about = nullptr;
BOOL CALLBACK findAbout(HWND w, LPARAM)
{
    if (::IsWindowVisible(w) && ::GetDlgItem(w, kAboutBitnessId)) { g_about = w; return FALSE; }
    return TRUE;
}

// The tabs a copy holds: every tab except empty untitled ones (Notepad++'s own sessions leave those out).
void recordDeskLayout()
{
    int active[4];
    for (int v = 0; v < views.count(); ++v) active[v] = views.currentIndex(v);
    const int focus = views.current();
    for (int v = 0; v < views.count(); ++v)
    {
        g_deskNames[v].clear();
        g_deskActive[v] = -1;
        for (int i = 0; views.visible(v) && i < views.nbFiles(v); ++i)
        {
            views.activate(v, i);
            const intptr_t id = views.bufferId(v, i);
            const std::wstring p = bufferPath(id);
            const bool untitled = p.find(L'\\') == std::wstring::npos;
            if (untitled && sci(views.sci(v), SCI_GETLENGTH) == 0) continue;
            if (i == active[v]) g_deskActive[v] = static_cast<int>(g_deskNames[v].size());
            if (id == g_deskNoteBuf) { g_deskNoteView = v; g_deskNoteIndex = static_cast<int>(g_deskNames[v].size()); }
            g_deskNames[v].push_back(fileNameOf(p));
        }
        g_deskTabs[v] = static_cast<int>(g_deskNames[v].size());
    }
    for (int v = 0; v < views.count(); ++v)
        if (views.visible(v) && active[v] >= 0) views.activate(v, active[v]);
    views.activate(focus, active[focus]);
    g_deskView = views.current();
    g_deskCaret = static_cast<intptr_t>(sci(curSci(), SCI_GETCURRENTPOS));
}

std::wstring deskLayoutText()
{
    std::wstring t;
    for (int v = 0; v < views.count(); ++v)
    {
        t += L" view" + std::to_wstring(v + 1) + L"[";
        for (size_t i = 0; i < g_deskNames[v].size(); ++i)
            t += (i ? L"," : L"") + std::wstring(static_cast<int>(i) == g_deskActive[v] ? L"*" : L"") + g_deskNames[v][i];
        t += L"]";
    }
    return t + L" focus view" + std::to_wstring(g_deskView + 1);
}

// The new window has the recorded tabs (plus `extraInMain` files opened from its command line).
bool sameDeskLayout(HWND w, int extraInMain, std::wstring* why)
{
    if (!w) { *why = L": no window"; return false; }
    bool ok = true;
    std::wstring got;
    for (int v = 0; v < views.count(); ++v)
    {
        const bool vis = ::SendMessageW(w, NPPM_KEX_ISVIEWVISIBLE, v, 0) != 0;
        const int n = vis ? static_cast<int>(::SendMessageW(w, NPPM_KEX_GETNBVIEWFILES, v, 0)) : 0;
        const int a = vis ? static_cast<int>(::SendMessageW(w, NPPM_KEX_GETVIEWCURRENTINDEX, v, 0)) : -1;
        const int want = g_deskTabs[v] + (v == 0 ? extraInMain : 0);
        got += L" view" + std::to_wstring(v + 1) + L":" + std::to_wstring(n) + L"/" + std::to_wstring(want) + L" active " + std::to_wstring(a + 1);
        if (n != want) ok = false;
        if (extraInMain == 0 && want > 0 && g_deskActive[v] >= 0 && a != g_deskActive[v]) ok = false;
    }
    const int focus = static_cast<int>(::SendMessageW(w, NPPM_KEX_GETCURRENTVIEW, 0, 0));
    if (extraInMain == 0 && focus != g_deskView) ok = false;
    got += L", focus view " + std::to_wstring(focus + 1);
    *why = L" (" + got + L")";
    return ok;
}

// %TEMP%\KEX-Notepad++\desktop-copy-<pid of the second process>
std::wstring deskCopyFolder()
{
    wchar_t tmp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tmp);
    std::wstring d = tmp;
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    return d + L"KEX-Notepad++\\desktop-copy-" + std::to_wstring(g_second.dwProcessId);
}

// fake desktop ids for the second notepad++.exe: its session file and backup folder are named after them
const wchar_t kDeskA[] = L"00000000-0000-0000-0000-0000000000a1";
const wchar_t kDeskB[] = L"00000000-0000-0000-0000-0000000000b2";
const char kDeskTyped[] = "typed on desktop A, then Notepad++ crashed";

// notepad++.exe [file]; KEX_SELFTEST_OTHER_DESKTOP tells it whether this window is on its desktop,
// KEX_SELFTEST_DESKTOP_ID which desktop it is on
void launchSecond(const wchar_t* otherDesktop, const std::wstring& file, const wchar_t* desktopId = kDeskA)
{
    wchar_t exe[MAX_PATH];
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + exe + L"\"";
    if (!file.empty()) cmd += L" \"" + file + L"\"";
    ::SetEnvironmentVariableW(L"KEX_SELFTEST_OTHER_DESKTOP", otherDesktop);
    ::SetEnvironmentVariableW(L"KEX_SELFTEST_DESKTOP_ID", desktopId);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    g_second = PROCESS_INFORMATION{};
    g_secondWnd = nullptr;
    BOOL ok = ::CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &g_second);
    ::SetEnvironmentVariableW(L"KEX_SELFTEST_OTHER_DESKTOP", nullptr);
    ::SetEnvironmentVariableW(L"KEX_SELFTEST_DESKTOP_ID", nullptr);
    tlog(L"INFO started notepad++.exe%ls%ls with KEX_SELFTEST_OTHER_DESKTOP=%ls (pid %lu): %d", file.empty() ? L"" : L" ",
         fileNameOf(file).c_str(), otherDesktop, g_second.dwProcessId, ok);
}

// the second process's Notepad++ window is up and shows the copied tabs (its title names the active file)
bool deskCopyReady(const wchar_t* activeName, int mainTabs)
{
    if (!g_second.hProcess || ::WaitForSingleObject(g_second.hProcess, 0) == WAIT_OBJECT_0) return true;   // gone: fail in the check
    HWND h = nullptr;
    g_secondWnd = nullptr;
    while ((h = ::FindWindowExW(nullptr, h, L"Notepad++", nullptr)) != nullptr)
    {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        if (pid == g_second.dwProcessId && ::IsWindowVisible(h)) g_secondWnd = h;
    }
    if (!g_secondWnd || windowTitle(g_secondWnd).find(activeName) == std::wstring::npos) return false;
    // ready once the copy is open (it opens in one go at startup)
    return static_cast<int>(::SendMessageW(g_secondWnd, NPPM_KEX_GETNBVIEWFILES, 0, 0)) >= mainTabs;
}

std::string asciiOf(const wchar_t* w)
{
    std::string a;
    for (; *w; ++w) a += static_cast<char>(*w);
    return a;
}

std::wstring deskSessionFile(const wchar_t* id) { return settings.dir() + L"\\session-desktop-" + id + L".xml"; }
std::wstring deskBackupDir(const wchar_t* id) { return settings.dir() + L"\\backup\\desktop-" + id; }

std::string readAll(const std::wstring& p)
{
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return text;
    char chunk[4096];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0; ) text.append(chunk, n);
    fclose(f);
    return text;
}

int filesIn(const std::wstring& dir)
{
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n; } while (::FindNextFileW(f, &fd));
    ::FindClose(f);
    return n;
}

void removeDeskSession(const wchar_t* id)
{
    ::DeleteFileW(deskSessionFile(id).c_str());
    const std::wstring dir = deskBackupDir(id);
    WIN32_FIND_DATAW fd;
    HANDLE f = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE)
    {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ::DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); } while (::FindNextFileW(f, &fd));
        ::FindClose(f);
    }
    ::RemoveDirectoryW(dir.c_str());
}

POINT g_hoverPoint{};   // a point on the first tab while F12 shows the tab bar
int g_slideDir = 0, g_slideBefore = 0, g_slideFrames = 0;   // the tab slide steps
TabMotionStats g_motion;    // the tab fold and save glint steps
int g_fadeSlides = 0;       // core slides counted before a closed tab's document fades
int g_swipeIdx = 0;                        // the tab the swipe steps start from
SwipeStats g_swipeBefore;
std::vector<intptr_t> g_swipeOrder;        // the tabs in Ctrl+Tab order before a swipe

// a two-finger swipe as Windows sends it: WM_MOUSEHWHEEL over the view (or its tab bar), in small steps.
// units > 0 move toward the tab on the right with Windows' default touchpad setting (the content follows
// the fingers, so the fingers moving right scroll left: a negative delta).
void swipeBy(HWND target, int units, int step = 40, int gapMs = 4)
{
    RECT r; ::GetWindowRect(target, &r);
    const POINT pt{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    for (int done = 0; done < std::abs(units);)
    {
        const int d = std::min(step, std::abs(units) - done);
        done += d;
        const short delta = static_cast<short>(units > 0 ? -d : d);
        ::SendMessageW(target, WM_MOUSEHWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), MAKELPARAM(pt.x, pt.y));
        pump(gapMs);
    }
}
// the same over the active tab of view 0's tab bar, where the pointer would be
std::wstring g_underTab;
void swipeOverTab(int units)
{
    HWND bar = views.tabBar(0);
    RECT r{};
    ::SendMessageW(bar, TCM_GETITEMRECT, ::SendMessageW(bar, TCM_GETCURSEL, 0, 0), reinterpret_cast<LPARAM>(&r));
    ::MapWindowPoints(bar, nullptr, reinterpret_cast<POINT*>(&r), 2);
    const POINT pt{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    wchar_t cls[64] = {};
    ::GetClassNameW(kexsuite::windowFromPoint(pt), cls, 63);
    g_underTab = cls;
    for (int done = 0; done < std::abs(units); done += 40)
    {
        const short delta = static_cast<short>(units > 0 ? -40 : 40);
        ::SendMessageW(bar, WM_MOUSEHWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), MAKELPARAM(pt.x, pt.y));
        pump(4);
    }
}
// the buffers of every view, most recent first (Ctrl+Tab order)
std::vector<intptr_t> recentOrder()
{
    std::vector<std::pair<long, intptr_t>> all;
    for (int v = 0; v < views.count(); ++v)
        for (int i = 0, n = views.nbFiles(v); i < n; ++i)
        {
            const intptr_t b = views.bufferId(v, i);
            all.push_back({ static_cast<long>(nppMsg(NPPM_KEX_RECENTTAG, static_cast<WPARAM>(b), 0)), b });
        }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<intptr_t> out;
    for (const auto& a : all) out.push_back(a.second);
    return out;
}
std::string g_flashText;   // a.cpp before the flash steps edit it
bool g_lettersWereOn = true;   // the cut ghost step turns the falling letters off for a moment
bool g_flashModified = false;

// the text of the second window's tab that holds `want` ("" when none does)
std::wstring findTextInSecond(HWND w, const std::wstring& want)
{
    for (int v = 0; v < 4; ++v)
    {
        const int n = static_cast<int>(::SendMessageW(w, NPPM_KEX_GETNBVIEWFILES, v, 0));
        for (int i = 0; i < n; ++i)
        {
            if (!::SendMessageW(w, NPPM_KEX_ACTIVATEDOC, v, i)) continue;
            HWND s = reinterpret_cast<HWND>(::SendMessageW(w, NPPM_KEX_GETVIEWSCINTILLA, v, 0));
            wchar_t t[512] = {};
            ::SendMessageW(s, WM_GETTEXT, 511, reinterpret_cast<LPARAM>(t));
            if (std::wstring(t).find(want) != std::wstring::npos) return t;
        }
    }
    return L"";
}

void closeSecond(bool kill)
{
    if (!g_second.hProcess) return;
    if (kill && ::WaitForSingleObject(g_second.hProcess, 0) != WAIT_OBJECT_0)
    {
        ::TerminateProcess(g_second.hProcess, 0);
        ::WaitForSingleObject(g_second.hProcess, 5000);
    }
    ::CloseHandle(g_second.hProcess);
    ::CloseHandle(g_second.hThread);
    g_second = PROCESS_INFORMATION{};
    g_secondWnd = nullptr;
}

// ---- speed --------------------------------------------------------------------------------------
struct Perf { double keyMs = 0; double idleCpu = 0; double caretRedraws = 0; double idleRedraws = 0; double paints = 0;
             double caretWork = 0; };

int g_sciPaints = 0;
LRESULT CALLBACK countPaints(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (m == WM_PAINT) ++g_sciPaints;
    return ::DefSubclassProc(h, m, w, l);
}

double processCpuMs()
{
    FILETIME c, e, k, u;
    ::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u);
    auto ms = [](const FILETIME& f) { return ((static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0; };
    return ms(k) + ms(u);
}

void drainMessages()
{
    MSG m;
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        if (!nppMsg(NPPM_KEX_TRANSLATEACCEL, 0, reinterpret_cast<LPARAM>(&m))) { ::TranslateMessage(&m); ::DispatchMessageW(&m); }
}

// Typing: keys posted the way the keyboard delivers them (through the message loop and its hooks),
// each one painted before the next. Idle: waiting for messages for two seconds, as Notepad++ does.
Perf measurePerf()
{
    Perf r;
    HWND s = curSci();
    if (!s) return r;
    sci(s, SCI_GOTOLINE, 1500);
    sci(s, SCI_LINEEND);
    drainMessages();
    const int keys = 200;
    sci(s, SCI_BEGINUNDOACTION);
    const int p0 = caretStats().pushes;
    const CaretStats cs0 = caretStats();
    ::SetWindowSubclass(s, countPaints, 0x4B455050, 0);
    g_sciPaints = 0;
    const double t0 = nowMs();
    for (int i = 0; i < keys; ++i)
    {
        ::PostMessageW(s, WM_CHAR, static_cast<WPARAM>(i % 20 == 19 ? ' ' : 'a' + i % 26), 0);
        drainMessages();
        ::UpdateWindow(s);
    }
    r.keyMs = (nowMs() - t0) / keys;
    r.caretRedraws = static_cast<double>(caretStats().pushes - p0) / keys;
    r.paints = static_cast<double>(g_sciPaints) / keys;
    const CaretStats cs1 = caretStats();
    r.caretWork = (cs1.workMs - cs0.workMs) / keys;
    ::RemoveWindowSubclass(s, countPaints, 0x4B455050);
    sci(s, SCI_ENDUNDOACTION);
    sci(s, SCI_UNDO);
    drainMessages();
    ::UpdateWindow(s);
    const int i0 = caretStats().pushes;
    const double c0 = processCpuMs(), w0 = nowMs();
    for (;;)
    {
        const double left = 2000 - (nowMs() - w0);
        if (left <= 0) break;
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left) + 1, QS_ALLINPUT);
        drainMessages();
    }
    r.idleCpu = 100.0 * (processCpuMs() - c0) / (nowMs() - w0);
    r.idleRedraws = 1000.0 * (caretStats().pushes - i0) / (nowMs() - w0);
    return r;
}

LRESULT CALLBACK testWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_TEST_TRACKMENU)
    {
        ::SetForegroundWindow(npp._nppHandle);
        ::TrackPopupMenuEx(reinterpret_cast<HMENU>(w), TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
            static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)), npp._nppHandle, nullptr);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

const char* kCpp =
    "// KEX Suite self-test: falling letters keep their syntax colours\r\n"
    "#include <cstdio>\r\n"
    "#include <string>\r\n"
    "\r\n"
    "struct Particle { float x, y, vx, vy; };\r\n"
    "\r\n"
    "static int gravity = 2200;   /* px per second squared */\r\n"
    "\r\n"
    "int main(int argc, char** argv)\r\n"
    "{\r\n"
    "    std::string message = \"Deleted text falls off the screen\";\r\n"
    "    for (int i = 0; i < 10; ++i)\r\n"
    "        printf(\"%d %s\\n\", i * gravity, message.c_str());\r\n"
    "    return argc > 1 ? 0 : 42;\r\n"
    "}\r\n"
    "\r\n"
    "class SmoothCaret { public: double glideMs = 80.0; bool fade = true; };\r\n"
    "\r\n"
    "// line 19 ------------------------\r\n"
    "// line 20 ------------------------\r\n"
    "// line 21 ------------------------\r\n"
    "// line 22 ------------------------\r\n"
    "// line 23 ------------------------\r\n"
    "// line 24\r\n"
    "// line 25: B keeps its caret here\r\n"
    "// line 26\r\n"
    "// line 27\r\n"
    "// line 28\r\n"
    "// line 29\r\n";

std::string bigText()
{
    std::string s;
    for (int i = 0; i < 3000; ++i)
    {
        char b[160];
        snprintf(b, sizeof b, "%04d The quick brown fox jumps over the lazy dog; KEX Suite huge delete line.\r\n", i);
        s += b;
    }
    return s;
}

int pressHotkey()
{
    // Real key events through SendInput; if this Windows/Wine does not turn them into WM_HOTKEY
    // for us, drive the same handler directly. Returns 1 when WM_HOTKEY was delivered.
    int presses = hotkeyStats().presses;
    chord({ VK_CONTROL, VK_MENU }, 'M');
    pump(450);
    if (hotkeyStats().presses > presses) return 1;
    hotkeyPress();
    pump(400);
    return 0;
}

// The on/off tick of a feature on its Preferences page, clicked the way the user does it.
const int kEnableBox[M_COUNT] = { IDC_L_EN, IDC_C_EN, IDC_H_EN, IDC_T_EN, IDC_D_EN, IDC_S_EN, IDC_J_EN, IDC_G_EN, IDC_K_EN, IDC_E_EN, IDC_W_EN, IDC_P_EN, IDC_SW_EN,
                                  IDC_F_EN, IDC_GL_EN };
HWND enableBox(int module)
{
    if (!prefPage(pageOfModule(module))) { openPreferences(); pump(300); }
    HWND p = prefPage(pageOfModule(module));
    return p ? ::GetDlgItem(p, kEnableBox[module]) : nullptr;
}
void clickEnable(int module)
{
    HWND b = enableBox(module);
    if (!b) return;
    ::SendMessageW(b, BM_SETCHECK, ::SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
    ::SendMessageW(::GetParent(b), WM_COMMAND, MAKEWPARAM(kEnableBox[module], BN_CLICKED), reinterpret_cast<LPARAM>(b));
}
bool enableTicked(int module) { HWND b = enableBox(module); return b && ::SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; }
HWND prefDialog() { HWND p = prefPage(0); return p ? ::GetParent(p) : nullptr; }
void showPrefPage(const wchar_t* name)
{
    HWND d = prefDialog();
    if (!d) return;
    HWND list = ::GetDlgItem(d, IDC_LIST_DLGTITLE);
    LRESULT i = ::SendMessageW(list, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name));
    ::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
    ::SendMessageW(d, WM_COMMAND, MAKEWPARAM(IDC_LIST_DLGTITLE, LBN_SELCHANGE), reinterpret_cast<LPARAM>(list));
}
std::wstring itemText(HWND page, int id) { wchar_t t[256] = {}; if (page) ::GetDlgItemTextW(page, id, t, 255); return t; }

std::string selText(HWND s)
{
    intptr_t a = sci(s, SCI_GETSELECTIONSTART), b = sci(s, SCI_GETSELECTIONEND);
    if (b <= a) return std::string();
    const char* p = reinterpret_cast<const char*>(sci(s, SCI_GETRANGEPOINTER, a, b - a));
    return p ? std::string(p, static_cast<size_t>(b - a)) : std::string();
}

// what picking an item in a drop-down list does
void pickCombo(HWND page, int id, int index)
{
    HWND cb = ::GetDlgItem(page, id);
    ::SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
    ::SendMessageW(page, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(cb));
}
std::string lineText(HWND s, intptr_t line)
{
    intptr_t a = sci(s, SCI_POSITIONFROMLINE, line), b = sci(s, SCI_GETLINEENDPOSITION, line);
    if (a < 0 || b <= a) return std::string();
    const char* p = reinterpret_cast<const char*>(sci(s, SCI_GETRANGEPOINTER, a, b - a));
    return p ? std::string(p, static_cast<size_t>(b - a)) : std::string();
}

// ---- real mouse input for tab drags --------------------------------------------------------------
void mouseTo(int x, int y)
{
    INPUT in{}; in.type = INPUT_MOUSE;
    const int sw = std::max(1, ::GetSystemMetrics(SM_CXSCREEN) - 1), sh = std::max(1, ::GetSystemMetrics(SM_CYSCREEN) - 1);
    in.mi.dx = ::MulDiv(x, 65535, sw); in.mi.dy = ::MulDiv(y, 65535, sh);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    ::SendInput(1, &in, sizeof(in));
}
void mouseButton(bool down)
{
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    ::SendInput(1, &in, sizeof(in));
}
RECT screenRectOf(HWND h) { RECT r{}; if (h) ::GetWindowRect(h, &r); return r; }
RECT viewRect(int v) { return screenRectOf(views.tabBar(v)); }   // the tab control spans the whole pane
bool rectNear(const RECT& a, const RECT& b, int tol)
{
    return std::abs(a.left - b.left) <= tol && std::abs(a.top - b.top) <= tol && std::abs(a.right - b.right) <= tol && std::abs(a.bottom - b.bottom) <= tol;
}
int viewOfFile(const std::wstring& name)
{
    for (int v = 0; v < views.count(); ++v)
        for (int i = 0; views.visible(v) && i < views.nbFiles(v); ++i)
            if (fileNameOf(bufferPath(views.bufferId(v, i))) == name) return v;
    return -1;
}
std::wstring g_dragFile;   // the file whose tab is being dragged
// press on the active tab of view v and pull it off the tab strip
void dragStart(int v)
{
    HWND tab = views.tabBar(v);
    int idx = views.currentIndex(v);
    g_dragFile = fileNameOf(bufferPath(views.bufferId(v, idx)));
    RECT r{};
    ::SendMessageW(tab, TCM_GETITEMRECT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&r));
    ::MapWindowPoints(tab, nullptr, reinterpret_cast<POINT*>(&r), 2);
    const int x = r.left + 14, y = (r.top + r.bottom) / 2;
    mouseTo(x, y); pump(80);
    POINT c{}; ::GetCursorPos(&c);
    wchar_t cls[64] = {}; ::GetClassNameW(::WindowFromPoint(c), cls, 63);
    mouseButton(true); pump(120);
    // a few small moves on the tab itself start the drag (Windows' tab control captures the mouse
    // on the press; under Wine the tab bar only takes it once it is dragging), then pull it away
    for (int i = 1; i <= 4; ++i) { mouseTo(x + 3 * i, y); pump(40); }
    for (int i = 1; i <= 4; ++i) { mouseTo(x + 12, y + 12 * i); pump(30); }
    wchar_t cap[64] = {}; if (::GetCapture()) ::GetClassNameW(::GetCapture(), cap, 63);
    tlog(L"INFO drag %ls from view %d: tab %d at (%d,%d), cursor (%ld,%ld) over %ls, capture %ls, current view %d",
        g_dragFile.c_str(), v + 1, idx, x, y, c.x, c.y, cls, cap, views.current() + 1);
}
void dragTo(int x, int y)
{
    POINT c{}; ::GetCursorPos(&c);
    for (int i = 1; i <= 6; ++i) { mouseTo(c.x + (x - c.x) * i / 6, c.y + (y - c.y) * i / 6); pump(30); }
    pump(300);   // the preview glides in about 120 ms
}
void dragEnd()
{
    mouseButton(false); pump(400);
    std::wstring where;
    for (int v = 0; v < views.count(); ++v)
    {
        wchar_t b[96]; swprintf(b, 96, L" view%d:%ls%d", v + 1, views.visible(v) ? L"" : L"hidden/", views.visible(v) ? views.nbFiles(v) : 0);
        where += b;
    }
    tlog(L"INFO after drop: %ls in view %d;%ls", g_dragFile.c_str(), viewOfFile(g_dragFile) + 1, where.c_str());
}

// ---- glass text area -----------------------------------------------------------------------------
COLORREF g_glassBack[4] = {};
int g_glassTech[4] = {}, g_glassQuality[4] = {};
const UINT WM_UPDATESCINTILLAS_ = WM_USER + 5000 + 1;   // WordStyleDlg.h: Style Configurator's "apply"
HWND catcherTitled(HWND host, const wchar_t* title)
{
    HWND h = nullptr;
    while ((h = ::FindWindowExW(nullptr, h, L"KexGlassCatcher", title)) != nullptr)
        if (reinterpret_cast<HWND>(::GetWindowLongPtrW(h, GWLP_USERDATA)) == host) return h;
    return nullptr;
}
HWND glassCatcherOf(HWND host) { return catcherTitled(host, L"KEX glass"); }
HWND glassBlurCatcherOf(HWND host) { return catcherTitled(host, L"KEX glass (moving)"); }   // acrylic: the plain blur that moves instead
int viewCount()
{
    int n = 0;
    for (int v = 0; v < views.count(); ++v) if (views.sci(v)) ++n;
    return n;
}
bool keyedWith(HWND w, COLORREF* key)
{
    if (!(::GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_LAYERED)) return false;
    COLORREF k = 0; BYTE a = 0; DWORD f = 0;
    if (!::GetLayeredWindowAttributes(w, &k, &a, &f)) return false;
    if (key) *key = k;
    return (f & LWA_COLORKEY) != 0;
}
int stylesWithBack(HWND s, COLORREF c)
{
    int n = 0;
    for (int st = 0; st < 256; ++st)
        if (st != STYLE_CALLTIP && static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, st)) == c) ++n;
    return n;
}
RECT clientOnScreen(HWND h)
{
    RECT r{}; ::GetClientRect(h, &r);
    ::MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&r), 2);
    return r;
}
HWND nextVisible(HWND h)
{
    HWND n = ::GetWindow(h, GW_HWNDNEXT);
    while (n && !::IsWindowVisible(n)) n = ::GetWindow(n, GW_HWNDNEXT);
    return n;
}
bool viewsKeyed(COLORREF key)
{
    for (int v = 0; v < views.count(); ++v)
        if (HWND s = views.sci(v))
            if (static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT)) != key) return false;
    return true;
}
// a screen point on the glass: well right of the end of a short line of view 0
POINT glassPoint(int line)
{
    HWND s = views.sci(0);
    intptr_t end = sci(s, SCI_GETLINEENDPOSITION, line);
    POINT pt{ static_cast<int>(sci(s, SCI_POINTXFROMPOSITION, 0, end)) + 80,
              static_cast<int>(sci(s, SCI_POINTYFROMPOSITION, 0, end)) + static_cast<int>(sci(s, SCI_TEXTHEIGHT, line)) / 2 };
    ::ClientToScreen(s, &pt);
    return pt;
}
LPARAM glassLParam(HWND catcher, POINT screenPt)
{
    ::ScreenToClient(catcher, &screenPt);
    return MAKELPARAM(screenPt.x, screenPt.y);
}
std::wstring classOf(HWND h) { wchar_t c[64] = {}; if (h) ::GetClassNameW(h, c, 63); return c; }

// ---- live token styles ------------------------------------------------------------------------------
const int kTokValue = 0x4B54;    // TokenStyles.cpp: the indicator value of its runs
const char* kTokCpp =
    "// token test\r\n"
    "int alpha = 1;\r\n"
    "int beta = alpha + 2; // alpha in a comment\r\n"
    "/* block alpha */\r\n"
    "void gamma() { return; }\r\n"
    "int alphabet = 3; // not a whole word\r\n";

std::string docText(HWND s)
{
    const char* p = reinterpret_cast<const char*>(sci(s, SCI_GETCHARACTERPOINTER));
    return p ? std::string(p, static_cast<size_t>(sci(s, SCI_GETLENGTH))) : std::string();
}
// whole-word occurrences, found independently of Scintilla
std::vector<intptr_t> wordsIn(HWND s, const std::string& w)
{
    std::vector<intptr_t> out;
    const std::string t = docText(s);
    auto wordc = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    for (size_t p = t.find(w); p != std::string::npos; p = t.find(w, p + 1))
        if ((p == 0 || !wordc(t[p - 1])) && (p + w.size() >= t.size() || !wordc(t[p + w.size()]))) out.push_back(static_cast<intptr_t>(p));
    return out;
}
bool tokOurs(HWND s, int ind, intptr_t a, intptr_t n)
{
    return sci(s, SCI_INDICATORVALUEAT, ind, a) == kTokValue && sci(s, SCI_INDICATOREND, ind, a) >= a + n;
}
int tokRuns(HWND s, int ind)
{
    int n = 0;
    const intptr_t len = sci(s, SCI_GETLENGTH);
    for (intptr_t p = 0; p < len;)
    {
        intptr_t e = sci(s, SCI_INDICATOREND, ind, p);
        if (e <= p) break;
        if (sci(s, SCI_INDICATORVALUEAT, ind, p) == kTokValue) ++n;
        p = e;
    }
    return n;
}
int allOurs(HWND s, int ind, const std::vector<intptr_t>& at, intptr_t n)
{
    int k = 0;
    for (intptr_t a : at) if (tokOurs(s, ind, a, n)) ++k;
    return k;
}
// SCI_GETSTYLEAT hands styles above 127 back negative (a signed char)
int styleAt(HWND s, intptr_t pos) { return static_cast<int>(sci(s, SCI_GETSTYLEAT, pos)) & 0xFF; }
// every character of the occurrences drawn with a bold/italic variant of `base` (-1: any base)
bool styledAs(HWND s, const std::vector<intptr_t>& at, intptr_t n, int flags, int base, std::wstring* why)
{
    for (intptr_t a : at)
        for (intptr_t i = a; i < a + n; ++i)
        {
            const int st = styleAt(s, i);
            int f = 0;
            const int b = tokenVariantBase(s, st, &f);
            const bool ok = flags ? (b >= 0 && f == flags && (base < 0 || b == base)) : b < 0;
            if (!ok)
            {
                if (why) { wchar_t w[160]; swprintf(w, 160, L"position %d: style %d (base %d, flags %d)", static_cast<int>(i), st, b, f); *why = w; }
                return false;
            }
        }
    return true;
}
// what a failed restore step saw
std::wstring tokDiag(HWND s)
{
    const std::vector<intptr_t> be = wordsIn(s, "beta"), ga = wordsIn(s, "gamma");
    int f = 0;
    const int gs = ga.empty() ? -1 : styleAt(s, ga[0]);
    const int gb = ga.empty() ? -1 : tokenVariantBase(s, gs, &f);
    wchar_t w[400];
    swprintf(w, 400, L"[%ls, %d rules, wrapped %d, beta x%d value %d, gamma x%d style %d base %d flags %d, lexed to %d of %d]", curName().c_str(), tokenRuleCount(s),
             tokenDocWrapped(s) ? 1 : 0, static_cast<int>(be.size()), be.empty() ? -1 : static_cast<int>(sci(s, SCI_INDICATORVALUEAT, 24, be[0])),
             static_cast<int>(ga.size()), gs, gb, f, static_cast<int>(sci(s, SCI_GETENDSTYLED)), static_cast<int>(sci(s, SCI_GETLENGTH)));
    return w;
}
void caretOn(HWND s, intptr_t pos) { sci(s, SCI_SETEMPTYSELECTION, pos + 1); }
void tokCommand(UINT id) { ::SendMessageW(npp._nppHandle, WM_COMMAND, id, 0); }
// let the views paint (that is when Scintilla lexes) and the posted work run
void settle(int ms = 250) { for (int v = 0; v < views.count(); ++v) if (HWND s = views.sci(v)) ::UpdateWindow(s); pump(ms); }
bool menuHasId(HMENU m, UINT id) { return m && ::GetMenuState(m, id, MF_BYCOMMAND) != static_cast<UINT>(-1); }
HMENU subWith(HMENU m, UINT id)
{
    if (!m) return nullptr;
    for (int i = 0, n = ::GetMenuItemCount(m); i < n; ++i)
        if (::GetMenuItemID(m, i) == id) return m;
    for (int i = 0, n = ::GetMenuItemCount(m); i < n; ++i)
        if (HMENU r = subWith(::GetSubMenu(m, i), id)) return r;
    return nullptr;
}
std::vector<int> g_tokBase;    // the lexer's styles of k.cpp before any bold/italic
std::vector<int> stylesOf(HWND s)
{
    std::vector<int> v;
    for (intptr_t i = 0, n = sci(s, SCI_GETLENGTH); i < n; ++i) v.push_back(styleAt(s, i));
    return v;
}

void buildSteps()
{
    auto add = [](std::function<int()> f) { g_steps.push_back(f); };

    // ---- setup ---------------------------------------------------------------------------
    add([] {
        writeFile(path(L"a.cpp"), kCpp);
        writeFile(path(L"b.py"),
            "# Python sample\r\nimport os\r\n\r\ndef falling(letters, gravity=9.81):\r\n    \"\"\"Return letters that fall.\"\"\"\r\n"
            "    return [c for c in letters if not c.isspace()]\r\n\r\nprint(falling('KEX Suite'))\r\n");
        writeFile(path(L"c.txt"), "c.txt - plain text\r\nsecond line of c\r\nthird line of c\r\n");
        writeFile(path(L"d.txt"), "d.txt - plain text\r\nanother file\r\n");
        writeFile(path(L"big.txt"), bigText());
        ::MoveWindow(npp._nppHandle, 20, 16, 960, 660, TRUE);
        tlog(L"INFO Notepad++ version word 0x%08lx; views: %ls, count %d; Wine: %ls",
            static_cast<long>(nppMsg(NPPM_GETNPPVERSION, 1, 0)), views.patched() ? L"KEX patched" : L"standard (NPPM_KEX_GETINFO returned 0)",
            views.count(), underWine ? L"yes" : L"no");
        if (views.patched())
            check(views.count() == 4, L"S1", L"KEX-patched Notepad++: NPPM_KEX_GETINFO reports %d views, using the four-view API", views.count());
        else
            check(views.count() == 2, L"S1", L"unpatched Notepad++: NPPM_KEX_GETINFO=0, using the two-view API");
        int ok = 0;
        for (int i = 0; i < M_COUNT; ++i)
        {
            tlog(L"INFO self-check %ls: %ls", modules[i]->title(), modules[i]->_checkMsg.c_str());
            if (modules[i]->_checkOk) ++ok;
        }
        check(ok == M_COUNT, L"S2", L"startup self-check: KEX Suite: %d/%d features OK (the desktop feature uses fake desktops in self-test mode)", ok, M_COUNT);
        DesktopProvider* real = createRealDesktopProvider();
        tlog(L"INFO real IVirtualDesktopManager here: %ls", real->ok() ? L"available" : real->error().c_str());
        check(true, L"S3", L"real virtual-desktop provider degrades without crashing (available=%d)", real->ok() ? 1 : 0);
        for (int i = 0; i < M_COUNT; ++i)
            check(modules[i]->isEnabled() == modules[i]->defaultOn(), L"S4", L"%ls %ls at startup, its default", modules[i]->title(), modules[i]->defaultOn() ? L"on" : L"off");
        // the microinteractions get their own steps (near the end); before them, tab switches and edits show at once
        setModuleEnabled(M_SLIDE, false, false);
        setModuleEnabled(M_FLASH, false, false);
        setModuleEnabled(M_SPLASH, false, false);
        open(L"d.txt"); open(L"c.txt"); open(L"b.py"); open(L"a.cpp");
        g_origCaretWidth = static_cast<int>(sci(views.sci(0), SCI_GETCARETWIDTH));
        return 600;
    });

    // ---- the KEX group in the Settings menu, and the Preferences pages ------------------------
    add([] {
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        HMENU sm = subWith(mm, IDM_SETTING_PREFERENCE);
        const int n = sm ? ::GetMenuItemCount(sm) : 0;
        static const wchar_t* const kNames[PAGE_COUNT] = { L"KEX Editing", L"KEX Tabs", L"KEX Windows", L"KEX Highlighting", L"KEX Quick Note", L"KEX Effects" };
        bool group = n > PAGE_COUNT && (::GetMenuState(sm, n - PAGE_COUNT - 1, MF_BYPOSITION) & MF_SEPARATOR);
        std::wstring names, items;
        for (int p = 0; p < PAGE_COUNT && group; ++p)
        {
            wchar_t t[64] = {};
            ::GetMenuStringW(sm, n - PAGE_COUNT + p, t, 63, MF_BYPOSITION);
            HMENU sub = ::GetSubMenu(sm, n - PAGE_COUNT + p);
            int switches = 0, onPage = 0;
            for (int i = 0; i < M_COUNT; ++i)
                if (pageOfModule(i) == p) { ++onPage; if (menuHasId(sub, settingsSwitchId(i))) ++switches; }
            const int last = sub ? ::GetMenuItemCount(sub) - 1 : -1;
            if (!sub || sub != settingsSubmenu(p) || wcscmp(t, kNames[p]) || switches != onPage
                || ::GetMenuItemID(sub, last) != settingsPreferencesId(p)) group = false;
            names += (p ? L", " : L"") + std::wstring(t);
            items += (p ? L" / " : L"") + std::to_wstring(switches) + L"+1";
        }
        check(group, L"M1", L"the Settings menu ends with the KEX group: %ls (feature switches + Preferences...: %ls)", names.c_str(), items.c_str());
        int import = -1;
        for (int i = 0; i < n; ++i)
            if (HMENU sub = ::GetSubMenu(sm, i)) if (menuHasId(sub, IDM_SETTING_IMPORTPLUGIN)) import = i;
        check(import == 4 && !menuHasId(mm, IDM_SETTING_KEX), L"M1b",
              L"Notepad++'s own Settings items keep their places (Import is item %d, as upstream) and the old KEX Features item is gone", import + 1);
        HMENU pm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPPLUGINMENU, 0));
        bool inPlugins = false;
        for (int i = 0, n = ::GetMenuItemCount(pm); i < n; ++i)
        {
            wchar_t p[128] = {}; ::GetMenuStringW(pm, i, p, 127, MF_BYPOSITION);
            if (wcsstr(p, L"KEX")) inPlugins = true;
        }
        check(!inPlugins, L"M2", L"nothing KEX left in the Plugins menu (the suite is built in)");
        focusEditor();
        ::PostMessageW(npp._nppHandle, WM_SYSCOMMAND, SC_KEYMENU, 't');   // open Settings from the menu bar
        return 500;
    });
    add([] { tap(VK_UP); tap(VK_UP); tap(VK_UP); tap(VK_RIGHT); return 500; });   // up to KEX Windows, and open it
    add([] { return shot(L"menu-settings-kex"); });
    add([] { ::EndMenu(); return 300; });
    add([] {
        // the switches show what runs; picking one switches the feature and stores it, as the Preferences tick does
        bool marks = true;
        for (int p = 0; p < PAGE_COUNT; ++p)
        {
            HMENU sub = settingsSubmenu(p);
            ::SendMessageW(npp._nppHandle, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), MAKELPARAM(0, FALSE));
            for (int i = 0; i < M_COUNT; ++i)
                if (pageOfModule(i) == p && ((::GetMenuState(sub, settingsSwitchId(i), MF_BYCOMMAND) & MF_CHECKED) != 0) != modules[i]->isEnabled())
                    marks = false;
        }
        check(marks, L"M3", L"each feature's switch in the KEX submenus is ticked exactly when the feature runs");
        const bool was = modules[M_CARET]->isEnabled();
        ::SendMessageW(npp._nppHandle, WM_COMMAND, settingsSwitchId(M_CARET), 0);
        const bool flipped = modules[M_CARET]->isEnabled() != was;
        const int stored = settings.getInt(modules[M_CARET]->key(), L"Enabled", -1);
        ::SendMessageW(npp._nppHandle, WM_COMMAND, settingsSwitchId(M_CARET), 0);
        const int back = settings.getInt(modules[M_CARET]->key(), L"Enabled", -1);
        check(flipped && stored == (was ? 0 : 1) && modules[M_CARET]->isEnabled() == was && back == (was ? 1 : 0), L"M4",
              L"Settings > KEX Editing > Smooth caret switches it %ls and back, and remembers it (stored %d, then %d)", was ? L"off" : L"on", stored, back);
        ::SendMessageW(npp._nppHandle, WM_COMMAND, settingsPreferencesId(PAGE_TABS), 0);
        pump(300);
        HWND d = prefDialog(), p = prefPage(PAGE_TABS);
        check(d && ::IsWindowVisible(d) && p && ::IsWindowVisible(p), L"M5", L"Settings > KEX Tabs > Preferences... opens Preferences on the \"KEX Tabs\" page");
        focusEditor();
        return 300;
    });
    // ---- the name "Notepad++ KEX" and the badged icon ----------------------------------------
    add([] {
        wchar_t cls[32] = {};
        ::GetClassNameW(npp._nppHandle, cls, 31);
        const std::wstring title = windowTitle(npp._nppHandle);
        check(title.find(L" - Notepad++ KEX") != std::wstring::npos && !wcscmp(cls, L"Notepad++"), L"B1",
              L"the title bar says \"%ls\"; the window class stays \"%ls\" for tools that look for Notepad++", title.c_str(), cls);
        const std::wstring desc = exeVersionString(L"FileDescription"), product = exeVersionString(L"ProductName");
        check(desc == L"Notepad++ KEX" && product == L"Notepad++ KEX", L"B2",
              L"notepad++.exe describes itself as \"%ls\" (product \"%ls\"), the name the taskbar and Task Manager show", desc.c_str(), product.c_str());
        // the badge: a violet-blue disc in the bottom-right corner, here outside its white sparkle
        HICON icon = reinterpret_cast<HICON>(::GetClassLongPtrW(npp._nppHandle, GCLP_HICON));
        COLORREF c = icon ? iconPixel(icon, 21, 27) : 0;
        const int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
        check(icon && b > 180 && b - g > 70 && r > g, L"B3", L"the app icon carries the KEX badge (32 px icon at 21,27: rgb %d,%d,%d)", r, g, b);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_ABOUT);
        return 600;
    });
    add([] {
        g_about = nullptr;
        ::EnumThreadWindows(::GetCurrentThreadId(), findAbout, 0);
        std::wstring text;
        RECT vr{}, br{};
        int width = 0;
        for (HWND c = g_about ? ::GetWindow(g_about, GW_CHILD) : nullptr; c; c = ::GetWindow(c, GW_HWNDNEXT))
        {
            wchar_t t[64] = {};
            ::GetWindowTextW(c, t, 63);
            if (wcsncmp(t, L"Notepad++", 9)) continue;
            text = t;
            ::GetWindowRect(c, &vr);
            HDC dc = ::GetDC(c);
            HGDIOBJ of = ::SelectObject(dc, reinterpret_cast<HFONT>(::SendMessageW(c, WM_GETFONT, 0, 0)));
            SIZE sz{}; ::GetTextExtentPoint32W(dc, t, static_cast<int>(wcslen(t)), &sz);
            ::SelectObject(dc, of); ::ReleaseDC(c, dc);
            width = sz.cx;
            break;
        }
        if (g_about) ::GetWindowRect(::GetDlgItem(g_about, kAboutBitnessId), &br);
        check(text.compare(0, 15, L"Notepad++ KEX v") == 0 && br.left >= vr.left + width, L"B4",
              L"the About box says \"%ls\", with the bitness after it (text ends at %ld, bitness starts at %ld)", text.c_str(), vr.left + width, br.left);
        return 300;
    });
    add([] { return shot(L"about-kex"); });
    add([] {
        if (g_about) ::SendMessageW(g_about, WM_COMMAND, IDCANCEL, 0);
        focusEditor();
        return 300;
    });
    add([] {
        ::SendMessageW(npp._nppHandle, WM_COMMAND, settingsPreferencesId(PAGE_EDITING), 0);
        pump(300);
        HWND p = prefPage(0), d = prefDialog();
        bool ticks = true;
        for (int i = 0; i < M_COUNT; ++i) if (enableTicked(i) != modules[i]->isEnabled()) ticks = false;
        check(d && ::IsWindowVisible(d) && p && ::IsWindowVisible(p) && !::IsWindowVisible(prefPage(PAGE_TABS)), L"P1",
              L"Settings > KEX Editing > Preferences... opens Preferences on the \"KEX Editing\" page");
        check(ticks, L"P2", L"the on/off ticks on both pages match the running features");
        tlog(L"INFO page summary: \"%ls\"", itemText(p, IDC_SUMMARY).c_str());
        return 300;
    });
    add([] { return shot(L"preferences-kex-editing"); });
    add([] {
        showPrefPage(L"KEX Quick Note");
        pump(200);
        HWND p = prefPage(PAGE_NOTE);
        std::wstring k = itemText(p, IDC_H_KEYSTATUS);
        check(p && ::IsWindowVisible(p) && !::IsWindowVisible(prefPage(PAGE_EDITING)), L"P3", L"\"KEX Quick Note\" page shows when picked in the list");
        check(k.find(L"works") != std::wstring::npos, L"P4", L"the page reports the quick note hotkey: \"%ls\"", k.c_str());
        return 300;
    });
    add([] { return shot(L"preferences-kex-quicknote"); });
    add([] {
        showPrefPage(L"KEX Tabs");
        pump(200);
        HWND p = prefPage(PAGE_TABS);
        check(p && ::IsWindowVisible(p) && !::IsWindowVisible(prefPage(PAGE_NOTE)) && ::IsDlgButtonChecked(p, IDC_S_EN) == BST_CHECKED,
            L"P3b", L"\"KEX Tabs\" page shows tab snapping (on) and F12 hover tabs: \"%ls\"", itemText(p, IDC_S_STATUS).c_str());
        return 300;
    });
    add([] { return shot(L"preferences-kex-tabs"); });
    add([] {
        // "another program" owns Ctrl+Alt+M: drop ours, take it with a foreign window, start ours again
        setModuleEnabled(M_HOTKEY, false, false);
        BOOL got = ::RegisterHotKey(g_testWnd, 77, MOD_CONTROL | MOD_ALT, 'M');
        tlog(L"INFO helper window took Ctrl+Alt+M: %d", got);
        setModuleEnabled(M_HOTKEY, true, false);
        HotkeyStats hs = hotkeyStats();
        check(!hs.registered && hs.lastError == ERROR_HOTKEY_ALREADY_REGISTERED, L"P5",
            L"taken hotkey detected: registered=%d error=%lu", hs.registered ? 1 : 0, hs.lastError);
        std::wstring k = itemText(prefPage(PAGE_NOTE), IDC_H_KEYSTATUS);
        check(k.find(L"taken") != std::wstring::npos, L"P6", L"the page shows: \"%ls\"", k.c_str());
        return 400;
    });
    add([] { return shot(L"preferences-hotkey-taken"); });
    add([] {
        ::UnregisterHotKey(g_testWnd, 77);
        clickEnable(M_HOTKEY);   // off
        clickEnable(M_HOTKEY);   // and on again
        std::wstring k = itemText(prefPage(PAGE_NOTE), IDC_H_KEYSTATUS);
        check(hotkeyStats().registered && k.find(L"works") != std::wstring::npos, L"P7",
            L"after the other program lets go, switching the quick note off and on takes the key back: \"%ls\"", k.c_str());
        HWND p1 = prefPage(PAGE_TABS);
        ::SetDlgItemInt(p1, IDC_T_DELAY, 420, FALSE);   // typed number: applied half a second later
        pump(800);
        check(settings.getInt(modules[M_TABS]->key(), L"HideDelayMs", 0) == 420 && modules[M_TABS]->isEnabled(), L"P8",
            L"a number typed on the page is saved and the feature restarted with it");
        ::SetDlgItemInt(p1, IDC_T_DELAY, 350, FALSE);
        pump(800);
        ::SendMessageW(prefDialog(), WM_COMMAND, IDCANCEL, 0);
        pump(200);
        check(!::IsWindowVisible(prefDialog()), L"P9", L"Preferences closes");
        focusEditor();
        return 300;
    });

    // ---- falling letters -----------------------------------------------------------------------
    add([] {
        switchTo(L"a.cpp");
        focusEditor();
        HWND s = curSci();
        sci(views.sci(0), SCI_SETCARETWIDTH, 2);   // a wider caret shows up better in screenshots
        sci(views.sci(1), SCI_SETCARETWIDTH, 2);
        intptr_t a = posOf(s, 10, 4), b = sci(s, SCI_GETLINEENDPOSITION, 10);
        sci(s, SCI_SETSEL, a, b);
        int expect = visibleNonBlank(s, a, b);
        tlog(L"INFO deleting the selection on line 11 (%lld chars, %d visible non-blank)", static_cast<long long>(b - a), expect);
        lettersFreezeAfter(230);
        int before = lettersStats().totalSpawned;
        tap(VK_DELETE);   // real keyboard input
        pump(150);
        LettersStats st = lettersStats();
        check(st.totalSpawned - before == expect && expect > 0, L"F1",
            L"Delete key on a selection: %d letters spawned, expected %d", st.totalSpawned - before, expect);
        return 500;
    });
    add([] {
        LettersStats st = lettersStats();
        RECT nr; ::GetWindowRect(npp._nppHandle, &nr);
        check(st.overlayVisible && st.live > 0, L"F2", L"overlay alive with %d letters, bounding box %ls (Notepad++ window %ls)",
            st.live, rectStr(st.overlay).c_str(), rectStr(nr).c_str());
        HWND ov = ::FindWindowW(L"KexSuiteOverlay", L"KEX effects");
        LONG_PTR ex = ov ? ::GetWindowLongPtrW(ov, GWL_EXSTYLE) : 0;
        check(ov && (ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED) && (ex & WS_EX_NOACTIVATE) && (ex & WS_EX_TOPMOST), L"F3",
            L"overlay is layered, click-through, non-activating, topmost (exstyle 0x%lx)", static_cast<long>(ex));
        check(::GetForegroundWindow() == npp._nppHandle && views.viewOf(::GetFocus()) >= 0, L"F4", L"focus stays in the editor while letters fall");
        return shot(L"falling-letters-selection");
    });
    add([] {
        lettersFreezeAfter(-1);
        return until([] { return lettersStats().live == 0 && !lettersStats().overlayVisible; }, 6000);
    });
    add([] {
        check(g_untilOk, L"F5", L"all letters fell off the screen and the overlay hid itself (frames rendered so far: %d)", lettersStats().frames);
        HWND s = curSci();
        sci(s, SCI_GOTOPOS, posOf(s, 6, 18));   // after "gravity" on line 7
        focusEditor();
        lettersFreezeAfter(160);
        int before = lettersStats().totalSpawned;
        tap(VK_BACK); tap(VK_BACK); tap(VK_BACK);
        pump(120);
        check(lettersStats().totalSpawned - before == 3, L"F6", L"Backspace x3: %d letters spawned", lettersStats().totalSpawned - before);
        return 300;
    });
    add([] { return shot(L"falling-letters-backspace"); });
    add([] {
        lettersFreezeAfter(-1);
        pump(50);
        HWND s = curSci();
        intptr_t a = posOf(s, 4, 7), b = posOf(s, 4, 15);   // "Particle"
        sci(s, SCI_SETSEL, a, b);
        int before = lettersStats().totalSpawned;
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_EDIT_CUT);
        pump(80);
        check(lettersStats().totalSpawned - before == 8, L"F7", L"Edit > Cut of \"Particle\": %d letters spawned", lettersStats().totalSpawned - before);
        int su = lettersStats().skippedUndo;
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("UNDO_ME"));
        before = lettersStats().totalSpawned;
        sci(s, SCI_UNDO);
        pump(50);
        check(lettersStats().totalSpawned == before && lettersStats().skippedUndo > su, L"F8",
            L"undo of typed text does not rain letters (skipped undo deletes: %d)", lettersStats().skippedUndo - su);
        sci(s, SCI_GOTOPOS, 0);
        return 200;
    });
    add([] {
        open(L"big.txt");
        HWND s = curSci();
        focusEditor();
        sci(s, SCI_SETFIRSTVISIBLELINE, 0);
        sci(s, SCI_SETSEL, posOf(s, 2500, 0), posOf(s, 2502, 0));
        sci(s, SCI_SETFIRSTVISIBLELINE, 0);
        int before = lettersStats().totalSpawned;
        sci(s, SCI_CLEAR);
        pump(50);
        check(lettersStats().totalSpawned == before && lettersStats().lastCandidates == 0, L"F9", L"deleting off-screen lines spawns nothing");
        sci(s, SCI_SELECTALL);
        sci(s, SCI_SETFIRSTVISIBLELINE, 100);
        lettersFreezeAfter(260);
        before = lettersStats().totalSpawned;
        double t0 = nowMs();
        tap(VK_DELETE);
        pump(200);
        LettersStats st = lettersStats();
        check(st.lastCandidates > 200 && st.lastSpawned > 0 && st.lastSpawned <= 200, L"F10",
            L"Select All + Delete of 3000 lines: %d visible letters grouped into %d falling pieces (cap 200); total time incl. 200 ms wait %.0f ms",
            st.lastCandidates, st.lastSpawned, nowMs() - t0);
        return 300;
    });
    add([] { return shot(L"falling-letters-huge-delete"); });
    add([] {
        lettersFreezeAfter(-1);
        HWND s = curSci();
        sci(s, SCI_SETSAVEPOINT);   // so closing big.txt does not ask
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(100);
        switchTo(L"b.py");
        focusEditor();
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_SEARCH_REPLACE);
        return 700;
    });
    add([] {
        // Replace All: Notepad++ switches SCN_MODIFIED off during it and reports NPPN_GLOBALMODIFIED
        HWND dlg = findReplaceDlg();
        if (!dlg) { check(false, L"F11", L"Replace dialog not found"); return 100; }
        ::SetWindowTextW(::GetDlgItem(dlg, 1601), L"letters");   // IDFINDWHAT
        ::SetWindowTextW(::GetDlgItem(dlg, 1602), L"glyphs");    // IDREPLACEWITH
        int before = lettersStats().totalSpawned, gm = g_globalModified;
        ::SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(1609, BN_CLICKED), reinterpret_cast<LPARAM>(::GetDlgItem(dlg, 1609)));
        pump(300);
        std::string txt(static_cast<size_t>(sci(curSci(), SCI_GETLENGTH)) + 1, '\0');
        sci(curSci(), SCI_GETTEXT, txt.size(), reinterpret_cast<LPARAM>(&txt[0]));
        bool replaced = txt.find("glyphs") != std::string::npos;
        check(replaced && lettersStats().totalSpawned == before, L"F11",
            L"Replace All (%ls) spawned %d letters; NPPN_GLOBALMODIFIED seen %d time(s)", replaced ? L"text replaced" : L"NOT replaced",
            lettersStats().totalSpawned - before, g_globalModified - gm);
        ::SendMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
        ::ShowWindow(dlg, SW_HIDE);
        return 300;
    });
    add([] {
        BOOL was = TRUE;
        ::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &was, 0);
        BOOL setOk = ::SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), 0);
        BOOL now = TRUE;
        ::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &now, 0);
        HWND s = curSci();
        focusEditor();
        sci(s, SCI_SETSEL, posOf(s, 0, 2), posOf(s, 0, 8));
        int before = lettersStats().totalSpawned, sk = lettersStats().skippedAnimOff;
        tap(VK_DELETE);
        pump(80);
        if (setOk && !now)
            check(lettersStats().totalSpawned == before && lettersStats().skippedAnimOff > sk, L"F12", L"Windows animation effects off: no letters (respected by default)");
        else
            tlog(L"SKIP F12 this system cannot switch SPI_SETCLIENTAREAANIMATION (set=%d, value now=%d)", setOk, now);
        ::SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(was)), 0);
        lettersFreezeAfter(-1);
        pump(100);
        switchTo(L"c.txt");
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_CLONE_TO_ANOTHER_VIEW);
        pump(200);
        HWND s2 = views.sci(1);
        check(views.current() == 1 && views.visible(1), L"F13a", L"c.txt cloned into the second view");
        ::SetFocus(s2);
        sci(s2, SCI_SETSEL, posOf(s2, 1, 0), posOf(s2, 1, 11));
        lettersFreezeAfter(200);
        before = lettersStats().totalSpawned;
        tap(VK_DELETE);
        pump(120);
        check(lettersStats().totalSpawned - before == 10, L"F13", L"second view: %d letters spawned from \"second line\"", lettersStats().totalSpawned - before);
        return 300;
    });
    add([] { return shot(L"falling-letters-second-view"); });
    add([] {
        lettersFreezeAfter(-1);
        return until([] { return lettersStats().live == 0; }, 6000);
    });

    // ---- smooth caret --------------------------------------------------------------------------
    add([] {
        bool ok = true;
        std::wstring detail;
        for (int v = 0; v < 2; ++v)
        {
            HWND s = views.sci(v);
            int cur = static_cast<int>(sci(s, SCI_GETCARETSTYLE));
            wchar_t b[96]; swprintf(b, 96, L"view%d on=%d style=0x%x; ", v + 1, smoothCaretOn(s) ? 1 : 0, cur);
            detail += b;
            // Scintilla draws the smooth caret itself: Notepad++'s caret setting is left as it is
            if (!smoothCaretOn(s) || (cur & CARETSTYLE_INS_MASK) == CARETSTYLE_INVISIBLE) ok = false;
        }
        g_caretStyle = static_cast<int>(sci(views.sci(0), SCI_GETCARETSTYLE));
        check(ok, L"C1", L"smooth caret on in Scintilla in both views, Notepad++'s caret style untouched (%ls)", detail.c_str());
        switchTo(L"a.cpp");
        focusEditor();
        ::SetFocus(views.sci(0));
        caretTestFreeze(-1, true);   // solid (no blink) so screenshots always catch it
        HWND s = views.sci(0);
        sci(s, SCI_GOTOPOS, posOf(s, 2, 0));
        pump(150);
        return 100;
    });
    add([] {
        HWND s = views.sci(0);
        CaretStats a = caretStats();
        intptr_t target = posOf(s, 8, 14);
        float tx = static_cast<float>(sci(s, SCI_POINTXFROMPOSITION, 0, target));
        float ty = static_cast<float>(sci(s, SCI_POINTYFROMPOSITION, 0, target));
        sci(s, SCI_GOTOPOS, target);
        std::wstring traj;
        double t0 = nowMs();
        int between = 0;
        while (nowMs() - t0 < 160)
        {
            pump(12);
            CaretStats c = caretStats();
            wchar_t b[64]; swprintf(b, 64, L"%.0fms:(%.1f,%.1f) ", nowMs() - t0, c.mainX, c.mainY);
            traj += b;
            if (c.mainY > a.mainY + 0.5f && c.mainY < ty - 0.5f) ++between;
        }
        CaretStats e = caretStats();
        tlog(L"INFO caret glide from (%.1f,%.1f) to (%.1f,%.1f): %ls", a.mainX, a.mainY, tx, ty, traj.c_str());
        check(between >= 2 && std::fabs(e.mainX - std::round(tx - 0.51f)) <= 1.0f && std::fabs(e.mainY - ty) < 0.5f, L"C2",
            L"caret glides through %d intermediate positions and lands on the target (%.1f,%.1f)", between, e.mainX, e.mainY);
        caretTestFreeze(0.45, true);
        sci(s, SCI_GOTOPOS, posOf(s, 2, 0));
        pump(120);
        return 200;
    });
    add([] { return shot(L"caret-glide-frozen-midway"); });
    add([] {
        caretTestFreeze(-1, true);
        HWND s = views.sci(0);
        sci(s, SCI_GOTOPOS, posOf(s, 10, 30));
        pump(200);
        CaretStats c = caretStats();
        check(c.visible && c.carets == 1, L"C3", L"smooth caret visible at (%.0f,%.0f), %.0f px high", c.mainX, c.mainY, c.mainH);
        return 100;
    });
    add([] { return shot(L"caret-normal"); });
    add([] {
        HWND s = views.sci(0);
        sci(s, SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE);
        sci(s, SCI_SETRECTANGULARSELECTIONANCHOR, posOf(s, 18, 3));
        sci(s, SCI_SETRECTANGULARSELECTIONCARET, posOf(s, 22, 8));
        pump(200);
        CaretStats c = caretStats();
        check(c.carets == 5, L"C4", L"rectangular selection over 5 lines: %d carets drawn", c.carets);
        return 100;
    });
    add([] { return shot(L"caret-rectangular-selection"); });
    add([] {
        HWND s = views.sci(0);
        sci(s, SCI_CANCEL);
        sci(s, SCI_SETSELECTIONMODE, SC_SEL_STREAM);
        sci(s, SCI_GOTOPOS, posOf(s, 6, 4));
        sci(s, SCI_SETOVERTYPE, 1);
        pump(200);
        CaretStats c = caretStats();
        check(c.block && c.carets == 1, L"C5", L"overtype mode: block caret");
        return 100;
    });
    add([] { return shot(L"caret-overtype-block"); });
    add([] {
        HWND s = views.sci(0);
        sci(s, SCI_SETOVERTYPE, 0);
        int z0 = static_cast<int>(sci(s, SCI_GETZOOM));
        sci(s, SCI_SETZOOM, z0 + 5);
        pump(200);
        CaretStats c = caretStats();
        float lh = static_cast<float>(sci(s, SCI_TEXTHEIGHT, 6));
        check(std::fabs(c.mainH - lh) < 0.5f, L"C6", L"zoom +5: caret height %.0f = line height %.0f", c.mainH, lh);
        sci(s, SCI_SETZOOM, z0);
        pump(150);
        sci(s, SCI_GOTOPOS, posOf(s, 12, 4));
        pump(150);
        CaretStats b = caretStats();
        sci(s, SCI_LINESCROLL, 0, 3);
        pump(30);
        CaretStats a = caretStats();
        float lh2 = static_cast<float>(sci(s, SCI_TEXTHEIGHT, 0));
        check(std::fabs((b.mainY - a.mainY) - 3 * lh2) < 0.5f, L"C7", L"scroll by 3 lines: caret moved %.0f px at once (3 lines = %.0f px)", b.mainY - a.mainY, 3 * lh2);
        sci(s, SCI_LINESCROLL, 0, -3);
        HWND other = ::FindWindowExW(npp._nppHandle, nullptr, L"ReBarWindow32", nullptr);
        ::SetFocus(other ? other : npp._nppHandle);
        pump(80);
        bool hid = !caretStats().visible;
        ::SetFocus(s);
        pump(150);
        check(hid && caretStats().visible, L"C8", L"caret hides on focus loss and comes back on focus");
        HWND s2 = views.sci(1);
        ::SetFocus(s2);
        pump(150);
        check(caretStats().target == s2 && caretStats().visible, L"C9", L"second view focused: caret drawn in view 2");
        ::SetFocus(s);
        pump(100);
        return 100;
    });
    add([] {
        // Notepad++'s caret setting (Preferences > Editing) changes the smooth caret too: a block caret
        HWND s = views.sci(0);
        const int orig = static_cast<int>(sci(s, SCI_GETCARETSTYLE));
        sci(s, SCI_SETCARETSTYLE, CARETSTYLE_BLOCK);
        sci(s, SCI_GOTOPOS, posOf(s, 3, 0));
        pump(150);
        const CaretStats b = caretStats();
        check(b.visible && b.block, L"C10", L"caret style set to block in Notepad++: the smooth caret is a block too (%.0f px high)", b.mainH);
        sci(s, SCI_SETCARETSTYLE, orig);   // the user's real value again
        sci(s, SCI_GOTOPOS, posOf(s, 3, 2));
        pump(150);
        clickEnable(M_CARET);   // its tick on the KEX Editing page
        const int r0 = static_cast<int>(sci(views.sci(0), SCI_GETCARETSTYLE));
        check(!modules[M_CARET]->isEnabled() && !smoothCaretOn(views.sci(0)) && !smoothCaretOn(views.sci(1)) && r0 == orig && orig == g_caretStyle, L"C11",
            L"caret off from Preferences: Scintilla's own caret again in both views, caret style untouched (0x%x)", r0);
        return 300;
    });
    add([] { return shot(L"caret-disabled-scintilla-caret-back"); });
    add([] {
        clickEnable(M_CARET);
        focusEditor();
        pump(100);
        check(modules[M_CARET]->isEnabled() && smoothCaretOn(views.sci(0)) && smoothCaretOn(views.sci(1)), L"C12",
            L"caret back on from Preferences");
        caretTestFreeze(-1, false);
        return 100;
    });

    add([] {
        // how the caret blinks (KEX Editing > Smooth caret > Blink): fades, on and off as Notepad++'s, or not at all
        HWND page = prefPage(PAGE_EDITING);
        HWND s = views.sci(0);
        const int period = static_cast<int>(sci(s, SCI_GETCARETPERIOD));
        const int was = settings.getInt(modules[M_CARET]->key(), L"Blink", 0);
        float lo[3] = {}, hi[3] = {};
        int mid[3] = {};
        for (int k = 2; k >= 0; --k)
        {
            pickCombo(page, IDC_C_BLINK, k);
            ::SetFocus(s);
            sci(s, SCI_GOTOPOS, posOf(s, 2, k + 1));
            lo[k] = 2; hi[k] = -1;
            const double end = nowMs() + 1600;
            while (nowMs() < end)
            {
                ::InvalidateRect(s, nullptr, FALSE);
                ::UpdateWindow(s);
                const CaretStats c = caretStats();
                if (c.visible)
                {
                    lo[k] = std::min(lo[k], c.alpha); hi[k] = std::max(hi[k], c.alpha);
                    if (c.alpha > 0.02f && c.alpha < 0.98f) ++mid[k];
                }
                pump(12);
            }
        }
        const int saved = settings.getInt(modules[M_CARET]->key(), L"Blink", -1);
        if (period > 0)
        {
            check(lo[2] == 1.0f && hi[2] == 1.0f, L"C13", L"Blink \"Never\": the caret stays fully on (%.2f..%.2f over 1.6 s)", lo[2], hi[2]);
            check(lo[1] == 0.0f && hi[1] == 1.0f && mid[1] == 0, L"C14", L"Blink \"On and off\": hard on and off as Notepad++'s caret (%d frames in between)", mid[1]);
            check(lo[0] == 0.0f && hi[0] == 1.0f && mid[0] > 0, L"C15", L"Blink \"Fade\" (the default): fades in and out (%d frames in between)", mid[0]);
        }
        else
            tlog(L"SKIP C13-C15 Notepad++'s caret doesn't blink here (period %d)", period);
        check(saved == 0 && ::SendDlgItemMessageW(page, IDC_C_BLINK, CB_GETCURSEL, 0, 0) == 0, L"C16", L"the choice is remembered (Blink=%d)", saved);
        if (was != 0) pickCombo(page, IDC_C_BLINK, was);
        return 100;
    });

    // ---- a Notepad++ for each desktop: a second notepad++.exe ---------------------------------------
    add([] {
        DesktopStats d = desktopStats();
        check(modules[M_DESKTOP]->isEnabled() && !d.newWindow, L"D1", L"per-desktop windows on; this is the first window");
        removeDeskSession(kDeskA);
        removeDeskSession(kDeskB);
        // what a copy must reproduce: a.cpp active with its caret, the other views, and an untitled tab with text
        switchTo(L"c.txt");
        if (views.current() == 1 && views.nbFiles(1) > 1) nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);   // the clone in view 2
        views.activate(0, 0);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
        sci(curSci(), SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(kDeskNote));
        sci(curSci(), SCI_GOTOPOS, 12);
        g_deskNote = curName();
        g_deskNoteBuf = static_cast<intptr_t>(nppMsg(NPPM_GETCURRENTBUFFERID));
        {
            // its styles go along: a colour on one occurrence, a coloured token made bold, one italic word
            HWND ns = curSci();
            sci(ns, SCI_SETSEL, 5, 9);     // "note"
            tokCommand(IDM_SEARCH_MARKONEEXT4);
            sci(ns, SCI_SETSEL, 18, 22);   // "line"
            tokCommand(IDM_SEARCH_MARKALLEXT1);
            tokCommand(static_cast<UINT>(tokenCommandId(0)));   // Bold
            sci(ns, SCI_SETSEL, 11, 17);   // "second"
            tokCommand(static_cast<UINT>(tokenCommandId(4)));   // Style One Token > Italic
            sci(ns, SCI_GOTOPOS, 12);
            settle(200);
        }
        switchTo(L"a.cpp");
        focusEditor();
        sci(curSci(), SCI_GOTOPOS, posOf(curSci(), 4, 2));
        pump(150);
        recordDeskLayout();
        tlog(L"INFO desktop copy expected:%ls", deskLayoutText().c_str());
        // a second notepad++.exe that finds this window on its own desktop hands it its file, as Notepad++ always did
        writeFile(path(L"d1.txt"), "handed over\r\n");
        launchSecond(L"0", path(L"d1.txt"));
        return until([] { return !g_second.hProcess || ::WaitForSingleObject(g_second.hProcess, 0) == WAIT_OBJECT_0; }, 20000);
    });
    add([] {
        DWORD code = 0;
        if (g_second.hProcess) ::GetExitCodeProcess(g_second.hProcess, &code);
        closeSecond(false);
        pump(300);
        check(g_untilOk && countNppWindows() == 1 && curName() == L"d1.txt", L"D2",
              L"notepad++.exe d1.txt with this window on the same desktop: it exited (code %lu) and this window opened d1.txt (%ls)", code, curName().c_str());
        if (curName() == L"d1.txt") nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        switchTo(L"a.cpp");
        pump(150);
        // the same, started on a desktop without Notepad++
        launchSecond(L"1", L"");
        return until([] { return deskCopyReady(L"a.cpp", g_deskTabs[0]); }, 30000);
    });
    add([] { return shot(L"desktop-new-window"); });
    add([] {
        HWND w = g_secondWnd;
        std::wstring why;
        const bool layout = sameDeskLayout(w, 0, &why);
        check(g_untilOk && w && ::IsWindowVisible(w) && countNppWindows() == 2 && ::IsWindow(npp._nppHandle), L"D3",
              L"notepad++.exe on a desktop without Notepad++: a new Notepad++ window (pid %lu) opened there, this one stayed", g_second.dwProcessId);
        check(layout, L"D4", L"the new window has the same tabs in every view, the same active tabs and the same active view%ls", why.c_str());
        HWND s = reinterpret_cast<HWND>(::SendMessageW(w, NPPM_KEX_GETVIEWSCINTILLA, g_deskView, 0));
        const intptr_t caret = s ? static_cast<intptr_t>(::SendMessageW(s, SCI_GETCURRENTPOS, 0, 0)) : -1;
        check(caret == g_deskCaret, L"D5", L"a.cpp's caret is where it was in the first window (%lld, expected %lld)",
              static_cast<long long>(caret), static_cast<long long>(g_deskCaret));
        // the untitled tab: a copy of its text, caret and name, unmodified so closing that window does not ask to save it
        std::wstring text;
        intptr_t noteCaret = -1;
        LRESULT modified = -1;
        if (g_deskNoteView >= 0 && ::SendMessageW(w, NPPM_KEX_ACTIVATEDOC, g_deskNoteView, g_deskNoteIndex))
        {
            HWND ns = reinterpret_cast<HWND>(::SendMessageW(w, NPPM_KEX_GETVIEWSCINTILLA, g_deskNoteView, 0));
            wchar_t t[256] = {};
            ::SendMessageW(ns, WM_GETTEXT, 255, reinterpret_cast<LPARAM>(t));
            text = t;
            noteCaret = static_cast<intptr_t>(::SendMessageW(ns, SCI_GETCURRENTPOS, 0, 0));
            modified = ::SendMessageW(ns, SCI_GETMODIFY, 0, 0);
        }
        if (g_deskNoteView >= 0)
            ::SendMessageW(reinterpret_cast<HWND>(::SendMessageW(w, NPPM_KEX_GETVIEWSCINTILLA, g_deskNoteView, 0)), SCI_COLOURISE, 0, -1);
        pump(300);
        {
            // the note's styles came along
            HWND ns = g_deskNoteView >= 0 ? reinterpret_cast<HWND>(::SendMessageW(w, NPPM_KEX_GETVIEWSCINTILLA, g_deskNoteView, 0)) : nullptr;
            auto styleOf = [ns](int pos) { return static_cast<int>(::SendMessageW(ns, SCI_GETSTYLEAT, pos, 0)) & 0xFF; };
            const LRESULT note = ns ? ::SendMessageW(ns, SCI_INDICATORVALUEAT, 22, 5) : -1;
            const LRESULT line = ns ? ::SendMessageW(ns, SCI_INDICATORVALUEAT, 25, 18) : -1;
            const LRESULT lineBold = ns ? ::SendMessageW(ns, SCI_STYLEGETWEIGHT, styleOf(19), 0) : -1;
            const LRESULT secondItalic = ns ? ::SendMessageW(ns, SCI_STYLEGETITALIC, styleOf(12), 0) : -1;
            const LRESULT deskPlain = ns ? (::SendMessageW(ns, SCI_STYLEGETITALIC, styleOf(1), 0) == 0 && ::SendMessageW(ns, SCI_STYLEGETWEIGHT, styleOf(1), 0) < SC_WEIGHT_BOLD) : 0;
            check(note == 1 && line == kTokValue && lineBold >= SC_WEIGHT_BOLD && secondItalic && deskPlain, L"D6b",
                  L"the untitled tab's styles came along: \"note\" coloured (%d), \"line\" a bold token (%d, weight %d), \"second\" italic (%d)",
                  static_cast<int>(note), static_cast<int>(line), static_cast<int>(lineBold), static_cast<int>(secondItalic));
        }
        const std::wstring title = windowTitle(w);
        check(text == L"desk note\r\nsecond line" && noteCaret == 12 && modified == 0 && title.find(g_deskNote) != std::wstring::npos, L"D6",
              L"untitled tab %ls copied: text \"%ls\", caret %lld, unmodified (%d), title \"%ls\"", g_deskNote.c_str(),
              text.substr(0, 9).c_str(), static_cast<long long>(noteCaret), static_cast<int>(modified), title.c_str());
        // text typed in that window is backed up into its own folder, as in the first window
        if (g_deskNoteView >= 0)
        {
            ::SendMessageW(w, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
            HWND ns = reinterpret_cast<HWND>(::SendMessageW(w, NPPM_KEX_GETVIEWSCINTILLA, g_deskNoteView, 0));
            const std::wstring typed(kDeskTyped, kDeskTyped + sizeof(kDeskTyped) - 1);
            for (wchar_t c : typed) ::SendMessageW(ns, WM_CHAR, c, 1);   // typed, as Scintilla's WM_SETTEXT takes 8-bit text
        }
        return until([] { return filesIn(deskBackupDir(kDeskA)) > 0
                                 && readAll(deskSessionFile(kDeskA)).find("desktop-" + asciiOf(kDeskA)) != std::string::npos; }, 25000);
    });
    add([] {
        const bool backedUp = g_untilOk;
        const int backups = filesIn(deskBackupDir(kDeskA));
        const std::wstring leftover = deskCopyFolder();
        closeSecond(true);   // as if it crashed: it isn't closed, its process ends
        pump(300);
        check(!fileExists(leftover) && countNppWindows() == 1 && curName() == L"a.cpp", L"D7",
              L"the copy's temporary folder was removed (%ls) and this window kept a.cpp active", leftover.c_str());
        check(backedUp, L"D7b", L"the new window backed up typed text into its own folder (%d file(s) in backup\\desktop-%ls) and saved its own session",
              backups, kDeskA);
        // a file opened on a desktop without Notepad++: the copied tabs, then that file, active
        writeFile(path(L"d2.txt"), "opened on desktop 2\r\n");
        launchSecond(L"1", path(L"d2.txt"), kDeskB);
        return until([] { return deskCopyReady(L"d2.txt", g_deskTabs[0] + 1); }, 30000);
    });
    add([] {
        HWND w = g_secondWnd;
        std::wstring why;
        const bool layout = sameDeskLayout(w, 1, &why);
        const int n = w ? static_cast<int>(::SendMessageW(w, NPPM_KEX_GETNBVIEWFILES, 0, 0)) : 0;
        const int active = w ? static_cast<int>(::SendMessageW(w, NPPM_KEX_GETVIEWCURRENTINDEX, 0, 0)) : -2;
        check(g_untilOk && layout && n == g_deskTabs[0] + 1 && active == n - 1 && windowTitle(w).find(L"d2.txt") != std::wstring::npos, L"D8",
              L"notepad++.exe d2.txt on a desktop without Notepad++: the copied tabs, then d2.txt last and active (%d of %d)%ls", active + 1, n, why.c_str());
        closeSecond(true);
        pump(300);
        // tidy up: the untitled tab goes without a question
        for (int v = 0; v < views.count(); ++v)
            for (int i = 0; views.visible(v) && i < views.nbFiles(v); ++i)
                if (views.bufferId(v, i) == g_deskNoteBuf)
                {
                    views.activate(v, i);
                    sci(curSci(), SCI_SETSAVEPOINT);
                    nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
                    v = views.count();
                    break;
                }
        switchTo(L"a.cpp");
        focusEditor();
        pump(150);
        return 100;
    });
    add([] {
        // Notepad++ started again on desktop A, whose window crashed: it comes back with that desktop's own
        // tabs, including the typed text from its backup, instead of a new copy of this window's tabs
        launchSecond(L"1", L"", kDeskA);
        return until([] {
            if (!g_second.hProcess || ::WaitForSingleObject(g_second.hProcess, 0) == WAIT_OBJECT_0) return true;
            HWND h = nullptr;
            g_secondWnd = nullptr;
            while ((h = ::FindWindowExW(nullptr, h, L"Notepad++", nullptr)) != nullptr)
            {
                DWORD pid = 0;
                ::GetWindowThreadProcessId(h, &pid);
                if (pid == g_second.dwProcessId && ::IsWindowVisible(h)) g_secondWnd = h;
            }
            return g_secondWnd && ::SendMessageW(g_secondWnd, NPPM_KEX_GETNBVIEWFILES, 0, 0) > 0;
        }, 30000);
    });
    add([] {
        HWND w = g_secondWnd;
        pump(500);
        const std::wstring typed(kDeskTyped, kDeskTyped + sizeof(kDeskTyped) - 1);
        const std::wstring found = (g_untilOk && w) ? findTextInSecond(w, typed) : L"";
        if (found.empty() && w)
            for (int v = 0; v < 4; ++v)
                for (int i = 0, n = static_cast<int>(::SendMessageW(w, NPPM_KEX_GETNBVIEWFILES, v, 0)); i < n; ++i)
                    if (::SendMessageW(w, NPPM_KEX_ACTIVATEDOC, v, i))
                    {
                        HWND s = reinterpret_cast<HWND>(::SendMessageW(w, NPPM_KEX_GETVIEWSCINTILLA, v, 0));
                        wchar_t t[64] = {};
                        ::SendMessageW(s, WM_GETTEXT, 63, reinterpret_cast<LPARAM>(t));
                        tlog(L"INFO   view %d tab %d: %ls | %ls", v + 1, i + 1, windowTitle(w).c_str(), t);
                    }
        check(!found.empty() && countNppWindows() == 2, L"D9",
              L"Notepad++ started again on that desktop: its own session came back with the backed-up text (\"%ls\")", found.substr(0, 26).c_str());
        const std::string own = readAll(settings.dir() + L"\\kex-desktop-" + kDeskA + L".log");
        check(own.find("own session") != std::string::npos, L"D10",
              L"that window logs to its own kex-desktop-<id>.log (%d bytes), so it doesn't overwrite kex.log", static_cast<int>(own.size()));
        closeSecond(true);
        pump(300);
        removeDeskSession(kDeskA);
        removeDeskSession(kDeskB);
        ::DeleteFileW((settings.dir() + L"\\kex-desktop-" + kDeskA + L".log").c_str());
        ::DeleteFileW((settings.dir() + L"\\kex-desktop-" + kDeskB + L".log").c_str());
        switchTo(L"a.cpp");
        focusEditor();
        return 100;
    });

    // ---- global hotkey ---------------------------------------------------------------------------
    add([] {
        check(hotkeyStats().registered, L"H1", L"global hotkey %ls registered", hotkeyStats().text.c_str());
        STARTUPINFOW si{}; si.cb = sizeof(si);
        wchar_t cmd[] = L"notepad.exe";
        BOOL ok = ::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &g_other);
        tlog(L"INFO started notepad.exe as 'another program': %d", ok);
        return until([] { EnumCtx c{ g_other.dwProcessId, nullptr }; ::EnumWindows(findTop, reinterpret_cast<LPARAM>(&c)); g_otherWnd = c.found; return g_otherWnd != nullptr; }, 15000);
    });
    add([] {
        if (g_otherWnd) ::MoveWindow(g_otherWnd, 340, 220, 620, 420, TRUE);
        ::DeleteFileW(quickNoteFile().c_str());
        ::ShowWindow(npp._nppHandle, SW_MINIMIZE);
        pump(200);
        if (g_otherWnd) ::SetForegroundWindow(g_otherWnd);
        ::SetCursorPos(420, 300);
        pump(250);
        tlog(L"INFO Notepad++ minimized=%d, foreground is the other program: %d", ::IsIconic(npp._nppHandle), ::GetForegroundWindow() == g_otherWnd);
        int delivered = pressHotkey();
        tlog(L"INFO SendInput Ctrl+Alt+M -> WM_HOTKEY delivered: %ls", delivered ? L"yes" : L"no (handler driven directly)");
        HWND q = quickNoteWindow();
        RECT r{}; if (q) ::GetWindowRect(q, &r);
        POINT pt{ 420, 300 };
        MONITORINFO mi{ sizeof(mi) }; ::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        int dx = (r.left + r.right) / 2 - (mi.rcWork.left + mi.rcWork.right) / 2, dy = (r.top + r.bottom) / 2 - (mi.rcWork.top + mi.rcWork.bottom) / 2;
        check(q && ::IsWindowVisible(q) && std::abs(dx) <= 2 && std::abs(dy) <= 2, L"Q1",
            L"hotkey from another program while Notepad++ is minimized: quick note %ls centred on the screen (off by %d,%d; WM_HOTKEY from real key events: %ls)",
            rectStr(r).c_str(), dx, dy, delivered ? L"yes" : L"no");
        check(q && ::GetForegroundWindow() == q && ::GetFocus() == quickNoteEditor() && ::IsIconic(npp._nppHandle), L"Q2",
            L"the note is in front with the typing focus; Notepad++ stays minimized");
        tap('N'); tap('O'); tap('T'); tap('E');
        return 300;
    });
    add([] { return shot(L"quick-note"); });
    add([] {
        HWND ed = quickNoteEditor();
        char buf[64] = {};
        if (ed) sci(ed, SCI_GETTEXT, sizeof(buf), reinterpret_cast<LPARAM>(buf));
        check(strcmp(buf, "note") == 0, L"Q3", L"typing goes into the note: \"%hs\"", buf);
        int delivered = pressHotkey();
        pump(200);
        std::string saved;
        if (FILE* f = _wfopen(quickNoteFile().c_str(), L"rb")) { char b[64] = {}; size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); saved.assign(b, n); }
        check(!quickNoteWindow() && !::FindWindowW(L"KexQuickNote", nullptr) && ::GetForegroundWindow() == g_otherWnd && saved == "note", L"Q4",
            L"hotkey again: note gone, focus back to the other program, text saved to %ls (\"%hs\"; WM_HOTKEY: %ls)",
            quickNoteFile().c_str(), saved.c_str(), delivered ? L"yes" : L"no");
        return 200;
    });
    add([] {
        pressHotkey();
        HWND ed = quickNoteEditor();
        char buf[64] = {};
        if (ed) sci(ed, SCI_GETTEXT, sizeof(buf), reinterpret_cast<LPARAM>(buf));
        check(ed && strcmp(buf, "note") == 0 && sci(ed, SCI_GETCURRENTPOS) == 4, L"Q5", L"reopened with the text kept, caret at the end: \"%hs\"", buf);
        tap(VK_ESCAPE);
        pump(300);
        check(!quickNoteWindow() && ::GetForegroundWindow() == g_otherWnd, L"Q6", L"Esc hides the note and gives the focus back");
        return 100;
    });
    add([] {
        pressHotkey();
        ::SetForegroundWindow(g_otherWnd);
        pump(250);
        HWND q = quickNoteWindow();
        bool lostFocus = ::GetForegroundWindow() == g_otherWnd;
        pressHotkey();
        pump(200);
        check(q && lostFocus && !quickNoteWindow() && !::FindWindowW(L"KexQuickNote", nullptr) && ::GetForegroundWindow() == g_otherWnd, L"Q7",
            L"clicked away, note still on screen: one press hides it and the other program keeps the focus");
        // "Always on top" off and the note fully covered: the press brings it back instead
        const wchar_t* k = modules[M_HOTKEY]->key();
        settings.setInt(k, L"OnTop", 0); modules[M_HOTKEY]->loadOptions();
        pressHotkey();
        q = quickNoteWindow();
        RECT nr{}; if (q) ::GetWindowRect(q, &nr);
        RECT orig{}; ::GetWindowRect(g_otherWnd, &orig);
        ::SetWindowPos(g_otherWnd, HWND_TOP, nr.left - 30, nr.top - 30, nr.right - nr.left + 60, nr.bottom - nr.top + 60, 0);
        ::SetForegroundWindow(g_otherWnd);
        pump(250);
        pressHotkey();
        bool broughtBack = q && quickNoteWindow() == q && ::GetForegroundWindow() == q;
        pressHotkey();
        pump(200);
        check(broughtBack && !quickNoteWindow(), L"Q8", L"covered by another window: a press brings it to front, the next hides it");
        settings.setInt(k, L"OnTop", 1); modules[M_HOTKEY]->loadOptions();
        ::MoveWindow(g_otherWnd, orig.left, orig.top, orig.right - orig.left, orig.bottom - orig.top, TRUE);
        ::ShowWindow(npp._nppHandle, SW_RESTORE);
        focusEditor();
        pump(200);
        return 200;
    });
    add([] {
        // the options: line numbers, no wrapping, current line, open where it was left
        const wchar_t* k = modules[M_HOTKEY]->key();
        settings.setInt(k, L"LineNumbers", 1); settings.setInt(k, L"WordWrap", 0); settings.setInt(k, L"CurrentLine", 1); settings.setInt(k, L"RememberPlace", 1);
        modules[M_HOTKEY]->loadOptions();
        pressHotkey();
        pump(200);
        HWND q = quickNoteWindow(), ed = quickNoteEditor();
        const LONG_PTR st = q ? ::GetWindowLongPtrW(q, GWL_STYLE) : 0;
        check(q && ed && !(st & WS_CAPTION) && sci(ed, SCI_GETMARGINWIDTHN, 0) > 0 && sci(ed, SCI_GETWRAPMODE) == SC_WRAP_NONE && sci(ed, SCI_GETCARETLINEVISIBLE),
              L"Q9", L"no title bar; line numbers %d px wide, no wrapping, current line shown", q && ed ? static_cast<int>(sci(ed, SCI_GETMARGINWIDTHN, 0)) : -1);
        RECT r{}; if (q) ::GetWindowRect(q, &r);
        auto hit = [&](int x, int y) { return static_cast<int>(::SendMessageW(q, WM_NCHITTEST, 0, MAKELPARAM(x, y))); };
        const int top = hit((r.left + r.right) / 2, r.top + 2), corner = hit(r.right - 2, r.bottom - 2), left = hit(r.left + 2, (r.top + r.bottom) / 2),
                  mid = hit((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        check(top == HTCAPTION && corner == HTBOTTOMRIGHT && left == HTLEFT && mid == HTCLIENT, L"Q10",
              L"its edge: top moves it (%d), corner and sides resize it (%d, %d), the middle is text (%d)", top, corner, left, mid);
        if (q) ::SetWindowPos(q, nullptr, 60, 50, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        pressHotkey();
        pump(250);
        pressHotkey();
        pump(200);
        RECT r2{}; if (quickNoteWindow()) ::GetWindowRect(quickNoteWindow(), &r2);
        check(quickNoteWindow() && r2.left == 60 && r2.top == 50, L"Q11", L"\"Open where I left it\": it comes back at %ls", rectStr(r2).c_str());
        pressHotkey();
        pump(200);
        settings.setInt(k, L"LineNumbers", 0); settings.setInt(k, L"WordWrap", 1); settings.setInt(k, L"CurrentLine", 0); settings.setInt(k, L"RememberPlace", 0);
        modules[M_HOTKEY]->loadOptions();
        ::ShowWindow(npp._nppHandle, SW_RESTORE);
        focusEditor();
        pump(200);
        return 200;
    });

    add([] {
        // a chosen file as the note's text (KEX Quick Note > Note file), with its own line endings
        const std::wstring f = path(L"chosen-note.txt");
        writeFile(f, "chosen text\nsecond line\n");
        settings.setStr(modules[M_HOTKEY]->key(), L"NoteFile", f);
        modules[M_HOTKEY]->loadOptions();
        pressHotkey();
        pump(300);
        HWND ed = quickNoteEditor();
        const std::string text = ed ? docText(ed) : std::string();
        check(ed && text == "chosen text\nsecond line\n" && sci(ed, SCI_GETEOLMODE) == SC_EOL_LF && quickNoteFile() == f && quickNoteFileError().empty(), L"Q12",
              L"\"Note file\" set: the note shows that file's text with its Unix line endings (\"%hs\")", text.substr(0, 11).c_str());
        check(ed && smoothCaretOn(ed), L"Q13", L"the note has the smooth caret, as the views do");
        // deleting there rains letters, as in the views
        lettersFreezeAfter(160);
        const int before = lettersStats().totalSpawned;
        if (ed) { ::SetFocus(ed); sci(ed, SCI_SETSEL, 0, 6); }   // "chosen"
        tap(VK_DELETE);
        pump(200);
        check(lettersStats().totalSpawned - before == 6, L"Q14", L"deleting \"chosen\" in the note: %d letters fall", lettersStats().totalSpawned - before);
        return shot(L"quick-note-letters");
    });
    add([] {
        lettersFreezeAfter(-1);
        tap('K');
        pump(100);
        pressHotkey();   // hides it, and saves
        pump(300);
        const std::string saved = readAll(path(L"chosen-note.txt"));
        check(!quickNoteWindow() && saved == "k text\nsecond line\n", L"Q15", L"hidden again: the chosen file now reads \"%hs\", line endings kept",
              saved.substr(0, 6).c_str());
        settings.setStr(modules[M_HOTKEY]->key(), L"NoteFile", L"");
        modules[M_HOTKEY]->loadOptions();
        check(quickNoteFile() == settings.dir() + L"\\kex-quicknote.txt", L"Q16", L"\"Note file\" emptied: back to the note's own file");
        ::ShowWindow(npp._nppHandle, SW_RESTORE);
        focusEditor();
        return until([] { return lettersStats().live == 0; }, 6000);
    });

    // ---- F12 hover tabs --------------------------------------------------------------------------
    add([] {
        if (!isPostItMode()) nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);
        pump(300);
        check(isPostItMode(), L"T0", L"in F12 post-it mode");
        switchTo(L"b.py");
        HWND s = curSci();
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("\r\n# unsaved edit"));
        switchTo(L"a.cpp");
        RECT r; ::GetWindowRect(npp._nppHandle, &r);
        RECT sr; ::GetWindowRect(curSci(), &sr);
        ::SetCursorPos((sr.left + sr.right) / 2, r.top + 2);
        return until([] { return tabsStats().stripVisible; }, 2000);
    });
    add([] {
        TabsStats t = tabsStats();
        int nb = views.nbFiles(views.current()), cur = views.currentIndex(views.current());
        int dirtyCount = 0; for (int d : t.dirty) dirtyCount += d;
        check(g_untilOk && t.tabCount == nb && t.activeIndex == cur, L"T1", L"hovering the top edge shows the tab bar %ls: %d tabs (view has %d), active tab %d",
            rectStr(t.strip).c_str(), t.tabCount, nb, t.activeIndex);
        check(dirtyCount >= 1, L"T2", L"unsaved marker shown on %d tab(s)", dirtyCount);
        RECT sr = t.strip; RECT sc; ::GetWindowRect(curSci(), &sc);
        check(sr.top <= sc.top + 2 && sr.bottom > sc.top, L"T3", L"tab bar over the text; the editor is not pushed down (editor top %ld, tab bar %ld..%ld)", sc.top, sr.top, sr.bottom);
        // Notepad++'s own tab bar, on top: the window under the first tab is that view's tab control
        HWND under = nullptr;
        if (!t.tabRects.empty())
        {
            const RECT r0 = t.tabRects[0];
            g_hoverPoint = POINT{ (r0.left + r0.right) / 2, (r0.top + r0.bottom) / 2 };
            under = ::WindowFromPoint(g_hoverPoint);
        }
        check(under && under == views.tabBar(views.current()), L"T3b", L"it is Notepad++'s real tab bar (window under the first tab %p, tab bar %p)",
              under, views.tabBar(views.current()));
        return 100;
    });
    add([] { return shot(L"f12-hover-tabs"); });
    add([] {
        TabsStats t = tabsStats();
        int target = -1;
        for (int i = 0; i < t.tabCount; ++i) if (i != t.activeIndex) { target = i; break; }
        if (target >= 0)
        {
            RECT r = t.tabRects[target];
            clickAt((r.left + r.right) / 2, (r.top + r.bottom) / 2);
            pump(300);
            bool viaMouse = views.currentIndex(views.current()) == target;
            if (!viaMouse) { tabsTestClick(target); pump(200); }
            check(views.currentIndex(views.current()) == target, L"T4", L"click on tab %d switched the document to %ls (%ls)",
                target, curName().c_str(), viaMouse ? L"real mouse click" : L"direct call");
        }
        ::SetCursorPos(500, 500);
        return until([] { return !tabsStats().stripVisible; }, 3000);
    });
    add([] {
        HWND under = ::WindowFromPoint(g_hoverPoint);
        check(g_untilOk && under == curSci(), L"T5", L"the tab bar hides when the mouse leaves, under the view again (window there %p, view %p)", under, curSci());
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);   // leave post-it
        pump(300);
        RECT r; ::GetWindowRect(npp._nppHandle, &r);
        ::SetCursorPos((r.left + r.right) / 2, r.top + 2);
        pump(400);
        check(!isPostItMode() && !tabsStats().stripVisible, L"T6", L"outside post-it mode the hot zone does nothing");
        ::SetCursorPos(600, 700);
        return 100;
    });

    // ---- microinteractions: tab slide, copy / cut / delete flashes, paint splash ----------------
    add([] {
        setModuleEnabled(M_SLIDE, true, false);
        setModuleEnabled(M_FLASH, true, false);
        setModuleEnabled(M_SPLASH, true, false);
        check(modules[M_SLIDE]->isEnabled() && modules[M_FLASH]->isEnabled() && modules[M_SPLASH]->isEnabled(), L"W0",
              L"tab slide, flashes and paint splash on");
        switchTo(L"b.py");
        switchTo(L"a.cpp");
        focusEditor();
        pump(300);
        // stop the slide half-way, so its frame can be checked and photographed
        slideTestFreeze(0.5);
        HWND s = curSci();
        const int from = views.currentIndex(views.current());
        const SlideStats before = slideStats(s);
        switchTo(L"b.py");
        const int to = views.currentIndex(views.current());
        g_slideDir = to > from ? 1 : -1;
        g_slideBefore = before.started;
        g_slideFrames = before.frames;
        settle(250);
        return 100;
    });
    add([] {
        HWND s = curSci();
        const SlideStats st = slideStats(s);
        check(curName() == L"b.py" && st.started == g_slideBefore + 1 && st.active && st.frames > g_slideFrames && std::fabs(st.fraction - 0.5f) < 0.01f
              && st.lastDir == g_slideDir, L"W1",
              L"switching tabs slides the new document in from the %ls (started %d, %d frames, held at %.2f; active %d, pictures %d, stopped early %d, technology %d)",
              g_slideDir > 0 ? L"right" : L"left", st.started - g_slideBefore, st.frames - g_slideFrames, st.fraction, st.active ? 1 : 0, st.captures, st.cancelled,
              static_cast<int>(sci(s, SCI_GETTECHNOLOGY)));
        return shot(L"tab-slide-half-way");
    });
    add([] {
        slideTestFreeze(-1);
        ::InvalidateRect(curSci(), nullptr, FALSE);
        return until([] { return !slideStats(curSci()).active; }, 3000);
    });
    add([] {
        const SlideStats st = slideStats(curSci());
        check(g_untilOk && std::fabs(st.fraction - 1.0f) < 0.001f, L"W2", L"the slide ends with the new document in place (last frame at %.2f)", st.fraction);
        // and back: the other way
        const int from = views.currentIndex(views.current());
        switchTo(L"a.cpp");
        const int to = views.currentIndex(views.current());
        settle(100);
        const SlideStats st2 = slideStats(curSci());
        check(st2.started == st.started + 1 && st2.lastDir == (to > from ? 1 : -1) && st2.lastDir == -g_slideDir, L"W3",
              L"switching back slides the other way (%ls)", st2.lastDir > 0 ? L"from the right" : L"from the left");
        // Windows' "Animation effects" off: no slide
        BOOL was = TRUE;
        ::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &was, 0);
        BOOL setOk = ::SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), 0);
        waitFor([] { return !slideStats(curSci()).active; }, 2000);
        const SlideStats st3 = slideStats(curSci());
        switchTo(L"b.py");
        const SlideStats st4 = slideStats(curSci());
        if (setOk)
            check(st4.started == st3.started && st4.skipped == st3.skipped + 1 && !st4.active, L"W4", L"Windows animation effects off: tabs switch at once (respected by default)");
        else
            tlog(L"SKIP W4 this system cannot switch SPI_SETCLIENTAREAANIMATION");
        ::SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(was)), 0);
        switchTo(L"a.cpp");
        focusEditor();
        return until([] { return !slideStats(curSci()).active; }, 2000);
    });
    // ---- tab close and open, save glint -------------------------------------------------------------
    add([] {
        check(modules[M_TABFOLD]->isEnabled() && modules[M_GLINT]->isEnabled(), L"TF0", L"tab fold and save glint on by default");
        writeFile(path(L"f1.txt"), "fold one\r\n");
        writeFile(path(L"f2.txt"), "fold two\r\n");
        writeFile(path(L"f3.txt"), "fold three\r\n");
        writeFile(path(L"sv.txt"), "save me\r\n");
        views.activate(0, 0);
        switchTo(L"a.cpp");
        focusEditor();
        pump(200);
        g_motion = tabMotionStats();
        tabMotionTestFreeze(0.5);
        open(L"f1.txt");
        return 200;
    });
    add([] {
        const TabMotionStats st = tabMotionStats();
        check(curName() == L"f1.txt" && st.unfolds == g_motion.unfolds + 1 && st.overlayShown && std::fabs(st.lastFraction - 0.5f) < 0.01f && st.lastDx > 0
              && st.foldFrames > g_motion.foldFrames, L"TF1",
              L"opening f1.txt unfolds its tab: the tabs on its right make room (%d px, held at %.2f, skipped %d)", st.lastDx, st.lastFraction,
              st.foldSkipped - g_motion.foldSkipped);
        return shot(L"tab-unfold-half-way");
    });
    add([] {
        tabMotionTestFreeze(-1);
        slideTestFreeze(-1);
        return until([] { return !tabMotionStats().overlayShown && !slideStats(curSci()).active; }, 3000);
    });
    add([] {
        check(g_untilOk, L"TF2", L"the unfold ends and its window goes: the real tab bar shows again");
        // closing it: the tab folds shut, its document fades out over the one that takes its place
        g_motion = tabMotionStats();
        g_fadeSlides = slideStats(curSci()).slides;
        tabMotionTestFreeze(0.5);
        slideTestFreeze(0.5);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        return 200;
    });
    add([] {
        const TabMotionStats st = tabMotionStats();
        const SlideStats sl = slideStats(curSci());
        check(curName() != L"f1.txt" && st.folds == g_motion.folds + 1 && st.overlayShown && st.docFades == g_motion.docFades + 1
              && sl.active && sl.slides == g_fadeSlides + 1 && std::fabs(sl.fraction - 0.5f) < 0.01f, L"TF3",
              L"closing f1.txt folds its tab shut and fades its document out over %ls (fade held at %.2f)", curName().c_str(), sl.fraction);
        return shot(L"tab-close-fold");
    });
    add([] {
        tabMotionTestFreeze(-1);
        slideTestFreeze(-1);
        ::InvalidateRect(curSci(), nullptr, FALSE);
        return until([] { return !tabMotionStats().overlayShown && !slideStats(curSci()).active; }, 3000);
    });
    add([] {
        check(g_untilOk, L"TF4", L"the fold and the fade end: the bar and the document as they are");
        // several tabs in one go (a session, Close All): they just change. In view 2, which has room for them
        // (a bar with scroll arrows never folds)
        views.activate(1, 0);
        pump(150);
        g_motion = tabMotionStats();
        open(L"f2.txt");
        open(L"f3.txt");
        pump(300);
        const TabMotionStats st = tabMotionStats();
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(300);
        const TabMotionStats st2 = tabMotionStats();
        check(st.foldBulk == g_motion.foldBulk + 1 && st.unfolds == g_motion.unfolds && st2.foldBulk == st.foldBulk + 1 && st2.folds == st.folds
              && !st2.overlayShown, L"TF5", L"two tabs opened in one go, then two closed: no folding, they just change (in one go %d and %d, folds %d, skipped %d)",
              st.foldBulk - g_motion.foldBulk, st2.foldBulk - st.foldBulk, st.unfolds - g_motion.unfolds + st2.folds - st.folds, st2.foldSkipped - g_motion.foldSkipped);
        switchTo(L"a.cpp");
        return until([] { return !slideStats(curSci()).active; }, 2000);
    });
    add([] {
        // saving: a glint across the tab
        open(L"sv.txt");
        focusEditor();
        pump(150);
        HWND s = curSci();
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_ADDTEXT, 1, reinterpret_cast<LPARAM>("!"));
        g_motion = tabMotionStats();
        tabMotionTestFreeze(0.5);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_SAVE);
        return 200;
    });
    add([] {
        const TabMotionStats st = tabMotionStats();
        check(st.saves == g_motion.saves + 1 && st.glints == g_motion.glints + 1 && st.glintLines == g_motion.glintLines && st.overlayShown
              && st.glintFrames > g_motion.glintFrames && sci(curSci(), SCI_GETMODIFY) == 0, L"TF6", L"saving sv.txt sends a glint across its tab");
        return shot(L"save-glint-half-way");
    });
    add([] {
        tabMotionTestFreeze(-1);
        return until([] { return !tabMotionStats().overlayShown; }, 2000);
    });
    add([] {
        check(g_untilOk, L"TF7", L"the glint passes and its window goes");
        // the tab bar hidden: a line of light along the top of the view
        nppMsg(NPPM_HIDETABBAR, 0, TRUE);
        pump(200);
        HWND s = curSci();
        g_motion = tabMotionStats();
        sci(s, SCI_ADDTEXT, 1, reinterpret_cast<LPARAM>("!"));
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_SAVE);
        pump(100);
        const TabMotionStats st = tabMotionStats();
        check(st.glints == g_motion.glints + 1 && st.glintLines == g_motion.glintLines + 1, L"TF8", L"with the tab bar hidden the glint runs along the top of the view");
        nppMsg(NPPM_HIDETABBAR, 0, FALSE);
        waitFor([] { return !tabMotionStats().overlayShown; }, 2000);
        // their switches
        setModuleEnabled(M_GLINT, false, false);
        setModuleEnabled(M_TABFOLD, false, false);
        const TabMotionStats b2 = tabMotionStats();
        sci(s, SCI_ADDTEXT, 1, reinterpret_cast<LPARAM>("!"));
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_SAVE);
        open(L"f1.txt");
        pump(150);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(200);
        const TabMotionStats st2 = tabMotionStats();
        check(st2.glints == b2.glints && st2.unfolds == b2.unfolds && st2.folds == b2.folds && st2.docFades == b2.docFades && !st2.overlayShown, L"TF9",
              L"tab fold and save glint off: tabs and saves change at once");
        setModuleEnabled(M_GLINT, true, false);
        setModuleEnabled(M_TABFOLD, true, false);
        switchTo(L"sv.txt");
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        switchTo(L"a.cpp");
        focusEditor();
        return until([] { return !tabMotionStats().overlayShown && !slideStats(curSci()).active; }, 2000);
    });
    // ---- touchpad tab swipe ---------------------------------------------------------------------
    add([] {
        setModuleEnabled(M_SWIPE, true, false);
        check(modules[M_SWIPE]->isEnabled(), L"SW0", L"touchpad tab swipe on (touchpad reports %ls)",
              swipeStats().rawRegistered ? L"registered" : L"not available here");
        views.activate(0, 0);
        focusEditor();
        pump(300);
        g_swipeIdx = views.currentIndex(0);
        g_swipeBefore = swipeStats();
        g_swipeOrder = recentOrder();
        // two fingers down, half a swipe to the right
        swipeTestFingers(2);
        swipeBy(views.sci(0), 400);
        settle(150);
        return 100;
    });
    add([] {
        HWND s = views.sci(0);
        const SwipeStats st = swipeStats();
        const SlideStats sl = slideStats(s);
        check(st.holding && sl.active && std::fabs(sl.fraction - 0.5f) < 0.02f && views.currentIndex(0) == g_swipeIdx + 1 && st.lastDir == 1, L"SW1",
              L"half a swipe: the tab on the right is half way in (at %.2f, tab %d -> %d)", sl.fraction, g_swipeIdx, views.currentIndex(0));
        return shot(L"tab-swipe-half-way");
    });
    add([] {
        pump(450);   // the fingers rest on the pad: no wheel messages, the slide stays
        const SlideStats sl = slideStats(views.sci(0));
        check(swipeStats().holding && sl.active && std::fabs(sl.fraction - 0.5f) < 0.02f, L"SW2",
              L"fingers resting: the slide holds where they left it (%.2f after 450 ms)", sl.fraction);
        // back to 0.3, then let go: it slides back
        swipeBy(views.sci(0), -160);
        settle(60);
        const float back = slideStats(views.sci(0)).fraction;
        swipeTestFingers(0);
        check(std::fabs(back - 0.3f) < 0.02f, L"SW3a", L"the slide follows the fingers back (%.2f)", back);
        return until([] { return !swipeStats().active && !slideStats(views.sci(0)).active; }, 2000);
    });
    add([] {
        const SwipeStats st = swipeStats();
        const std::vector<intptr_t> order = recentOrder();
        check(g_untilOk && views.currentIndex(0) == g_swipeIdx && st.cancels == g_swipeBefore.cancels + 1 && st.commits == g_swipeBefore.commits,
              L"SW3", L"let go before half way: it slides back to the first tab (tab %d)", views.currentIndex(0));
        check(order == g_swipeOrder, L"SW3b", L"the Ctrl+Tab order is as it was before the swipe (%d tabs)", static_cast<int>(order.size()));
        // most of the way, then let go: the tab on the right stays
        swipeTestFingers(2);
        swipeBy(views.sci(0), 560);
        swipeTestFingers(0);
        return until([] { return !swipeStats().active && !slideStats(views.sci(0)).active; }, 2000);
    });
    add([] {
        const SwipeStats st = swipeStats();
        const SlideStats sl = slideStats(views.sci(0));
        check(g_untilOk && views.currentIndex(0) == g_swipeIdx + 1 && st.commits == g_swipeBefore.commits + 1 && std::fabs(sl.fraction - 1.0f) < 0.001f,
              L"SW4", L"let go past half way: the switch finishes (tab %d, last frame at %.2f)", views.currentIndex(0), sl.fraction);
        // a quick flick the other way, only a fifth of a swipe long: back to the tab on the left
        swipeTestFingers(2);
        swipeBy(views.sci(0), -160, 40, 6);
        swipeTestFingers(0);
        return until([] { return !swipeStats().active && !slideStats(views.sci(0)).active; }, 2000);
    });
    add([] {
        const SwipeStats st = swipeStats();
        check(g_untilOk && views.currentIndex(0) == g_swipeIdx && st.lastDir == -1 && st.commits == g_swipeBefore.commits + 2, L"SW5",
              L"a short quick flick to the left switches to the tab on the left (tab %d)", views.currentIndex(0));
        // the first tab: there is nothing on the left
        const SwipeStats before = st;
        swipeTestFingers(2);
        swipeBy(views.sci(0), -480);
        const bool moved = slideStats(views.sci(0)).active;
        swipeTestFingers(0);
        pump(200);
        check(!moved && views.currentIndex(0) == 0 && swipeStats().edges == before.edges + 1 && swipeStats().slides == before.slides, L"SW6",
              L"swiping left on the first tab does nothing");
        // no precision touchpad (an older driver): a pause is letting go
        swipeTestFingers(-1);
        pump(300);
        g_swipeBefore = swipeStats();
        swipeBy(views.sci(0), 560, 35);
        return until([] { return !swipeStats().active && !slideStats(views.sci(0)).active; }, 2000);
    });
    add([] {
        const SwipeStats st = swipeStats();
        check(g_untilOk && views.currentIndex(0) == 1 && st.commits == g_swipeBefore.commits + 1, L"SW7",
              L"without touchpad reports a pause in the swipe lets go (tab %d;%ls)", views.currentIndex(0), st.trace.c_str());
        // a mouse's tilt wheel: one notch, one tab
        pump(300);
        const SwipeStats before = swipeStats();
        HWND s = views.sci(0);
        RECT r; ::GetWindowRect(s, &r);
        ::SendMessageW(s, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2));
        pump(50);
        check(views.currentIndex(0) == 2 && swipeStats().notches == before.notches + 1, L"SW8", L"tilt wheel right: the next tab at once (tab %d)", views.currentIndex(0));
        pump(300);
        // a long line that scrolls sideways: the swipe scrolls it
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
        pump(150);
        s = views.sci(0);
        sci(s, SCI_SETWRAPMODE, SC_WRAP_NONE);
        std::string line(3000, 'x');
        sci(s, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(line.c_str()));
        sci(s, SCI_GOTOPOS, 0);
        sci(s, SCI_SETXOFFSET, 0);
        settle(100);
        g_swipeIdx = views.currentIndex(0);
        g_swipeBefore = swipeStats();
        swipeTestFingers(2);
        swipeBy(s, -240);   // toward the left tab, which scrolls the text to the right
        settle(60);
        return 100;
    });
    add([] {
        HWND s = views.sci(0);
        const SwipeStats st = swipeStats();
        const intptr_t x = sci(s, SCI_GETXOFFSET);
        check(x > 0 && !slideStats(s).active && views.currentIndex(0) == g_swipeIdx && st.textScrolls == g_swipeBefore.textScrolls + 1, L"SW9",
              L"where the line scrolls sideways the swipe scrolls it (x offset %d), no tab moves", static_cast<int>(x));
        swipeTestFingers(0);
        pump(300);
        // over the tab bar it switches anyway
        swipeTestFingers(2);
        swipeOverTab(-560);
        swipeTestFingers(0);
        return until([] { return !swipeStats().active && !slideStats(views.sci(0)).active; }, 2000);
    });
    add([] {
        check(g_untilOk && views.currentIndex(0) == g_swipeIdx - 1 && swipeStats().commits == g_swipeBefore.commits + 1, L"SW10",
              L"over the tab bar the swipe switches tabs even where the line could scroll (tab %d from %d, under the pointer: %ls;%ls)", views.currentIndex(0), g_swipeIdx, g_underTab.c_str(), swipeStats().trace.c_str());
        // the new tab goes again
        views.activate(0, g_swipeIdx);
        pump(100);
        HWND s = views.sci(0);
        sci(s, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(""));
        sci(s, SCI_EMPTYUNDOBUFFER);
        sci(s, SCI_SETSAVEPOINT);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(200);
        // off: a swipe does what Notepad++ does without KEX
        setModuleEnabled(M_SWIPE, false, false);
        views.activate(0, 0);
        pump(100);
        const SwipeStats before = swipeStats();
        swipeTestFingers(2);
        swipeBy(views.sci(0), 560);
        pump(300);
        check(views.currentIndex(0) == 0 && !slideStats(views.sci(0)).active && swipeStats().gestures == before.gestures, L"SW11",
              L"touchpad tab swipe off: swiping does not switch tabs");
        setModuleEnabled(M_SWIPE, true, false);
        swipeTestFingers(-1);
        switchTo(L"a.cpp");
        focusEditor();
        return 200;
    });
    add([] {
        // copy: the selection flashes once
        HWND s = curSci();
        focusEditor();
        g_flashText = docText(s);
        g_flashModified = sci(s, SCI_GETMODIFY) != 0;
        lettersFreezeAfter(40);   // hold the effects at their brightest
        sci(s, SCI_SETSEL, posOf(s, 1, 0), posOf(s, 1, 8));
        const FlashStats before = flashStats();
        tokCommand(IDM_EDIT_COPY);
        waitFor([before] { return flashStats().copies > before.copies; }, 1500);
        pump(120);
        const FlashStats st = flashStats();
        HWND ov = ::FindWindowW(L"KexSuiteOverlay", L"KEX effects");
        check(st.copies == before.copies + 1 && st.cuts == before.cuts && st.lastRects == 1 && st.spawned == before.spawned + 1 && st.live >= 1
              && ov && ::IsWindowVisible(ov), L"X1", L"Edit > Copy: the selection flashes (%d rectangle, %d live)", st.lastRects, st.live);
        // over three lines: a rectangle per line, as the selection is drawn
        sci(s, SCI_SETSEL, posOf(s, 0, 2), posOf(s, 2, 3));
        tokCommand(IDM_EDIT_COPY);
        waitFor([st] { return flashStats().copies > st.copies; }, 1500);
        check(flashStats().lastRects == 3, L"X2", L"copying three lines flashes %d rectangles", flashStats().lastRects);
        return 100;
    });
    add([] {
        // cut: the text lifts away, not also counted as a copy
        HWND s = curSci();
        lettersFreezeAfter(-1);
        waitFor([] { return flashStats().live == 0; }, 3000);
        sci(s, SCI_SETSEL, posOf(s, 2, 0), posOf(s, 2, 5));
        const FlashStats before = flashStats();
        tokCommand(IDM_EDIT_CUT);
        waitFor([before] { return flashStats().ghosts > before.ghosts; }, 1500);
        pump(250);   // the clipboard listener hears of the cut, so the next removal isn't taken for one
        const FlashStats st = flashStats();
        check(st.cuts == before.cuts + 1 && st.copies == before.copies && st.deletes == before.deletes && st.ghosts == before.ghosts + 1 && st.spawned == before.spawned,
              L"X3", L"Edit > Cut: the cut text lifts away, one piece (cuts %d, copies %d, flashes %d)", st.cuts - before.cuts, st.copies - before.copies,
              st.spawned - before.spawned);
        sci(s, SCI_UNDO);
        // delete: the text shrinks away
        focusEditor();
        sci(s, SCI_SETSEL, posOf(s, 2, 0), posOf(s, 2, 5));
        lettersFreezeAfter(60);
        tap(VK_DELETE);
        waitFor([st] { return flashStats().ghosts > st.ghosts; }, 1500);
        pump(100);
        const FlashStats st2 = flashStats();
        check(st2.deletes == st.deletes + 1 && st2.cuts == st.cuts && st2.lastRects == 1 && st2.ghosts == st.ghosts + 1 && st2.ghostsLive >= 1, L"X4",
              L"Delete on a selection: the text shrinks away (deletes %d, %d piece on screen)", st2.deletes - st.deletes, st2.ghostsLive);
        return shot(L"delete-shrink");
    });
    add([] {
        HWND s = curSci();
        sci(s, SCI_UNDO);
        lettersFreezeAfter(-1);
        // typing over a selection, or deleting single characters, flashes nothing
        focusEditor();
        const FlashStats before = flashStats();
        sci(s, SCI_SETSEL, posOf(s, 2, 0), posOf(s, 2, 5));
        tap('X');
        pump(100);
        sci(s, SCI_SETEMPTYSELECTION, posOf(s, 2, 1));
        tap(VK_DELETE);
        tap(VK_BACK);
        pump(100);
        const FlashStats st = flashStats();
        check(st.deletes == before.deletes && st.cuts == before.cuts && st.copies == before.copies, L"X5",
              L"typing over a selection and deleting single characters: no flash");
        // its own switch: copies stay quiet
        settings.setInt(modules[M_FLASH]->key(), L"Copy", 0);
        modules[M_FLASH]->loadOptions();
        sci(s, SCI_SETSEL, posOf(s, 1, 0), posOf(s, 1, 4));
        const FlashStats b2 = flashStats();
        tokCommand(IDM_EDIT_COPY);
        pump(200);
        check(flashStats().spawned == b2.spawned, L"X6", L"\"Copy\" unticked: copying flashes nothing");
        settings.setInt(modules[M_FLASH]->key(), L"Copy", 1);
        modules[M_FLASH]->loadOptions();
        // the falling letters off: the ghost carries the text itself, in its own fonts and colours
        const bool letters = modules[M_LETTERS]->isEnabled();
        setModuleEnabled(M_LETTERS, false, false);
        waitFor([] { return flashStats().live == 0; }, 3000);
        sci(s, SCI_SETSEL, posOf(s, 2, 0), posOf(s, 2, 5));
        const std::string all = docText(s);
        int expect = 0;
        std::string cutText;
        for (intptr_t i = posOf(s, 2, 0); i < posOf(s, 2, 5) && i < static_cast<intptr_t>(all.size()); ++i)
        {
            cutText += all[static_cast<size_t>(i)];
            if (static_cast<unsigned char>(all[static_cast<size_t>(i)]) > ' ') ++expect;
        }
        const FlashStats b3 = flashStats();
        lettersFreezeAfter(110);
        tokCommand(IDM_EDIT_CUT);
        waitFor([b3] { return flashStats().ghosts > b3.ghosts; }, 1500);
        pump(150);
        const FlashStats st3 = flashStats();
        check(expect > 0 && st3.ghosts == b3.ghosts + 1 && st3.ghostChars == expect && st3.ghostsLive >= 1, L"X7",
              L"falling letters off: the cut ghost carries the text (%d of the %d characters to draw in \"%hs\")", st3.ghostChars, expect, cutText.c_str());
        g_lettersWereOn = letters;
        return shot(L"cut-lift");
    });
    add([] {
        HWND s = curSci();
        if (g_lettersWereOn) setModuleEnabled(M_LETTERS, true, false);
        lettersFreezeAfter(-1);
        sci(s, SCI_UNDO);
        // their own switches
        settings.setInt(modules[M_FLASH]->key(), L"CutLift", 0);
        settings.setInt(modules[M_FLASH]->key(), L"DeleteFade", 0);
        modules[M_FLASH]->loadOptions();
        waitFor([] { return flashStats().live == 0; }, 3000);
        const FlashStats b4 = flashStats();
        sci(s, SCI_SETSEL, posOf(s, 2, 0), posOf(s, 2, 5));
        tokCommand(IDM_EDIT_CUT);
        pump(250);
        sci(s, SCI_UNDO);
        focusEditor();
        sci(s, SCI_SETSEL, posOf(s, 2, 0), posOf(s, 2, 5));
        tap(VK_DELETE);
        pump(200);
        const FlashStats st4 = flashStats();
        check(st4.cuts == b4.cuts + 1 && st4.deletes == b4.deletes + 1 && st4.ghosts == b4.ghosts, L"X8",
              L"\"Cut\" and \"Delete\" unticked: cutting and deleting show nothing");
        sci(s, SCI_UNDO);
        settings.setInt(modules[M_FLASH]->key(), L"CutLift", 1);
        settings.setInt(modules[M_FLASH]->key(), L"DeleteFade", 1);
        modules[M_FLASH]->loadOptions();
        // a.cpp as it was
        if (docText(s) != g_flashText)
        {
            sci(s, SCI_TARGETWHOLEDOCUMENT);
            sci(s, SCI_REPLACETARGET, static_cast<WPARAM>(g_flashText.size()), reinterpret_cast<LPARAM>(g_flashText.c_str()));
        }
        if (!g_flashModified) sci(s, SCI_SETSAVEPOINT);
        sci(s, SCI_SETEMPTYSELECTION, posOf(s, 0, 0));
        return until([] { return flashStats().live == 0; }, 3000);
    });
    add([] {
        // paint splash: Style All Occurrences of Token splashes every new occurrence
        waitFor([] { return lettersStats().live == 0; }, 6000);
        writeFile(path(L"s.txt"), "paint the town red\r\nred paint, red ink\r\nred\r\n");
        open(L"s.txt");
        focusEditor();
        HWND s = curSci();
        settle(200);
        const std::vector<intptr_t> red = wordsIn(s, "red");
        caretOn(s, red.empty() ? 0 : red[0]);
        const SplashStats before = splashStats();
        lettersFreezeAfter(60);
        tokCommand(IDM_SEARCH_MARKALLEXT2);
        pump(150);
        const SplashStats st = splashStats();
        check(red.size() == 4 && st.splashes == before.splashes + 1 && st.lastTokens == 4 && st.spawned == before.spawned + 4 * 8 && st.live > 0, L"P1",
              L"Style All Occurrences of Token: \"red\" splashes paint at its %d occurrences (%d drops and washes)", st.lastTokens, st.spawned - before.spawned);
        return shot(L"paint-splash");
    });
    add([] {
        HWND s = curSci();
        lettersFreezeAfter(-1);
        // the same again: nothing new is coloured, nothing splashes
        const SplashStats before = splashStats();
        const std::vector<intptr_t> red = wordsIn(s, "red");
        tokCommand(IDM_SEARCH_MARKALLEXT2);
        pump(100);
        const bool still = !red.empty() && sci(s, SCI_INDICATORVALUEAT, 24, red[0]) != 0;
        if (still) check(splashStats().splashes == before.splashes, L"P2", L"styling a token that already has that colour splashes nothing");
        else tlog(L"INFO P2 this Notepad++ takes the style off again on a second Style All");
        // Style One Token: only that occurrence
        const std::vector<intptr_t> pa = wordsIn(s, "paint");
        sci(s, SCI_SETSEL, pa.empty() ? 0 : pa[0], pa.empty() ? 0 : pa[0] + 5);
        const SplashStats b2 = splashStats();
        tokCommand(IDM_SEARCH_MARKONEEXT3);
        pump(100);
        const SplashStats st = splashStats();
        check(st.splashes == b2.splashes + 1 && st.lastTokens == 1, L"P3", L"Style One Token: one splash, on that occurrence only (%d)", st.lastTokens);
        // its switch
        setModuleEnabled(M_SPLASH, false, false);
        const std::vector<intptr_t> ink = wordsIn(s, "ink");
        sci(s, SCI_SETSEL, ink.empty() ? 0 : ink[0], ink.empty() ? 0 : ink[0] + 3);
        tokCommand(IDM_SEARCH_MARKONEEXT1);
        pump(100);
        check(splashStats().spawned == st.spawned, L"P4", L"paint splash off: colouring splashes nothing");
        setModuleEnabled(M_SPLASH, true, false);
        tokCommand(IDM_SEARCH_CLEARALLMARKS);
        sci(s, SCI_SETEMPTYSELECTION, 0);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(150);
        // the Effects page: a line on how the effects are drawn and how fast
        showPrefPage(L"KEX Effects");
        const std::wstring info = itemText(prefPage(PAGE_EFFECTS), IDC_E_INFO);
        check(prefPage(PAGE_EFFECTS) && !info.empty(), L"P5", L"KEX Effects page: \"%ls\"", info.c_str());
        switchTo(L"a.cpp");
        focusEditor();
        return until([] { return splashStats().live == 0 && flashStats().live == 0; }, 4000);
    });

    // ---- every feature off and on again from Preferences ----------------------------------------
    add([] {
        for (int i = 0; i < M_COUNT; ++i)
        {
            if (!modules[i]->defaultOn()) continue;   // off by default: its own tests switch it on and off
            int origStyle0 = static_cast<int>(sci(views.sci(0), SCI_GETCARETSTYLE));
            clickEnable(i);
            pump(100);
            bool off = !modules[i]->isEnabled();
            bool unchecked = !enableTicked(i) && settings.getInt(modules[i]->key(), L"Enabled", 1) == 0;
            std::wstring extra;
            bool restored = true;
            switch (i)
            {
            case M_LETTERS:
                if (!modules[M_FLASH]->isEnabled() && !modules[M_SPLASH]->isEnabled())
                { restored = ::FindWindowW(L"KexSuiteOverlay", L"KEX effects") == nullptr; extra = L"render thread and overlay gone"; }
                else { restored = lettersStats().live == 0; extra = L"no letters left (the flashes and splashes keep the overlay)"; }
                break;
            case M_FLASH: restored = flashStats().live == 0; extra = L"no flashes left"; break;
            case M_SPLASH: restored = splashStats().live == 0; extra = L"no paint left"; break;
            case M_SLIDE: restored = !slideStats(views.sci(0)).active && !slideStats(views.sci(1)).active; extra = L"no slide running"; break;
            case M_TABFOLD: restored = !tabMotionStats().overlayShown; extra = L"tab bars unhooked, no fold window"; break;
            case M_GLINT: restored = !tabMotionStats().overlayShown; extra = L"no glint window"; break;
            case M_SWIPE: restored = !swipeStats().active && !swipeStats().rawRegistered && !slideStats(views.sci(0)).active; extra = L"touchpad reports released, no slide held"; break;
            case M_CARET:
            {
                int now = static_cast<int>(sci(views.sci(0), SCI_GETCARETSTYLE));
                restored = now == origStyle0 && !smoothCaretOn(views.sci(0)) && !smoothCaretOn(views.sci(1));
                wchar_t b[96]; swprintf(b, 96, L"Scintilla's own caret again, caret style 0x%x (was 0x%x)", now, origStyle0); extra = b;
                break;
            }
            case M_HOTKEY:
            {
                BOOL free = ::RegisterHotKey(g_testWnd, 78, MOD_CONTROL | MOD_ALT, 'M');
                if (free) ::UnregisterHotKey(g_testWnd, 78);
                restored = free != FALSE; extra = L"Ctrl+Alt+M released";
                break;
            }
            case M_TABS: restored = ::FindWindowW(L"KexSuiteTabStrip", nullptr) == nullptr; extra = L"strip window destroyed"; break;
            case M_DESKTOP:
            {
                wchar_t ini[16] = {};
                ::GetPrivateProfileStringW(L"PerDesktop", L"Enabled", L"", ini, 15, settings.path().c_str());
                restored = !wcscmp(ini, L"0"); extra = L"a second notepad++.exe will hand its files to this window again";
                break;
            }
            case M_SNAP: restored = ::FindWindowW(L"KexSuiteSnapPreview", nullptr) == nullptr && !kexsuite::tabSnapOn(); extra = L"preview window destroyed, the core is back to the drop menu"; break;
            }
            check(off && unchecked && restored, L"E1", L"%ls off from Preferences: unticked and remembered, %ls", modules[i]->title(), extra.c_str());
            clickEnable(i);
            pump(100);
            bool on = modules[i]->isEnabled() && enableTicked(i) && settings.getInt(modules[i]->key(), L"Enabled", 0) == 1;
            check(on, L"E2", L"%ls back on from Preferences", modules[i]->title());
        }
        DWORD_PTR r = 0;
        check(::SendMessageTimeoutW(npp._nppHandle, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 2000, &r) != 0, L"E3", L"Notepad++ still responsive");
        int ok = 0; for (int i = 0; i < M_COUNT; ++i) ok += modules[i]->_checkOk ? 1 : 0;
        tlog(L"INFO final summary: KEX: %d/%d features OK", ok, M_COUNT);
        sci(views.sci(0), SCI_SETCARETWIDTH, g_origCaretWidth);
        sci(views.sci(1), SCI_SETCARETWIDTH, g_origCaretWidth);
        return 100;
    });
    // ---- views 3 and 4 of a KEX-patched Notepad++, and snapped-out windows ----
    add([] {
        if (!views.patched()) { tlog(L"SKIP K1-K4: Notepad++ is not KEX-patched"); return 50; }
        switchTo(L"c.txt");
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW + 907);   // IDM_KEX_DOC_CLONE_TO_VIEW3
        pump(200);
        HWND s3 = views.sci(2);
        check(views.current() == 2 && views.visible(2), L"K1", L"c.txt cloned into view 3 (bottom left)");
        ::SetFocus(s3);
        sci(s3, SCI_SETFIRSTVISIBLELINE, 0);
        intptr_t a = posOf(s3, 0, 0), b = sci(s3, SCI_GETLINEENDPOSITION, 0);
        int expect = visibleNonBlank(s3, a, b);
        sci(s3, SCI_SETSEL, a, b);
        lettersFreezeAfter(200);
        int before = lettersStats().totalSpawned;
        tap(VK_DELETE);
        pump(150);
        check(expect > 0 && lettersStats().totalSpawned - before == expect, L"K2", L"view 3: deleting a line spawned %d letters (expected %d)",
            lettersStats().totalSpawned - before, expect);
        pump(100);
        check(caretStats().target == s3 && caretStats().visible, L"K3", L"view 3 focused: smooth caret drawn there (target %p, view 3 %p, visible %d, focus %p, carets %d, main %.0f,%.0f)",
            caretStats().target, s3, caretStats().visible, ::GetFocus(), caretStats().carets, caretStats().mainX, caretStats().mainY);
        return 300;
    });
    add([] { return views.patched() ? shot(L"kex-view3-letters") : 0; });
    add([] {
        if (!views.patched()) return 0;
        lettersFreezeAfter(-1);
        if (!isPostItMode()) nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);
        pump(300);
        RECT r; ::GetWindowRect(views.sci(2), &r);
        ::SetCursorPos((r.left + r.right) / 2, r.top + 2);
        return until([] { return tabsStats().stripVisible; }, 2000);
    });
    add([] {
        if (!views.patched()) return 0;
        TabsStats t = tabsStats();
        RECT r; ::GetWindowRect(views.sci(2), &r);
        check(g_untilOk && t.view == 2 && t.strip.top >= r.top - 2 && t.strip.top <= r.top + 4 && t.strip.left >= r.left - 2 && t.strip.right <= r.right + 2, L"K5",
            L"F12: hovering the top of view 3 shows view 3's tabs over view 3 (strip %ls, view 3 %ls, view %d)", rectStr(t.strip).c_str(), rectStr(r).c_str(), t.view + 1);
        return 100;
    });
    add([] { return views.patched() ? shot(L"kex-view3-hover-tabs") : 0; });
    add([] {
        if (!views.patched()) return 0;
        ::SetCursorPos(10, 10);
        pump(600);
        if (isPostItMode()) nppMsg(NPPM_MENUCOMMAND, 0, IDM_VIEW_POSTIT);
        pump(300);
        return 100;
    });
    add([] {
        if (!views.patched()) return 0;
        lettersFreezeAfter(-1);
        HWND s3 = views.sci(2);
        nppMsg(NPPM_KEX_TEAROUTVIEW, 2, TRUE);
        pump(250);
        HWND host = reinterpret_cast<HWND>(nppMsg(NPPM_KEX_GETVIEWHOSTWINDOW, 2));
        ::SetForegroundWindow(host);
        ::SetFocus(s3);
        sci(s3, SCI_GOTOPOS, posOf(s3, 1, 3));
        pump(250);
        RECT cr; ::GetClientRect(s3, &cr);
        CaretStats cs = caretStats();
        bool inside = cs.mainX >= cr.left && cs.mainX < cr.right && cs.mainY >= cr.top && cs.mainY + cs.mainH <= cr.bottom;
        check(host != npp._nppHandle && cs.target == s3 && cs.visible && inside, L"K4", L"view 3 snapped out: caret drawn in its own window at (%.0f,%.0f), inside view 3 %ls",
            cs.mainX, cs.mainY, rectStr(cr).c_str());
        return 200;
    });
    add([] { return views.patched() ? shot(L"kex-view3-snapped-out-caret") : 0; });
    add([] {
        if (!views.patched()) return 0;
        sci(views.sci(2), SCI_UNDO);
        nppMsg(NPPM_KEX_TEAROUTVIEW, 2, FALSE);
        pump(200);
        return 100;
    });
    // ---- JetBrains shortcuts ----------------------------------------------------------------------
    add([] {
        writeFile(path(L"j.cpp"), "int value = 1;\r\nint other = value + value;\r\ncall(alpha, \"a string here\", beta);\r\n// fourth line\r\n");
        open(L"j.cpp");
        pump(200);
        if (!modules[M_KEYS]->isEnabled()) setModuleEnabled(M_KEYS, true, false);
        focusEditor();
        HWND s = curSci();
        sci(s, SCI_GOTOPOS, 6);   // inside "value"
        pump(100);
        chord({ VK_MENU }, 'J'); pump(150);
        std::string first = selText(s);
        chord({ VK_MENU }, 'J'); pump(150);
        chord({ VK_MENU }, 'J'); pump(150);
        int n = static_cast<int>(sci(s, SCI_GETSELECTIONS));
        check(first == "value" && n == 3, L"J1", L"Alt+J from the caret selects the word, then each next occurrence: \"%hs\", %d selections", first.c_str(), n);
        chord({ VK_SHIFT, VK_MENU }, 'J'); pump(150);
        n = static_cast<int>(sci(s, SCI_GETSELECTIONS));
        GUITHREADINFO gi{}; gi.cbSize = sizeof(gi);
        ::GetGUIThreadInfo(::GetCurrentThreadId(), &gi);
        check(n == 2 && !(gi.flags & GUI_INMENUMODE), L"J2", L"Shift+Alt+J drops the last one (%d left); releasing Alt does not open the menu bar", n);
        sci(s, SCI_GOTOPOS, 6); pump(50);
        chord({ VK_CONTROL, VK_SHIFT, VK_MENU }, 'J'); pump(150);
        n = static_cast<int>(sci(s, SCI_GETSELECTIONS));
        check(n == 3, L"J3", L"Ctrl+Shift+Alt+J selects all %d occurrences", n);
        sci(s, SCI_GOTOPOS, 6);
        return 100;
    });
    add([] {
        HWND s = curSci();
        intptr_t l2 = sci(s, SCI_POSITIONFROMLINE, 2);
        sci(s, SCI_GOTOPOS, l2 + 16);   // inside "string"
        pump(50);
        const char* expect[] = { "string", "a string here", "\"a string here\"", "alpha, \"a string here\", beta", "(alpha, \"a string here\", beta)",
                                 "call(alpha, \"a string here\", beta)", "call(alpha, \"a string here\", beta);" };
        bool ok = true;
        std::string got;
        for (const char* e : expect)
        {
            chord({ VK_CONTROL }, 'W'); pump(120);
            got = selText(s);
            if (got != e) { ok = false; break; }
        }
        check(ok, L"J4", L"Ctrl+W extends the selection word > string > quotes > brackets > call > line (stopped at \"%hs\")", got.c_str());
        chord({ VK_CONTROL, VK_SHIFT }, 'W'); pump(120);
        got = selText(s);
        check(got == "call(alpha, \"a string here\", beta)", L"J5", L"Ctrl+Shift+W shrinks it back: \"%hs\"", got.c_str());
        check(::IsWindow(s) && curName() == L"j.cpp", L"J6", L"Ctrl+W no longer closes the tab while JetBrains keys are on");
        return 100;
    });
    add([] {
        HWND s = curSci();
        intptr_t lines = sci(s, SCI_GETLINECOUNT);
        sci(s, SCI_GOTOPOS, sci(s, SCI_POSITIONFROMLINE, 3));
        chord({ VK_CONTROL }, 'Y'); pump(150);
        bool deleted = sci(s, SCI_GETLINECOUNT) == lines - 1 && lineText(s, 3).empty();
        chord({ VK_CONTROL }, 'Z'); pump(150);
        check(deleted && sci(s, SCI_GETLINECOUNT) == lines, L"J7", L"Ctrl+Y deletes the line (Ctrl+Z brings it back)");
        sci(s, SCI_GOTOPOS, 0);
        chord({ VK_CONTROL }, VK_OEM_2); pump(150);
        std::string commented = lineText(s, 0);
        chord({ VK_CONTROL }, VK_OEM_2); pump(150);
        check(commented.rfind("//", 0) == 0 && lineText(s, 0) == "int value = 1;", L"J8", L"Ctrl+/ comments the line and uncomments it: \"%hs\"", commented.c_str());
        sci(s, SCI_GOTOPOS, 2);
        chord({ VK_SHIFT, VK_MENU }, VK_DOWN); pump(150);
        bool moved = lineText(s, 1) == "int value = 1;" && lineText(s, 0) == "int other = value + value;";
        chord({ VK_SHIFT, VK_MENU }, VK_UP); pump(150);
        check(moved && lineText(s, 0) == "int value = 1;", L"J9", L"Shift+Alt+Down / Up move the line");
        sci(s, SCI_GOTOPOS, 4);
        chord({ VK_SHIFT }, VK_RETURN); pump(150);
        intptr_t caretLine = sci(s, SCI_LINEFROMPOSITION, sci(s, SCI_GETCURRENTPOS));
        bool newLine = lineText(s, 0) == "int value = 1;" && caretLine == 1 && lineText(s, 1).empty();
        chord({ VK_CONTROL }, 'Z'); pump(150);
        check(newLine, L"J10", L"Shift+Enter starts a new line below without splitting this one");
        std::wstring before = curName();
        chord({ VK_MENU }, VK_RIGHT); pump(200);
        std::wstring after = curName();
        chord({ VK_MENU }, VK_LEFT); pump(200);
        check(after != before && curName() == before, L"J11", L"Alt+Right / Alt+Left switch tabs (%ls > %ls > %ls)", before.c_str(), after.c_str(), curName().c_str());
        return 100;
    });
    add([] {
        // keeping Notepad++'s own keys: Ctrl+Y is redo again, Alt+J still works
        settings.setInt(L"JetBrainsKeys", L"AlsoNotepadKeys", 0);
        setModuleEnabled(M_KEYS, false, false); setModuleEnabled(M_KEYS, true, false);
        focusEditor();
        HWND s = curSci();
        sci(s, SCI_EMPTYUNDOBUFFER);   // so Notepad++'s redo has nothing to redo
        intptr_t lines = sci(s, SCI_GETLINECOUNT);
        sci(s, SCI_GOTOPOS, sci(s, SCI_POSITIONFROMLINE, 3));
        chord({ VK_CONTROL }, 'Y'); pump(150);
        bool kept = sci(s, SCI_GETLINECOUNT) == lines;
        sci(s, SCI_GOTOPOS, 6);
        chord({ VK_MENU }, 'J'); pump(150);
        check(kept && selText(s) == "value", L"J12", L"\"Also the keys Notepad++ uses\" off: Ctrl+Y left to Notepad++, Alt+J still works");
        settings.setInt(L"JetBrainsKeys", L"AlsoNotepadKeys", 1);
        setModuleEnabled(M_KEYS, false, false);
        sci(s, SCI_GOTOPOS, sci(s, SCI_POSITIONFROMLINE, 3));
        chord({ VK_MENU }, 'J'); pump(150);
        bool inert = sci(s, SCI_GETSELECTIONEMPTY) != 0;
        ::EndMenu(); tap(VK_ESCAPE); pump(100);
        setModuleEnabled(M_KEYS, true, false);
        check(inert && modules[M_KEYS]->isEnabled(), L"J13", L"switched off, Alt+J does nothing; back on afterwards (%d keys handled in all)", jetBrainsStats().handled);
        return 100;
    });
    add([] {
        // the keys are Notepad++ commands of KEX's: in Settings > KEX Editing > JetBrains Commands and in
        // Shortcut Mapper (plugin "KEX"); switched off, the menu shows them without keys
        auto menuText = [](UINT id) { wchar_t t[128] = {}; ::GetMenuStringW(settingsSubmenu(PAGE_EDITING), id, t, 128, MF_BYCOMMAND); return std::wstring(t); };
        HMENU edit = settingsSubmenu(PAGE_EDITING);
        HMENU sub = nullptr;
        for (int i = 0, n = edit ? ::GetMenuItemCount(edit) : 0; i < n; ++i)
        {
            wchar_t t[64] = {};
            ::GetMenuStringW(edit, i, t, 64, MF_BYPOSITION);
            if (std::wstring(t) == L"JetBrains Commands") sub = ::GetSubMenu(edit, i);
        }
        const UINT next = static_cast<UINT>(jetBrainsCommandId(0));
        ShortcutKey sk{};
        const bool mapped = next && nppMsg(NPPM_GETSHORTCUTBYCMDID, next, reinterpret_cast<LPARAM>(&sk)) && sk._key == 'J' && sk._isAlt && !sk._isCtrl && !sk._isShift;
        const std::wstring on = menuText(next);
        setModuleEnabled(M_KEYS, false, false);
        const std::wstring off = menuText(next);
        setModuleEnabled(M_KEYS, true, false);
        const std::wstring back = menuText(next);
        check(sub && ::GetMenuItemCount(sub) == 27 && mapped && on == L"Select Next Occurrence\tAlt+J" && off == L"Select Next Occurrence" && back == on, L"J14",
              L"Settings > KEX Editing > JetBrains Commands has the 27 commands, Shortcut Mapper has Alt+J for \"%ls\"; off: \"%ls\"", on.c_str(), off.c_str());
        return 100;
    });
    // ---- tab snapping: drag a tab to the edge of a view ------------------------------------------
    add([] {
        if (!views.patched()) { tlog(L"SKIP N1-N9: Notepad++ is not KEX-patched"); return 50; }
        // start from one view: every tab back in the main view
        for (int v = 1; v < views.count(); ++v)
            for (int guard = 0; views.visible(v) && views.nbFiles(v) > 0 && guard < 20; ++guard)
            {
                views.activate(v, views.nbFiles(v) - 1);
                pump(50);
                nppMsg(NPPM_MENUCOMMAND, 0, IDM_KEX_DOC_TO_VIEW1);
                pump(80);
            }
        ::ShowWindow(npp._nppHandle, SW_RESTORE);
        ::MoveWindow(npp._nppHandle, 20, 16, 960, 660, TRUE);
        ::SetForegroundWindow(npp._nppHandle);
        switchTo(L"b.py");
        pump(300);
        check(views.visible(0) && !views.visible(1) && !views.visible(2) && !views.visible(3) && views.nbFiles(0) >= 4, L"N0",
            L"one view with %d tabs to drag from", views.nbFiles(0));
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        RECT rv = viewRect(0);
        const int w = rv.right - rv.left;
        dragStart(0);
        dragTo(rv.right - w / 10, (rv.top + rv.bottom) / 2);
        SnapStats st = snapStats();
        const int mid = rv.left + w / 2;
        bool rightHalf = st.target.left > mid - 12 && st.target.left < mid + 12 && std::abs(st.target.right - rv.right) <= 3
                      && std::abs(st.target.top - rv.top) <= 3 && std::abs(st.target.bottom - rv.bottom) <= 3;
        bool inside = st.window.left >= st.target.left && st.window.right <= st.target.right && st.window.top >= st.target.top && st.window.bottom <= st.target.bottom;
        check(st.visible && rightHalf && inside, L"N1", L"dragging %ls to the right edge previews the right half: %ls (view %ls, preview window %ls)",
            g_dragFile.c_str(), rectStr(st.target).c_str(), rectStr(rv).c_str(), rectStr(st.window).c_str());
        HWND pw = ::FindWindowW(L"KexSuiteSnapPreview", nullptr);
        LONG_PTR ex = pw ? ::GetWindowLongPtrW(pw, GWL_EXSTYLE) : 0;
        check(pw && ::IsWindowVisible(pw) && (ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED) && (ex & WS_EX_NOACTIVATE), L"N2",
            L"the preview is a click-through layered window that never takes focus");
        POINT c; ::GetCursorPos(&c);
        HWND lw = ::FindWindowW(L"KexSuiteTabLift", nullptr);
        LONG_PTR lex = lw ? ::GetWindowLongPtrW(lw, GWL_EXSTYLE) : 0;
        check(st.lifted && st.lifts >= 1 && ::PtInRect(&st.lift, c) && lw && ::IsWindowVisible(lw) && (lex & WS_EX_TRANSPARENT) && (lex & WS_EX_LAYERED),
            L"N12", L"the dragged tab is lifted off the bar and follows the mouse: %ls around the cursor (%ld,%ld), click-through", rectStr(st.lift).c_str(), c.x, c.y);
        return 50;
    });
    add([] { return views.patched() ? shot(L"tab-snap-preview-right") : 0; });
    add([] {
        if (!views.patched()) return 0;
        RECT predicted = snapStats().target;
        dragEnd();
        RECT r1 = viewRect(1);
        check(views.visible(1) && viewOfFile(g_dragFile) == 1 && views.nbFiles(1) == 1 && views.current() == 1 && !snapStats().visible && !snapStats().lifted, L"N3",
            L"dropped there: %ls opened in a new view on the right, preview and lifted tab gone", g_dragFile.c_str());
        check(rectNear(r1, predicted, 6), L"N4", L"the new view sits where the preview was: %ls vs preview %ls", rectStr(r1).c_str(), rectStr(predicted).c_str());
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // two views side by side: the bottom of the left one opens a view below it (a quarter)
        switchTo(L"c.txt");
        pump(150);
        RECT rv = viewRect(0);
        const int h = rv.bottom - rv.top;
        dragStart(0);
        dragTo((rv.left + rv.right) / 2, rv.bottom - h / 10);
        SnapStats st = snapStats();
        const int mid = rv.top + h / 2;
        bool bottomHalf = st.visible && st.target.top > mid - 14 && st.target.top < mid + 14 && std::abs(st.target.bottom - rv.bottom) <= 3
                       && std::abs(st.target.left - rv.left) <= 3 && std::abs(st.target.right - rv.right) <= 3;
        check(bottomHalf, L"N5", L"the bottom edge of the left view previews its bottom half: %ls (view %ls)", rectStr(st.target).c_str(), rectStr(rv).c_str());
        return 50;
    });
    add([] { return views.patched() ? shot(L"tab-snap-preview-quarter") : 0; });
    add([] {
        if (!views.patched()) return 0;
        RECT predicted = snapStats().target;
        dragEnd();
        RECT r3 = viewRect(2);
        check(views.visible(2) && viewOfFile(g_dragFile) == 2 && rectNear(r3, predicted, 6), L"N6",
            L"dropped: %ls opened bottom left at %ls (preview %ls)", g_dragFile.c_str(), rectStr(r3).c_str(), rectStr(predicted).c_str());
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // the middle of another view: the tab moves into it, the preview covers that view
        switchTo(L"d.txt");
        pump(150);
        RECT r1 = viewRect(1);
        int before = views.nbFiles(1);
        dragStart(viewOfFile(L"d.txt"));
        dragTo((r1.left + r1.right) / 2, (r1.top + r1.bottom) / 2);
        SnapStats st = snapStats();
        check(st.visible && rectNear(st.target, r1, 3), L"N7", L"over the middle of the right view the preview covers it: %ls (view %ls)",
            rectStr(st.target).c_str(), rectStr(r1).c_str());
        dragEnd();
        check(viewOfFile(L"d.txt") == 1 && views.nbFiles(1) == before + 1, L"N8", L"dropped: d.txt moved into the right view");
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // Esc while dragging cancels: nothing moves
        switchTo(L"a.cpp");
        pump(150);
        const int from = viewOfFile(L"a.cpp");
        RECT r1 = viewRect(1);
        int cancels = snapStats().cancels;
        dragStart(from);
        dragTo((r1.left + r1.right) / 2, r1.bottom - (r1.bottom - r1.top) / 10);
        bool shown = snapStats().visible;
        key(VK_ESCAPE, true); pump(200); key(VK_ESCAPE, false); pump(100);
        bool hidden = !snapStats().visible && snapStats().cancels == cancels + 1;
        dragEnd();
        check(shown && hidden && viewOfFile(L"a.cpp") == from && !views.visible(3), L"N9",
            L"Esc during a drag hides the preview and drops nothing (a.cpp still in view %d)", viewOfFile(L"a.cpp") + 1);
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // a quick pull straight down from the tab's label, out of the tab bar at once, still drags the tab
        switchTo(L"a.cpp");
        pump(150);
        const int from = viewOfFile(L"a.cpp");
        HWND tab = views.tabBar(from);
        const int idx = views.currentIndex(from);
        RECT r{};
        ::SendMessageW(tab, TCM_GETITEMRECT, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&r));
        ::MapWindowPoints(tab, nullptr, reinterpret_cast<POINT*>(&r), 2);
        const int x = (r.left + r.right) / 2, y = r.bottom - 4;   // on the label, near the bottom edge
        const int lifts = snapStats().lifts;
        mouseTo(x, y); pump(80);
        mouseButton(true); pump(60);
        mouseTo(x, y + 40); pump(30);
        mouseTo(x, y + 90); pump(250);
        SnapStats st = snapStats();
        POINT c; ::GetCursorPos(&c);
        check(st.lifts == lifts + 1 && st.lifted && ::PtInRect(&st.lift, c), L"N13",
            L"pressing a tab's label and pulling straight down out of the tab bar starts the drag at once (lifted tab at %ls)", rectStr(st.lift).c_str());
        mouseTo(x, (r.top + r.bottom) / 2); pump(100);   // back onto its own tab
        dragEnd();
        check(viewOfFile(L"a.cpp") == from && !snapStats().lifted && !snapStats().visible, L"N14",
            L"dropped back on its own tab: nothing moved, the lifted tab is gone");
        return 200;
    });
    add([] {
        if (!views.patched()) return 0;
        // switched off: no preview, the classic drop handling (a drop on another view still moves it)
        setModuleEnabled(M_SNAP, false, false);
        switchTo(L"a.cpp");
        pump(150);
        const int from = viewOfFile(L"a.cpp");
        const int to = (from == 1) ? 0 : 1;
        RECT rt = viewRect(to);
        int shows = snapStats().shows;
        dragStart(from);
        dragTo((rt.left + rt.right) / 2, (rt.top + rt.bottom) / 2);
        bool noPreview = !snapStats().visible && snapStats().shows == shows;
        dragEnd();
        check(noPreview && viewOfFile(L"a.cpp") == to, L"N10", L"tab snapping off: no preview, dropping on another view still moves the tab (view %d)", viewOfFile(L"a.cpp") + 1);
        setModuleEnabled(M_SNAP, true, false);
        check(kexsuite::tabSnapOn(), L"N11", L"tab snapping back on");
        return 200;
    });
    // ---- glass text area: colour key, click catcher, theme changes, off again ---------------------
    add([] {
        // Wine has no DWM blur, so the test engine keys the views and places the catcher without it
        settings.setStr(L"Glass", L"Engine", L"keyonly");
        std::string text;
        for (int i = 1; i <= 300; ++i) text += "glass line " + std::to_string(i) + "\r\n";
        writeFile(path(L"g.txt"), text);
        ::ShowWindow(npp._nppHandle, SW_RESTORE);
        ::MoveWindow(npp._nppHandle, 20, 16, 960, 660, TRUE);
        views.activate(0, 0);
        pump(100);
        open(L"g.txt");
        focusEditor();
        if (views.sci(1)) sci(views.sci(1), SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITE);   // a Direct2D view, if this Wine has Direct2D
        pump(200);
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            g_glassBack[v] = s ? static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT)) : 0;
            g_glassTech[v] = s ? static_cast<int>(sci(s, SCI_GETTECHNOLOGY)) : -1;
            g_glassQuality[v] = s ? static_cast<int>(sci(s, SCI_GETFONTQUALITY)) : -1;
        }
        tlog(L"INFO glass: g.txt in view %d, theme background %06lx, renderers %d %d %d %d", viewOfFile(L"g.txt") + 1,
             static_cast<unsigned long>(g_glassBack[0]), g_glassTech[0], g_glassTech[1], g_glassTech[2], g_glassTech[3]);
        clickEnable(M_GLASS);
        pump(400);
        ::SendMessageW(prefDialog(), WM_COMMAND, IDCANCEL, 0);
        pump(200);
        GlassStats g = glassStats();
        COLORREF key = 0;
        const bool keyed = keyedWith(npp._nppHandle, &key);
        check(modules[M_GLASS]->isEnabled() && g.active && keyed && key == (g_glassBack[0] ^ 0x010001) && settings.getInt(L"Glass", L"Enabled", 0) == 1, L"G1",
              L"glass on from Preferences: the main window is colour-keyed with %06lx (theme background %06lx)", static_cast<unsigned long>(key), static_cast<unsigned long>(g_glassBack[0]));
        int left = 0;
        for (int v = 0; v < views.count(); ++v) if (HWND s = views.sci(v)) left += stylesWithBack(s, g_glassBack[v]);
        HWND s0 = views.sci(0);
        check(viewsKeyed(key) && left == 0 && static_cast<COLORREF>(sci(s0, SCI_STYLEGETBACK, STYLE_LINENUMBER)) == key, L"G2",
              L"all four views' backgrounds and the line-number margin are the key (%d styles left on the old background)", left);
        bool tech = true;
        std::wstring ts;
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            if (!s) continue;
            const int o = g_glassTech[v], now = static_cast<int>(sci(s, SCI_GETTECHNOLOGY));
            const int want = (o == SC_TECHNOLOGY_DIRECTWRITE || o == SC_TECHNOLOGY_DIRECTWRITERETAIN || o == SC_TECHNOLOGY_DIRECT_WRITE_1) ? SC_TECHNOLOGY_DIRECTWRITEDC : o;
            if (now != want) tech = false;
            ts += std::to_wstring(o) + L">" + std::to_wstring(now) + L" ";
        }
        check(tech, L"G3", L"Direct2D window renderers drawn through a DC while the glass is on (view renderers %ls)", ts.c_str());
        // each view's text goes on a see-through layer of its own (DirectComposition); where there is none (Wine has
        // only stubs) the view draws through a DC without smoothing, so letters get no outline
        bool sharp = true;
        for (int v = 0; v < views.count(); ++v)
            if (HWND s = views.sci(v)) sharp = sharp && sci(s, SCI_GETFONTQUALITY) == SC_EFF_QUALITY_NON_ANTIALIASED;
        const int nv = viewCount();
        if (underWine)
            check(g.fallbackViews == nv && g.layerViews == 0 && sharp, L"G16",
                  L"no see-through layer under Wine: all %d views fall back to text without smoothing (Notepad++ had quality %d, layer error 0x%08lx)",
                  nv, g_glassQuality[0], g.layerError);
        else
            check(g.layerViews == nv && g.fallbackViews == 0, L"G16", L"all %d views draw their text on a see-through layer (%d fell back, error 0x%08lx)",
                  nv, g.fallbackViews, g.layerError);
        HWND c = glassCatcherOf(npp._nppHandle);
        RECT cr = clientOnScreen(npp._nppHandle), kr = screenRectOf(c);
        check(c && ::IsWindowVisible(c) && ::EqualRect(&cr, &kr) && nextVisible(npp._nppHandle) == c && g.hosts >= 1, L"G4",
              L"a click catcher %ls sits directly behind the window, over its client area %ls", rectStr(kr).c_str(), rectStr(cr).c_str());
        return 200;
    });
    add([] { return shot(L"glass-on"); });
    add([] {
        HWND s = views.sci(0);
        HWND c = glassCatcherOf(npp._nppHandle);
        sci(s, SCI_SETFIRSTVISIBLELINE, 0);
        sci(s, SCI_GOTOPOS, 0);
        const int line = 3;
        POINT pt = glassPoint(line);
        const int clicks = glassStats().clicks;
        ::SendMessageW(c, WM_LBUTTONDOWN, MK_LBUTTON, glassLParam(c, pt));
        ::SendMessageW(c, WM_LBUTTONUP, 0, glassLParam(c, pt));
        pump(150);
        const intptr_t caret = sci(s, SCI_GETCURRENTPOS), end = sci(s, SCI_GETLINEENDPOSITION, line);
        check(caret == end && glassStats().clicks == clicks + 1 && glassStats().lastTarget == s && ::GetFocus() == s, L"G5",
              L"a click on the glass right of line %d goes to the view under it: caret %d, line end %d", line + 1, static_cast<int>(caret), static_cast<int>(end));
        HWND real = ::WindowFromPoint(pt);
        check(kexsuite::windowFromPoint(pt) == s, L"G6", L"the core's windowFromPoint finds the view there (plain WindowFromPoint gives %ls)", classOf(real).c_str());
        const int wheels = glassStats().wheels;
        ::SendMessageW(c, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-3 * WHEEL_DELTA)), MAKELPARAM(pt.x, pt.y));
        pump(150);
        const intptr_t first = sci(s, SCI_GETFIRSTVISIBLELINE);
        check(first > 0 && glassStats().wheels == wheels + 1, L"G7", L"the mouse wheel on the glass scrolls the view under it (first line %d)", static_cast<int>(first) + 1);
        sci(s, SCI_SETFIRSTVISIBLELINE, 0);
        pt = glassPoint(line);
        ::PostMessageW(c, WM_RBUTTONDOWN, MK_RBUTTON, glassLParam(c, pt));
        ::PostMessageW(c, WM_RBUTTONUP, 0, glassLParam(c, pt));
        return 500;
    });
    add([] {
        HWND menu = ::FindWindowW(L"#32768", nullptr);
        const bool shown = menu && ::IsWindowVisible(menu);
        ::EndMenu();
        check(shown, L"G8", L"a right-click on the glass opens the editor's context menu");
        return 300;
    });
    add([] {
        COLORREF key = glassStats().key;
        int remaps = glassStats().remaps;
        ::SendMessageW(npp._nppHandle, WM_UPDATESCINTILLAS_, FALSE, 0);
        int left = stylesWithBack(views.sci(0), g_glassBack[0]);
        check(viewsKeyed(key) && left == 0 && glassStats().remaps > remaps, L"G9",
              L"Notepad++ re-applying the theme (Style Configurator) keeps the glass (%d styles left on the old background)", left);
        focusEditor();
        nppMsg(NPPM_SETCURRENTLANGTYPE, 0, L_PYTHON);
        pump(100);
        left = stylesWithBack(views.sci(0), g_glassBack[0]);
        bool lang = viewsKeyed(key) && left == 0;
        nppMsg(NPPM_SETCURRENTLANGTYPE, 0, L_TEXT);
        pump(100);
        check(lang && viewsKeyed(key), L"G10", L"switching g.txt to Python and back keeps the glass (%d styles left)", left);
        glassTestDragEnter();
        const bool solid = !keyedWith(npp._nppHandle, nullptr) && glassStats().dragSolidNow;
        check(solid, L"G11", L"dragging something over the glass turns it solid, so the view's own drop handling gets the drop");
        return 400;
    });
    add([] {
        check(keyedWith(npp._nppHandle, nullptr) && !glassStats().dragSolidNow, L"G12", L"the glass comes back once the mouse button is up");
        HWND c = glassCatcherOf(npp._nppHandle);
        if (g_otherWnd && ::IsWindow(g_otherWnd)) { ::SetForegroundWindow(g_otherWnd); pump(250); }
        ::SetForegroundWindow(npp._nppHandle);
        pump(250);
        ::MoveWindow(npp._nppHandle, 60, 40, 900, 640, TRUE);
        pump(400);
        RECT cr = clientOnScreen(npp._nppHandle), kr = screenRectOf(c);
        check(nextVisible(npp._nppHandle) == c && ::EqualRect(&cr, &kr), L"G13",
              L"after another program was in front and the window moved, the catcher is still right behind it at %ls", rectStr(kr).c_str());
        openPreferences();
        pump(300);
        showPrefPage(L"KEX Windows");
        pump(200);
        HWND p = prefPage(PAGE_WINDOWS);
        std::wstring st = itemText(p, IDC_G_STATUS), info = itemText(p, IDC_G_INFO), tint = itemText(p, IDC_G_TINT);
        std::wstring style = itemText(p, IDC_G_STYLE), edges = itemText(p, IDC_G_EDGES);
        check(st == L"Running" && info.rfind(L"On:", 0) == 0 && tint == L"40" && style == L"Blur" && edges == L"Smooth"
              && ::IsDlgButtonChecked(p, IDC_G_SMOOTHMOVE) == BST_CHECKED, L"G14",
              L"KEX Windows page: \"%ls\", \"%ls\", style %ls, tint %ls%%, text edges %ls", st.c_str(), info.c_str(), style.c_str(), tint.c_str(), edges.c_str());
        return 200;
    });
    add([] { return shot(L"preferences-kex-windows-glass"); });
    add([] {
        HWND p = prefPage(PAGE_WINDOWS);
        pickCombo(p, IDC_G_EDGES, 1);   // Sharp
        pump(300);
        const GlassStats g = glassStats();
        bool sharp = true;
        for (int v = 0; v < views.count(); ++v)
            if (HWND s = views.sci(v)) sharp = sharp && (g.layerViews > 0 || sci(s, SCI_GETFONTQUALITY) == SC_EFF_QUALITY_NON_ANTIALIASED);
        check(g.active && sharp && settings.getInt(L"Glass", L"Edges", 0) == 1 && g.layerViews + g.fallbackViews == viewCount(), L"G17",
              L"Text edges Sharp is remembered and the glass restarts with it (%d views on a layer, %d without)", g.layerViews, g.fallbackViews);
        pickCombo(p, IDC_G_EDGES, 0);
        pickCombo(p, IDC_G_STYLE, 1);   // acrylic: the test engine has no blur, the setting must still be taken
        pump(200);
        check(glassStats().active && settings.getInt(L"Glass", L"Style", 0) == 1, L"G18", L"Style Acrylic is remembered and the glass restarts with it");
        // acrylic lags behind a moving window, a plain blur doesn't: during a drag the plain blur moves instead
        HWND host = npp._nppHandle, cat = glassCatcherOf(host), blur = glassBlurCatcherOf(host);
        if (!blur) { tlog(L"SKIP G21-G22 no acrylic on this Windows (build below 1803)"); return 100; }
        const GlassStats g0 = glassStats();
        ::SendMessageW(host, WM_ENTERSIZEMOVE, 0, 0);
        const bool under = ::IsWindowVisible(blur) && ::IsWindowVisible(cat) && nextVisible(cat) == blur;
        RECT wr; ::GetWindowRect(host, &wr);
        RECT cr = clientOnScreen(host);
        ::OffsetRect(&wr, 30, 20);
        ::OffsetRect(&cr, 30, 20);
        ::SendMessageW(host, WM_MOVING, WMSZ_LEFT, reinterpret_cast<LPARAM>(&wr));
        RECT br = screenRectOf(blur);
        const bool swapped = glassStats().swaps == g0.swaps + 1 && ::IsWindowVisible(blur) && !::IsWindowVisible(cat) && nextVisible(host) == blur && ::EqualRect(&br, &cr);
        ::SendMessageW(host, WM_EXITSIZEMOVE, 0, 0);
        const bool back = glassStats().swapsBack == g0.swapsBack + 1 && ::IsWindowVisible(cat) && nextVisible(host) == cat;
        check(under && swapped && back, L"G21",
              L"a drag with acrylic: the plain blur waits under it (%d), takes over on the first move at %ls (%d), the acrylic comes back when the drag ends (%d)",
              under ? 1 : 0, rectStr(br).c_str(), swapped ? 1 : 0, back ? 1 : 0);
        return 400;
    });
    add([] {
        HWND blur = glassBlurCatcherOf(npp._nppHandle);
        if (blur) check(!::IsWindowVisible(blur) && ::IsWindowVisible(glassCatcherOf(npp._nppHandle)), L"G22", L"the plain blur goes once the acrylic is back");
        HWND p = prefPage(PAGE_WINDOWS);
        pickCombo(p, IDC_G_STYLE, 0);
        pump(300);
        check(!glassBlurCatcherOf(npp._nppHandle), L"G23", L"Style Blur: no second catcher (nothing to swap)");
        return 100;
    });
    add([] {
        // the text layer as Scintilla draws it (into a bitmap, so it can be counted): the key colour comes out
        // see-through, the text stays; Smooth gives soft edges, Sharp hard ones
        const COLORREF key = glassStats().key;
        HWND s = views.sci(viewOfFile(L"g.txt") >= 0 ? viewOfFile(L"g.txt") : 0);
        const int tech = static_cast<int>(sci(s, SCI_GETTECHNOLOGY)), quality = static_cast<int>(sci(s, SCI_GETFONTQUALITY));
        if (tech == SC_TECHNOLOGY_DEFAULT) sci(s, SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITEDC);
        if (sci(s, SCI_GETTECHNOLOGY) == SC_TECHNOLOGY_DEFAULT) { tlog(L"SKIP G19-G20 no Direct2D in this Wine"); return 50; }
        sci(s, SCI_SETFONTQUALITY, SC_EFF_QUALITY_ANTIALIASED);
        KexGlassTest smooth{}, hard{};
        smooth.key = hard.key = static_cast<int>(key);
        hard.sharp = 1;
        const LRESULT ok1 = sci(s, SCI_KEX_GLASSTEST, 0, reinterpret_cast<LPARAM>(&smooth));
        const LRESULT ok2 = sci(s, SCI_KEX_GLASSTEST, 0, reinterpret_cast<LPARAM>(&hard));
        sci(s, SCI_SETFONTQUALITY, quality);
        if (tech == SC_TECHNOLOGY_DEFAULT) sci(s, SCI_SETTECHNOLOGY, tech);
        tlog(L"INFO glass layer test: smooth %d clear, %d solid, %d soft, %d key (0x%08x); sharp %d clear, %d solid, %d soft, %d key (0x%08x)",
             smooth.clear, smooth.solid, smooth.partial, smooth.keyLeft, static_cast<unsigned>(smooth.error), hard.clear, hard.solid, hard.partial, hard.keyLeft,
             static_cast<unsigned>(hard.error));
        check(ok1 && ok2 && smooth.clear > 0 && smooth.keyLeft == 0 && hard.keyLeft == 0 && smooth.solid + smooth.partial > 0, L"G19",
              L"the text layer: the key comes out see-through (%d clear pixels, %d of the key left), the text stays (%d solid, %d soft)",
              smooth.clear, smooth.keyLeft, smooth.solid, smooth.partial);
        if (underWine && smooth.partial == 0 && hard.partial == 0)
            tlog(L"SKIP G20 Wine's Direct2D draws text without smoothing, so soft edges can only be counted on Windows");
        else
            check(smooth.partial > hard.partial, L"G20", L"Smooth text edges are soft on the layer (%d partly see-through pixels), Sharp ones hard (%d)",
                  smooth.partial, hard.partial);
        return 100;
    });
    add([] {
        clickEnable(M_GLASS);
        pump(300);
        ::SendMessageW(prefDialog(), WM_COMMAND, IDCANCEL, 0);
        pump(200);
        const bool layered = (::GetWindowLongPtrW(npp._nppHandle, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        bool back = true, tech = true;
        for (int v = 0; v < views.count(); ++v)
            if (HWND s = views.sci(v))
            {
                if (static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT)) != g_glassBack[v]) back = false;
                if (static_cast<int>(sci(s, SCI_GETTECHNOLOGY)) != g_glassTech[v]) tech = false;
                if (static_cast<int>(sci(s, SCI_GETFONTQUALITY)) != g_glassQuality[v]) tech = false;
            }
        check(!modules[M_GLASS]->isEnabled() && !layered && back && tech && !glassCatcherOf(npp._nppHandle) && settings.getInt(L"Glass", L"Enabled", 1) == 0, L"G15",
              L"glass off: the window is not layered, the theme's backgrounds, renderers and text smoothing are back, the catcher is gone");
        if (views.sci(1)) sci(views.sci(1), SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DEFAULT);
        return 200;
    });
    // ---- live token styles: Style All Occurrences of Token that follows the text, bold/italic, menus -----
    add([] {
        writeFile(path(L"k.cpp"), kTokCpp);
        ::DeleteFileW(tokenStoreFile().c_str());
        views.activate(0, 0);
        pump(100);
        open(L"k.cpp");
        focusEditor();
        pump(300);
        HWND s = curSci();
        TokenStats ts = tokenStats();
        check(modules[M_TOKENS]->isEnabled() && ts.menuKeyOn && tokenDocWrapped(s) && ts.wrapperCalls > 0, L"L1",
              L"live token styles on by default; k.cpp's lexer goes through the suite (%d lexers wrapped, %d alive)", ts.wrapperCalls, ts.liveWrappers);
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        caretOn(s, al[0]);
        tokCommand(IDM_SEARCH_MARKALLEXT1);   // Search > Style All Occurrences of Token > Using 1st Style
        settle();
        check(al.size() == 4 && allOurs(s, 25, al, 5) == 4 && tokRuns(s, 25) == 4 && tokenRuleCount(s) == 1, L"L2",
              L"Style All Occurrences > 1st style on \"alpha\": %d of %d whole-word occurrences styled live (not \"alphabet\"), %d runs",
              allOurs(s, 25, al, 5), static_cast<int>(al.size()), tokRuns(s, 25));
        return 200;
    });
    add([] {
        HWND s = curSci();
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("int delta = alpha * 2;\r\n"));
        pump(300);   // painting lexes the new line
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        check(al.size() == 5 && tokOurs(s, 25, al.back(), 5), L"L3", L"an \"alpha\" typed later is styled too (%d of %d)",
              allOurs(s, 25, al, 5), static_cast<int>(al.size()));
        // edit the first one into "alphx": it loses the style
        intptr_t a = al[0];
        sci(s, SCI_SETTARGETRANGE, a + 4, a + 5);
        sci(s, SCI_REPLACETARGET, 1, reinterpret_cast<LPARAM>("x"));
        settle();
        check(!tokOurs(s, 25, a, 4) && sci(s, SCI_INDICATORVALUEAT, 25, a) == 0 && tokRuns(s, 25) == 4, L"L4",
              L"an occurrence edited into \"alphx\" loses the style; the other %d keep it", tokRuns(s, 25));
        sci(s, SCI_SETTARGETRANGE, a + 4, a + 5);
        sci(s, SCI_REPLACETARGET, 1, reinterpret_cast<LPARAM>("a"));
        settle();
        g_tokBase = stylesOf(s);
        return 100;
    });
    add([] {
        HWND s = curSci();
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        caretOn(s, al[1]);
        tokCommand(static_cast<UINT>(tokenCommandId(0)));   // Bold
        settle(400);
        const intptr_t code = al[1];
        const int st = styleAt(s, code);
        int f = 0;
        const int base = tokenVariantBase(s, st, &f);
        check(base == SCE_C_IDENTIFIER && f == 1 && sci(s, SCI_STYLEGETWEIGHT, st) >= SC_WEIGHT_BOLD &&
              sci(s, SCI_STYLEGETFORE, st) == sci(s, SCI_STYLEGETFORE, SCE_C_IDENTIFIER) && sci(s, SCI_STYLEGETSIZEFRACTIONAL, st) == sci(s, SCI_STYLEGETSIZEFRACTIONAL, SCE_C_IDENTIFIER),
              L"L5", L"Bold: \"alpha\" in code is drawn with style %d, a bold copy of the identifier style %d (weight %d)", st, base, static_cast<int>(sci(s, SCI_STYLEGETWEIGHT, st)));
        std::wstring why;
        const bool all = styledAs(s, al, 5, 1, -1, &why);
        int fc = 0, fb = 0;
        const int bc = tokenVariantBase(s, styleAt(s, al[2]), &fc);
        const int bb = tokenVariantBase(s, styleAt(s, al[3]), &fb);
        // apart from the swapped styles, the lexer's output is exactly what it was
        std::vector<int> now = stylesOf(s);
        int diff = -1;
        for (size_t i = 0; i < now.size() && i < g_tokBase.size(); ++i)
        {
            const int b = tokenVariantBase(s, now[i], nullptr);
            if ((b >= 0 ? b : now[i]) != g_tokBase[i]) { diff = static_cast<int>(i); break; }
        }
        check(all && bc == SCE_C_COMMENTLINE && bb == SCE_C_COMMENT && now.size() == g_tokBase.size() && diff < 0,
              L"L6", L"all 5 are bold, each on its own lexer style (comment line %d, block comment %d); every other character keeps the lexer's style (first difference: %d) %ls", bc, bb, diff, why.c_str());
        return 200;
    });
    add([] { return shot(L"tokens-live"); });
    add([] {
        HWND s = curSci();
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("// more alpha here\r\n"));
        settle(400);
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        std::wstring why;
        check(al.size() == 6 && styledAs(s, std::vector<intptr_t>{ al.back() }, 5, 1, SCE_C_COMMENTLINE, &why) && tokOurs(s, 25, al.back(), 5), L"L7",
              L"a new \"alpha\" typed in a comment comes out bold and coloured straight away (%d found) %ls", static_cast<int>(al.size()), why.c_str());
        caretOn(s, al[1]);
        tokCommand(static_cast<UINT>(tokenCommandId(1)));   // Italic
        settle(400);
        const int st = styleAt(s, al[1]);
        check(styledAs(s, al, 5, 3, -1, &why) && sci(s, SCI_STYLEGETITALIC, st) && sci(s, SCI_STYLEGETWEIGHT, st) >= SC_WEIGHT_BOLD, L"L8",
              L"Italic too: style %d is bold italic %ls", st, why.c_str());
        ::SendMessageW(npp._nppHandle, WM_UPDATESCINTILLAS_, FALSE, 0);   // Style Configurator's apply
        settle(400);
        const int st2 = styleAt(s, al[1]);
        check(sci(s, SCI_STYLEGETITALIC, st2) && sci(s, SCI_STYLEGETWEIGHT, st2) >= SC_WEIGHT_BOLD && tokOurs(s, 25, al[1], 5), L"L9",
              L"after Notepad++ applies the theme again, the bold italic and the colour are still there (style %d)", st2);
        nppMsg(NPPM_SETCURRENTLANGTYPE, 0, L_PYTHON);
        settle(300);
        nppMsg(NPPM_SETCURRENTLANGTYPE, 0, L_CPP);
        settle(400);
        al = wordsIn(s, "alpha");
        const size_t vp = docText(s).find("void");
        const int vs = styleAt(s, static_cast<intptr_t>(vp)), vb = vp < g_tokBase.size() ? g_tokBase[vp] : -1;
        check(styledAs(s, al, 5, 3, -1, &why) && vs == vb && vs != 0, L"L10",
              L"switching the language to Python and back keeps them bold italic, and C++ colouring is back (\"void\" style %d, was %d) %ls (%d variants made, %d lexer collisions)",
              vs, vb, why.c_str(), tokenStats().variantsMade, tokenStats().collisions);
        return 200;
    });
    add([] {
        HWND s = curSci();
        caretOn(s, wordsIn(s, "beta")[0]);
        focusEditor();
        pump(100);
        chord({ VK_CONTROL, VK_SHIFT }, 'H');
        return 600;
    });
    add([] {
        HWND menu = ::FindWindowW(L"#32768", nullptr);
        check(menu && ::IsWindowVisible(menu) && tokenStats().menus >= 1, L"L11", L"Ctrl+Shift+H in the editor pops the token menu up at the caret");
        return shot(L"tokens-menu");
    });
    add([] {
        tap('2');     // "&2nd style"
        return 500;
    });
    add([] {
        HWND s = curSci();
        std::vector<intptr_t> be = wordsIn(s, "beta");
        check(!::FindWindowW(L"#32768", nullptr) && be.size() == 1 && tokOurs(s, 24, be[0], 4) && tokenRuleCount(s) == 2, L"L12",
              L"picking \"2nd style\" there styles \"beta\" with Mark Style 2 (%d tokens styled now)", tokenRuleCount(s));
        caretOn(s, be[0]);
        ::PostMessageW(npp._nppHandle, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(s), MAKELPARAM(-1, -1));
        return 500;
    });
    add([] {
        HWND menu = ::FindWindowW(L"#32768", nullptr);
        HMENU top = menu ? reinterpret_cast<HMENU>(::SendMessageW(menu, MN_GETHMENU, 0, 0)) : nullptr;
        HMENU sub = subWith(top, IDM_SEARCH_MARKALLEXT1);
        if (sub) ::SendMessageW(npp._nppHandle, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), 0);   // as Windows does when it opens
        const bool bold = menuHasId(sub, static_cast<UINT>(tokenCommandId(0))) && menuHasId(sub, static_cast<UINT>(tokenCommandId(1))) && menuHasId(sub, static_cast<UINT>(tokenCommandId(2)));
        const bool checked = sub && (::GetMenuState(sub, IDM_SEARCH_MARKALLEXT2, MF_BYCOMMAND) & MF_CHECKED) && !(::GetMenuState(sub, IDM_SEARCH_MARKALLEXT1, MF_BYCOMMAND) & MF_CHECKED);
        ::EndMenu();
        check(menu && sub && bold && checked, L"L13", L"right-click > Style All Occurrences of Token has Bold, Italic and Clear This Token, with \"Using 2nd Style\" ticked for \"beta\"");
        HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
        HMENU msub = subWith(mm, IDM_SEARCH_MARKALLEXT1);
        if (msub) ::SendMessageW(npp._nppHandle, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(msub), 0);
        check(menuHasId(msub, static_cast<UINT>(tokenCommandId(0))), L"L14", L"and so does Search > Style All Occurrences of Token");
        return 300;
    });
    add([] {
        HWND s = curSci();
        std::vector<intptr_t> al = wordsIn(s, "alpha");
        caretOn(s, al[2] + 2);
        tokCommand(static_cast<UINT>(tokenCommandId(2)));   // Clear This Token
        settle(400);
        al = wordsIn(s, "alpha");
        std::wstring why;
        check(tokRuns(s, 25) == 0 && styledAs(s, al, 5, 0, -1, &why) && tokenRuleCount(s) == 1 && tokOurs(s, 24, wordsIn(s, "beta")[0], 4), L"L15",
              L"Clear This Token takes the colour, bold and italic off every \"alpha\" and leaves \"beta\" %ls", why.c_str());
        // remembered per file: close k.cpp and open it again
        FILE* f = _wfopen(tokenStoreFile().c_str(), L"rb");
        char buf[2048] = {}; size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0; if (f) fclose(f);
        std::string stored(buf, n);
        check(stored.find("k.cpp") != std::string::npos && stored.find("\tbeta") != std::string::npos && stored.find("\talpha") == std::string::npos, L"L16",
              L"kex-tokens.txt remembers \"beta\" for k.cpp (%d bytes)", static_cast<int>(n));
        caretOn(s, wordsIn(s, "gamma")[0]);
        tokCommand(static_cast<UINT>(tokenCommandId(0)));   // gamma: bold, no colour
        settle(300);
        nppMsg(NPPM_SAVECURRENTFILE);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(300);
        open(L"k.cpp");
        settle(500);
        return 200;
    });
    add([] {
        HWND s = curSci();
        std::wstring why;
        const std::vector<intptr_t> ga = wordsIn(s, "gamma");
        check(curName() == L"k.cpp" && tokenRuleCount(s) == 2 && tokOurs(s, 24, wordsIn(s, "beta")[0], 4) && styledAs(s, ga, 5, 1, -1, &why), L"L17",
              L"closed and opened again, k.cpp has its styled tokens back: \"beta\" coloured, \"gamma\" bold %ls %ls", why.c_str(), tokDiag(s).c_str());
        openPreferences();
        pump(300);
        showPrefPage(L"KEX Highlighting");
        pump(200);
        HWND p = prefPage(PAGE_HIGHLIGHT);
        const std::wstring st = itemText(p, IDC_K_STATUS), info = itemText(p, IDC_K_INFO), key = itemText(p, IDC_K_KEY);
        wchar_t item[128] = {};
        ::GetMenuStringW(settingsSubmenu(PAGE_HIGHLIGHT), static_cast<UINT>(tokenMenuCommandId()), item, 128, MF_BYCOMMAND);
        check(p && ::IsWindowVisible(p) && st == L"Running" && key == L"Ctrl+Shift+H" && std::wstring(item) == L"Token Style Menu\tCtrl+Shift+H" && info.find(L"2 tokens") != std::wstring::npos,
              L"L18", L"KEX Highlighting page: \"%ls\", menu key %ls (Settings > KEX Highlighting > \"%ls\"), \"%ls\"", st.c_str(), key.c_str(), item, info.c_str());
        return 200;
    });
    add([] { return shot(L"preferences-kex-highlighting"); });
    add([] {
        HWND s = curSci();
        clickEnable(M_TOKENS);   // off
        pump(300);
        settle(400);
        std::wstring why;
        const bool plain = styledAs(s, wordsIn(s, "gamma"), 5, 0, -1, &why);
        check(!modules[M_TOKENS]->isEnabled() && !tokenStats().menuKeyOn && tokRuns(s, 24) == 0 && plain && settings.getInt(L"TokenStyles", L"Enabled", 1) == 0, L"L19",
              L"switched off: the colours and the bold are gone %ls", why.c_str());
        clickEnable(M_TOKENS);   // on again: the file's tokens come back from kex-tokens.txt
        pump(300);
        ::SendMessageW(prefDialog(), WM_COMMAND, IDCANCEL, 0);
        settle(400);
        check(modules[M_TOKENS]->isEnabled() && tokOurs(s, 24, wordsIn(s, "beta")[0], 4) && styledAs(s, wordsIn(s, "gamma"), 5, 1, -1, &why), L"L20",
              L"and on again: k.cpp's tokens are back %ls %ls", why.c_str(), tokDiag(s).c_str());
        tokCommand(IDM_SEARCH_CLEARALLMARKS);   // Search > Clear Style > Clear all Styles
        settle(400);
        FILE* f = _wfopen(tokenStoreFile().c_str(), L"rb");
        char buf[2048] = {}; size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0; if (f) fclose(f);
        check(tokenRuleCount(s) == 0 && tokRuns(s, 24) == 0 && styledAs(s, wordsIn(s, "gamma"), 5, 0, -1, &why) && std::string(buf, n).find("k.cpp") == std::string::npos,
              L"L21", L"Clear all Styles clears the live tokens too, and k.cpp is forgotten %ls", why.c_str());
        focusEditor();
        return 200;
    });
    add([] {
        // plain text: its null lexer styles only the last character of a range, the rest needs the suite
        writeFile(path(L"t.txt"), "plain words here\r\nmore words: plain and plainer plain\r\n");
        open(L"t.txt");
        settle(300);
        HWND s = curSci();
        caretOn(s, wordsIn(s, "plain")[0]);
        tokCommand(IDM_SEARCH_MARKALLEXT3);
        settle();
        tokCommand(static_cast<UINT>(tokenCommandId(0)));   // Bold
        settle(400);
        sci(s, SCI_DOCUMENTEND);
        sci(s, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("typed plain\r\n"));
        settle(400);
        std::wstring why;
        const std::vector<intptr_t> pl = wordsIn(s, "plain");
        const int st = pl.empty() ? -1 : styleAt(s, pl[0]);
        check(curName() == L"t.txt" && pl.size() == 4 && allOurs(s, 23, pl, 5) == 4 && styledAs(s, pl, 5, 1, -1, &why) && styledAs(s, wordsIn(s, "words"), 5, 0, -1, nullptr)
              && sci(s, SCI_STYLEGETWEIGHT, st) >= SC_WEIGHT_BOLD, L"L22",
              L"plain .txt: \"plain\" is coloured and bold, the one typed later too, \"plainer\" and the other words stay plain (style %d) %ls", st, why.c_str());
        return 100;
    });
    add([] {
        HWND s = curSci();
        caretOn(s, wordsIn(s, "plain")[1]);
        tokCommand(static_cast<UINT>(tokenCommandId(1)));   // Italic on
        settle(300);
        tokCommand(static_cast<UINT>(tokenCommandId(0)));   // Bold off
        settle(400);
        std::wstring why;
        const std::vector<intptr_t> pl = wordsIn(s, "plain");
        const bool italicOnly = styledAs(s, pl, 5, 2, -1, &why);
        tokCommand(static_cast<UINT>(tokenCommandId(2)));   // Clear This Token
        settle(400);
        std::wstring why2;
        const bool cleared = styledAs(s, pl, 5, 0, -1, &why2) && tokRuns(s, 23) == 0;
        check(italicOnly && cleared, L"L23", L"plain .txt: Bold off leaves it italic, Clear This Token makes it plain again %ls %ls", why.c_str(), why2.c_str());
        nppMsg(NPPM_SAVECURRENTFILE);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(200);
        focusEditor();
        return 200;
    });
    add([] {
        // Style One Token > Bold / Italic: only that occurrence
        writeFile(path(L"o.txt"), "one two one three one\r\nmore one here\r\n");
        open(L"o.txt");
        settle(300);
        HWND s = curSci();
        const std::vector<intptr_t> on = wordsIn(s, "one");
        if (on.size() != 4) { check(false, L"L24", L"o.txt has %d \"one\"", static_cast<int>(on.size())); return 100; }
        sci(s, SCI_SETSEL, on[1], on[1] + 3);
        tokCommand(IDM_SEARCH_MARKONEEXT2);   // Search > Style One Token > Using 2nd Style
        settle();
        tokCommand(static_cast<UINT>(tokenCommandId(3)));   // Style One Token > Bold
        settle(400);
        std::wstring why, why2;
        const bool bold = styledAs(s, { on[1] }, 3, 1, -1, &why) && tokenOneOffAt(s) == 1;
        const bool others = styledAs(s, { on[0], on[2], on[3] }, 3, 0, -1, &why2);
        const bool colour = sci(s, SCI_INDICATORVALUEAT, 24, on[1]) != 0 && sci(s, SCI_INDICATORVALUEAT, 24, on[0]) == 0;
        check(bold && others && colour, L"L24", L"Style One Token > Bold: only the second \"one\" is coloured and bold, the other three stay plain %ls %ls",
              why.c_str(), why2.c_str());
        tokCommand(static_cast<UINT>(tokenCommandId(4)));   // Style One Token > Italic
        settle(400);
        std::wstring why3;
        check(styledAs(s, { on[1] }, 3, 3, -1, &why3) && tokenOneOffAt(s) == 3 && styledAs(s, { on[0], on[2], on[3] }, 3, 0, -1, nullptr), L"L25",
              L"and Italic: that one is bold and italic %ls", why3.c_str());
        // a range that has no colour can be bold too
        sci(s, SCI_SETSEL, on[3], on[3] + 3);
        tokCommand(static_cast<UINT>(tokenCommandId(3)));
        settle(400);
        std::wstring why4;
        check(styledAs(s, { on[3] }, 3, 1, -1, &why4) && styledAs(s, { on[0], on[2] }, 3, 0, -1, nullptr), L"L26",
              L"bold on an uncoloured occurrence: only it %ls", why4.c_str());
        sci(s, SCI_SETEMPTYSELECTION, 0);
        nppMsg(NPPM_SAVECURRENTFILE);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(200);
        return 100;
    });
    add([] {
        const std::string store = readAll(tokenStoreFile());
        open(L"o.txt");
        settle(400);
        HWND s = curSci();
        const std::vector<intptr_t> on = wordsIn(s, "one");
        std::wstring why;
        const bool back = on.size() == 4 && styledAs(s, { on[1] }, 3, 3, -1, &why) && styledAs(s, { on[3] }, 3, 1, -1, nullptr)
                          && styledAs(s, { on[0], on[2] }, 3, 0, -1, nullptr) && sci(s, SCI_INDICATORVALUEAT, 24, on[1]) != 0;
        check(back && store.find("S\tO\t3\t") != std::string::npos, L"L27",
              L"closed and opened again: the single bold/italic ranges and the colour come back (kept in %ls) %ls", fileNameOf(tokenStoreFile()).c_str(), why.c_str());
        tokCommand(IDM_SEARCH_CLEARALLMARKS);   // Search > Clear Style > Clear all Styles
        settle(400);
        check(on.size() == 4 && styledAs(s, on, 3, 0, -1, nullptr) && sci(s, SCI_INDICATORVALUEAT, 24, on[1]) == 0, L"L28",
              L"Clear all Styles takes the single bold/italic ranges away too");
        nppMsg(NPPM_SAVECURRENTFILE);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(200);
        focusEditor();
        return 100;
    });
    // ---- speed: what the features cost while typing and while idle --------------------------------
    add([] {
        std::string text;
        for (int i = 1; i <= 3000; ++i)
            text += "static int value" + std::to_string(i) + " = " + std::to_string(i * 7) + ";   // line " + std::to_string(i) + " of the speed test\r\n";
        writeFile(path(L"perf.cpp"), text);
        open(L"perf.cpp");
        focusEditor();
        pump(300);
        bool was[M_COUNT];
        for (int i = 0; i < M_COUNT; ++i) was[i] = modules[i]->isEnabled();
        const Perf on = measurePerf();
        for (int i = 0; i < M_COUNT; ++i) if (was[i]) setModuleEnabled(i, false, false);
        focusEditor();
        const Perf off = measurePerf();
        tlog(L"INFO PERF all KEX features on: %.3f ms per key, %.2f%% CPU idle; all off: %.3f ms per key, %.2f%% CPU idle",
             on.keyMs, on.idleCpu, off.keyMs, off.idleCpu);
        tlog(L"INFO PERF smooth caret: drawn in %.2f paints per key while typing, %.1f a second while idle (glide and blink fades)",
             on.caretRedraws, on.idleRedraws);
        tlog(L"INFO PERF text repaints per key: %.2f with the features on, %.2f with them off", on.paints, off.paints);
        tlog(L"INFO PERF smooth caret's own code in Scintilla per key: %.3f ms", on.caretWork);
        for (int i = 0; i < M_COUNT; ++i)
        {
            if (!was[i]) continue;
            setModuleEnabled(i, true, false);
            focusEditor();
            const Perf one = measurePerf();
            tlog(L"INFO PERF only %-34ls %.3f ms per key (%+.3f), %.2f%% CPU idle (%+.2f)", modules[i]->title(),
                 one.keyMs, one.keyMs - off.keyMs, one.idleCpu, one.idleCpu - off.idleCpu);
            setModuleEnabled(i, false, false);
        }
        for (int i = 0; i < M_COUNT; ++i) if (was[i]) setModuleEnabled(i, true, false);
        // The features' own work; the caret's extra paints are capped by its glide (80 ms at one frame
        // per 15 ms) and blink fade.
        const double ownKey = on.keyMs - off.keyMs;
        const double ownIdle = on.idleCpu - off.idleCpu;
        check(ownKey <= off.keyMs * 0.5 + 0.3 && ownIdle <= 1.5 && on.caretRedraws <= 8 && on.idleRedraws <= 25, L"PERF1",
              L"the features' own work adds %.3f ms per key and %.2f%% CPU while idle; caret redraws %.2f per key, %.1f a second idle (total %.3f vs %.3f ms per key, %.2f%% vs %.2f%% idle)",
              ownKey, ownIdle, on.caretRedraws, on.idleRedraws, on.keyMs, off.keyMs, on.idleCpu, off.idleCpu);
        nppMsg(NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
        pump(200);
        return 100;
    });
    add([] {
        if (g_other.hProcess) { ::TerminateProcess(g_other.hProcess, 0); ::CloseHandle(g_other.hProcess); ::CloseHandle(g_other.hThread); }
        tlog(L"RESULT %d passed, %d failed", g_pass, g_fail);
        tlog(L"DONE");
        g_done = true;
        return 0;
    });
}

void CALLBACK tick(HWND, UINT, UINT_PTR, DWORD)
{
    if (g_inStep || g_done) return;
    if (g_waitAck)
    {
        wchar_t ack[MAX_PATH]; swprintf(ack, MAX_PATH, L"%ls\\ack%d", g_dir.c_str(), g_waitAck);
        if (!fileExists(ack)) return;
        g_waitAck = 0;
    }
    if (g_until)
    {
        bool ok = false;
        try { ok = g_until(); } catch (...) {}
        if (!ok && nowMs() < g_untilDeadline) return;
        g_untilOk = ok;
        g_until = nullptr;
    }
    if (nowMs() < g_nextAt || g_idx >= g_steps.size()) return;
    g_inStep = true;
    int d = 0;
    size_t idx = g_idx++;
    try { d = g_steps[idx](); }
    catch (...) { tlog(L"FAIL X step %u threw an exception", static_cast<unsigned>(idx)); ++g_fail; }
    g_inStep = false;
    g_nextAt = nowMs() + d;
}

} // namespace

FILE* selfTestLogFile() { return g_tlog; }

void selfTestStart(const std::wstring& args)
{
    size_t p = args.find(L"KexSuite_testdir=");
    if (p != std::wstring::npos)
    {
        size_t e = args.find(L';', p);
        g_dir = args.substr(p + 17, e == std::wstring::npos ? std::wstring::npos : e - p - 17);
    }
    if (g_dir.empty()) g_dir = settings.dir() + L"\\KexSuite-selftest";
    ::CreateDirectoryW(g_dir.c_str(), nullptr);
    g_tlog = _wfopen((g_dir + L"\\selftest-log.txt").c_str(), L"w, ccs=UTF-8");
    tlog(L"KEX Suite self-test (plugin message: %ls)", args.c_str());
    WNDCLASSW wc{}; wc.lpfnWndProc = testWndProc; wc.hInstance = hInst; wc.lpszClassName = L"KexSuiteTestWnd";
    ::RegisterClassW(&wc);
    g_testWnd = ::CreateWindowExW(0, L"KexSuiteTestWnd", L"test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    buildSteps();
    g_nextAt = nowMs() + 800;
    ::SetTimer(nullptr, 0, 40, tick);
}

void selfTestOnNotify(SCNotification* n)
{
    if (n->nmhdr.code == NPPN_GLOBALMODIFIED) ++g_globalModified;
}

} // namespace kex
