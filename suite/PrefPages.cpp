// PrefPages.cpp - the KEX Settings window (KEX > Settings...): KEX's own settings, apart from Notepad++'s
// Preferences but built the same way: the pages "Editing", "Tabs", "Windows", "Highlighting", "Quick Note"
// and "Effects" in a list on the left, the picked page on the right, a Close button, Notepad++'s dark mode.
// Like the Preferences pages, every change takes effect at once; numbers apply half a second after the
// last keystroke (or when the window closes).
#include "KexSuite.h"
#include "KexSuiteRes.h"
#include <cwchar>
#include <commdlg.h>

namespace kex {

namespace {

const UINT_PTR TIMER_APPLY = 1;
HWND g_pages[PAGE_COUNT] = {};
HWND g_dialog = nullptr;
int g_page = 0;                 // the page showing (or shown last)
const wchar_t* const kPageTitle[PAGE_COUNT] = { L"Editing", L"Tabs", L"Windows", L"Highlighting", L"Quick Note", L"Effects" };
bool g_filling = false;

struct IntField { int id; int module; const wchar_t* key; int def; };
const IntField kInts[] = {
    { IDC_L_GRAVITY, M_LETTERS, L"Gravity", 2200 },
    { IDC_L_MAX, M_LETTERS, L"MaxLetters", 200 },
    { IDC_L_SPIN, M_LETTERS, L"SpinDegPerSec", 260 },
    { IDC_L_DRIFT, M_LETTERS, L"DriftPxPerSec", 90 },
    { IDC_L_OPACITY, M_LETTERS, L"Opacity", 100 },
    { IDC_C_GLIDE, M_CARET, L"GlideMs", 80 },
    { IDC_C_SNAP, M_CARET, L"SnapAfterLines", 12 },
    { IDC_T_ZONE, M_TABS, L"HotZonePx", 6 },
    { IDC_T_DELAY, M_TABS, L"HideDelayMs", 350 },
    { IDC_G_TINT, M_GLASS, L"Tint", 40 },
    { IDC_H_OPACITY, M_HOTKEY, L"Opacity", 100 },
    { IDC_E_STRENGTH, M_FLASH, L"Strength", 100 },
    { IDC_W_MS, M_SLIDE, L"DurationMs", 200 },
    { IDC_P_DROPS, M_SPLASH, L"DropsPerToken", 7 },
    { IDC_SW_SENS, M_SWIPE, L"Sensitivity", 100 },
    { IDC_F_MS, M_TABFOLD, L"DurationMs", 180 },
};
struct BoolField { int id; int module; const wchar_t* key; int def; };
const BoolField kBools[] = {
    { IDC_L_RESPECT, M_LETTERS, L"RespectSystemAnimationSetting", 1 },
    { IDC_L_UNDO, M_LETTERS, L"AnimateUndoRedo", 0 },
    { IDC_C_OVB, M_CARET, L"OvertypeBlock", 1 },
    { IDC_H_KEEP, M_HOTKEY, L"KeepText", 1 },
    { IDC_H_FADE, M_HOTKEY, L"Fade", 1 },
    { IDC_H_LINES, M_HOTKEY, L"LineNumbers", 0 },
    { IDC_H_WRAP, M_HOTKEY, L"WordWrap", 1 },
    { IDC_H_CARETLINE, M_HOTKEY, L"CurrentLine", 0 },
    { IDC_H_ONTOP, M_HOTKEY, L"OnTop", 1 },
    { IDC_H_PLACE, M_HOTKEY, L"RememberPlace", 0 },
    { IDC_S_ANIM, M_SNAP, L"Animate", 1 },
    { IDC_S_LIFT, M_SNAP, L"LiftTab", 1 },
    { IDC_J_ALL, M_KEYS, L"AlsoNotepadKeys", 1 },
    { IDC_G_MARGINS, M_GLASS, L"Margins", 1 },
    { IDC_G_SMOOTHMOVE, M_GLASS, L"SmoothMove", 1 },
    { IDC_K_REMEMBER, M_TOKENS, L"Remember", 1 },
    { IDC_H_AUTOHIDE, M_HOTKEY, L"HideWhenInactive", 0 },
    { IDC_H_CARET, M_HOTKEY, L"SmoothCaret", 1 },
    { IDC_H_LETTERS, M_LETTERS, L"InQuickNote", 1 },
    { IDC_H_FLASH, M_FLASH, L"InQuickNote", 1 },
    { IDC_E_COPY, M_FLASH, L"Copy", 1 },
    { IDC_E_CUT, M_FLASH, L"CutLift", 1 },
    { IDC_E_DEL, M_FLASH, L"DeleteFade", 1 },
    { IDC_SW_REVERSE, M_SWIPE, L"Reverse", 0 },
};
// one box on the Effects page for all of its animations: "Not when Windows animation effects are off"
const int kRespectModules[] = { M_FLASH, M_SLIDE, M_SPLASH, M_TABFOLD, M_GLINT };
// options the feature reads again at once: no restart (which would close the quick note, or take the token styles away)
bool liveOption(int id, int module)
{
    return module == M_TOKENS || id == IDC_H_AUTOHIDE || id == IDC_H_CARET || id == IDC_H_OPACITY ||
           id == IDC_E_COPY || id == IDC_E_CUT || id == IDC_E_DEL || id == IDC_E_STRENGTH ||
           id == IDC_W_MS || id == IDC_P_DROPS || id == IDC_C_BLINK || module == M_SWIPE || module == M_TABFOLD || module == M_GLINT;
}
// a drop-down list: the setting is the index of the picked item
struct ComboField { int id; int module; const wchar_t* key; int def; const wchar_t* items; };
const ComboField kCombos[] = {
    { IDC_C_BLINK, M_CARET, L"Blink", 0, L"Fade in and out|On and off, like Notepad++|Never" },
    { IDC_J_CASE, M_KEYS, L"OccurrenceCase", 0, L"Like Smart Highlighting|Match case|Ignore case" },
    { IDC_J_WORDS, M_KEYS, L"OccurrenceWords", 0, L"Like Smart Highlighting|From a bare caret (IntelliJ)|Always|Never" },
    { IDC_G_STYLE, M_GLASS, L"Style", 0, L"Blur|Acrylic" },
    { IDC_G_EDGES, M_GLASS, L"Edges", 0, L"Smooth|Sharp" },
};
const int kEnable[M_COUNT] = { IDC_L_EN, IDC_C_EN, IDC_H_EN, IDC_T_EN, IDC_D_EN, IDC_S_EN, IDC_J_EN, IDC_G_EN, IDC_K_EN, IDC_E_EN, IDC_W_EN, IDC_P_EN, IDC_SW_EN,
                              IDC_F_EN, IDC_GL_EN };
const int kStatus[M_COUNT] = { IDC_L_STATUS, IDC_C_STATUS, IDC_H_STATUS, IDC_T_STATUS, IDC_D_STATUS, IDC_S_STATUS, IDC_J_STATUS, IDC_G_STATUS, IDC_K_STATUS,
                               IDC_E_STATUS, IDC_W_STATUS, IDC_P_STATUS, IDC_SW_STATUS, IDC_F_STATUS, IDC_GL_STATUS };

bool has(HWND d, int id) { return ::GetDlgItem(d, id) != nullptr; }

void fillStatus(HWND d)
{
    int ok = 0;
    for (int i = 0; i < M_COUNT; ++i)
    {
        Module* m = modules[i];
        if (m->_checkOk) ++ok;
        if (!has(d, kStatus[i])) continue;
        std::wstring s;
        if (!m->_checkOk) s = L"Unavailable: " + m->_checkMsg;
        else s = m->isEnabled() ? L"Running" : L"Off";
        ::SetDlgItemTextW(d, kStatus[i], s.c_str());
    }
    wchar_t b[96]; swprintf(b, 96, L"%d of %d KEX features passed their start-up self-check.", ok, M_COUNT);
    ::SetDlgItemTextW(d, IDC_SUMMARY, b);

    if (has(d, IDC_H_KEYSTATUS))
    {
        HotkeyStats hs = hotkeyStats();
        std::wstring ks;
        if (modules[M_HOTKEY]->isEnabled())
        {
            if (hs.registered) ks = L"works from any program";
            else if (hs.byOtherNpp) ks = L"handled by the first Notepad++ window, on every desktop";
            else
            {
                wchar_t e[160];
                swprintf(e, 160, L"taken by another program (error %lu)", hs.lastError);
                ks = e;
            }
        }
        ::SetDlgItemTextW(d, IDC_H_KEYSTATUS, ks.c_str());
    }
    if (has(d, IDC_D_INFO))
    {
        DesktopStats ds = desktopStats();
        std::wstring info;
        if (modules[M_DESKTOP]->isEnabled())
        {
            if (ds.newWindow)
                info = L"This window opened on its own desktop with " + std::to_wstring(std::max(0, ds.copiedTabs)) + L" tabs from the first window.";
            else
                info = L"Start Notepad++ on another desktop (taskbar, Start, a file) to open a window there with these tabs.";
        }
        ::SetDlgItemTextW(d, IDC_D_INFO, info.c_str());
    }
    if (has(d, IDC_G_INFO)) ::SetDlgItemTextW(d, IDC_G_INFO, glassInfo().c_str());
    if (has(d, IDC_E_INFO)) ::SetDlgItemTextW(d, IDC_E_INFO, effectsInfo().c_str());
    if (has(d, IDC_H_FILEINFO))
    {
        std::wstring info;
        const std::wstring err = quickNoteFileError();
        if (settings.getStr(modules[M_HOTKEY]->key(), L"NoteFile").empty())
            info = L"Empty: the note keeps its text in kex-quicknote.txt in Notepad++'s settings folder.";
        else if (!err.empty()) info = L"Not used: " + err + L". The note shows nothing from it and won't write to it.";
        else info = L"The note shows this file and saves to it as you type, in its own encoding and line endings.";
        ::SetDlgItemTextW(d, IDC_H_FILEINFO, info.c_str());
    }
    if (has(d, IDC_K_INFO))
    {
        ::SetDlgItemTextW(d, IDC_K_INFO, modules[M_TOKENS]->isEnabled() ? tokenInfo().c_str() : L"");
        const std::wstring key = tokenStats().menuKey;
        ::SetDlgItemTextW(d, IDC_K_KEY, key.empty() ? L"none" : key.c_str());
    }
}

void fill(HWND d)
{
    g_filling = true;
    for (int i = 0; i < M_COUNT; ++i)
        if (has(d, kEnable[i])) ::CheckDlgButton(d, kEnable[i], modules[i]->isEnabled() ? BST_CHECKED : BST_UNCHECKED);
    for (const IntField& f : kInts)
        if (has(d, f.id)) ::SetDlgItemInt(d, f.id, static_cast<UINT>(settings.getInt(modules[f.module]->key(), f.key, f.def)), FALSE);
    for (const BoolField& f : kBools)
        if (has(d, f.id)) ::CheckDlgButton(d, f.id, settings.getInt(modules[f.module]->key(), f.key, f.def) ? BST_CHECKED : BST_UNCHECKED);
    if (has(d, IDC_FX_RESPECT))
        ::CheckDlgButton(d, IDC_FX_RESPECT, settings.getInt(modules[M_FLASH]->key(), L"RespectSystemAnimationSetting", 1) ? BST_CHECKED : BST_UNCHECKED);
    for (const ComboField& f : kCombos)
    {
        if (!has(d, f.id)) continue;
        HWND cb = ::GetDlgItem(d, f.id);
        if (::SendMessageW(cb, CB_GETCOUNT, 0, 0) <= 0)
        {
            std::wstring items = f.items;
            for (size_t a = 0; a <= items.size();)
            {
                size_t e = items.find(L'|', a);
                if (e == std::wstring::npos) e = items.size();
                ::SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items.substr(a, e - a).c_str()));
                a = e + 1;
            }
        }
        ::SendMessageW(cb, CB_SETCURSEL, static_cast<WPARAM>(settings.getInt(modules[f.module]->key(), f.key, f.def)), 0);
    }
    if (has(d, IDC_H_FILE))
    {
        // only when it differs: setting the text puts the caret back at the start of the box
        wchar_t now[MAX_PATH * 2] = {};
        ::GetDlgItemTextW(d, IDC_H_FILE, now, MAX_PATH * 2);
        const std::wstring v = settings.getStr(modules[M_HOTKEY]->key(), L"NoteFile");
        if (v != now && ::GetFocus() != ::GetDlgItem(d, IDC_H_FILE)) ::SetDlgItemTextW(d, IDC_H_FILE, v.c_str());
    }
    if (has(d, IDC_H_KEY))
    {
        UINT mods = static_cast<UINT>(settings.getInt(modules[M_HOTKEY]->key(), L"Modifiers", MOD_CONTROL | MOD_ALT));
        UINT vk = static_cast<UINT>(settings.getInt(modules[M_HOTKEY]->key(), L"Key", 'M'));
        BYTE hk = 0;
        if (mods & MOD_CONTROL) hk |= HOTKEYF_CONTROL;
        if (mods & MOD_ALT) hk |= HOTKEYF_ALT;
        if (mods & MOD_SHIFT) hk |= HOTKEYF_SHIFT;
        ::SendDlgItemMessageW(d, IDC_H_KEY, HKM_SETHOTKEY, MAKEWORD(vk, hk), 0);
        ::CheckDlgButton(d, IDC_H_WIN, (mods & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED);
    }
    fillStatus(d);
    g_filling = false;
}

// A running feature reads its settings when it starts, so restart it to pick up a change.
void restart(int module)
{
    if (!modules[module]->isEnabled()) return;
    setModuleEnabled(module, false, false);
    setModuleEnabled(module, true, false);
}

void applyNumbers(HWND d)
{
    bool changed[M_COUNT] = {};
    for (const IntField& f : kInts)
    {
        if (!has(d, f.id)) continue;
        BOOL ok = FALSE;
        UINT v = ::GetDlgItemInt(d, f.id, &ok, FALSE);
        if (!ok || static_cast<int>(v) == settings.getInt(modules[f.module]->key(), f.key, f.def)) continue;
        settings.setInt(modules[f.module]->key(), f.key, static_cast<int>(v));
        if (liveOption(f.id, f.module)) modules[f.module]->loadOptions();
        else changed[f.module] = true;
    }
    if (has(d, IDC_H_FILE))
    {
        wchar_t buf[MAX_PATH * 2] = {};
        ::GetDlgItemTextW(d, IDC_H_FILE, buf, MAX_PATH * 2);
        std::wstring v = buf;
        while (!v.empty() && (v.back() == L' ' || v.back() == L'"')) v.pop_back();
        while (!v.empty() && (v.front() == L' ' || v.front() == L'"')) v.erase(0, 1);
        if (v != settings.getStr(modules[M_HOTKEY]->key(), L"NoteFile"))
        {
            settings.setStr(modules[M_HOTKEY]->key(), L"NoteFile", v);
            quickNoteOptionsChanged();
        }
    }
    for (int i = 0; i < M_COUNT; ++i) if (changed[i]) restart(i);
}

void applyHotkey(HWND d)
{
    WORD hk = static_cast<WORD>(::SendDlgItemMessageW(d, IDC_H_KEY, HKM_GETHOTKEY, 0, 0));
    UINT vk = LOBYTE(hk), flags = HIBYTE(hk), mods = 0;
    if (flags & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) mods |= MOD_ALT;
    if (flags & HOTKEYF_SHIFT) mods |= MOD_SHIFT;
    if (::IsDlgButtonChecked(d, IDC_H_WIN) == BST_CHECKED) mods |= MOD_WIN;
    if (vk && mods) hotkeyApply(mods, vk);
}

// a file for the note's text: an existing one, or a new name (made on the first save)
void browseNoteFile(HWND d)
{
    wchar_t path[MAX_PATH * 2] = {};
    ::GetDlgItemTextW(d, IDC_H_FILE, path, MAX_PATH * 2);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ::GetAncestor(d, GA_ROOT);
    ofn.lpstrFilter = L"Text files (*.txt;*.md;*.log)\0*.txt;*.md;*.log\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"Quick note file";
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_NOTESTFILECREATE;
    if (!::GetSaveFileNameW(&ofn)) return;
    ::SetDlgItemTextW(d, IDC_H_FILE, path);   // EN_CHANGE applies it
}

void onCommand(HWND d, int id, int code)
{
    if (g_filling) return;
    if (code == BN_CLICKED)
    {
        for (int i = 0; i < M_COUNT; ++i)
            if (id == kEnable[i])
            {
                bool want = ::IsDlgButtonChecked(d, id) == BST_CHECKED;
                setModuleEnabled(i, want, true);
                if (!want) settings.setInt(modules[i]->key(), L"Enabled", 0);
                refreshPrefPages();
                return;
            }
        for (const BoolField& f : kBools)
            if (id == f.id)
            {
                settings.setInt(modules[f.module]->key(), f.key, ::IsDlgButtonChecked(d, id) == BST_CHECKED ? 1 : 0);
                if (liveOption(f.id, f.module)) modules[f.module]->loadOptions();
                else restart(f.module);
                refreshPrefPages();
                return;
            }
        if (id == IDC_FX_RESPECT)
        {
            const int on = ::IsDlgButtonChecked(d, id) == BST_CHECKED ? 1 : 0;
            for (int m : kRespectModules)
            {
                settings.setInt(modules[m]->key(), L"RespectSystemAnimationSetting", on);
                modules[m]->loadOptions();
            }
            refreshPrefPages();
            return;
        }
        if (id == IDC_H_WIN) { applyHotkey(d); refreshPrefPages(); }
        if (id == IDC_H_BROWSE) { browseNoteFile(d); return; }
        if (id == IDC_J_LIST || id == IDC_K_KEYMAP)
        {
            // the keys of the KEX commands are Notepad++'s own shortcuts now
            nppMsg(NPPM_KEX_SHOWSHORTCUTS);
            modules[M_KEYS]->loadOptions();   // a key moved off or back onto one Notepad++ uses
            refreshPrefPages();
        }
        return;
    }
    if (code == CBN_SELCHANGE)
    {
        for (const ComboField& f : kCombos)
            if (id == f.id)
            {
                const LRESULT sel = ::SendDlgItemMessageW(d, id, CB_GETCURSEL, 0, 0);
                if (sel < 0) return;
                settings.setInt(modules[f.module]->key(), f.key, static_cast<int>(sel));
                if (f.module == M_GLASS) restart(f.module);   // it reads its settings when it starts
                else modules[f.module]->loadOptions();
                refreshPrefPages();
                return;
            }
        return;
    }
    if (code == EN_CHANGE)
    {
        if (id == IDC_H_KEY) { applyHotkey(d); fillStatus(d); return; }
        if (id == IDC_H_FILE) { ::SetTimer(d, TIMER_APPLY, 700, nullptr); return; }
        for (const IntField& f : kInts)
            if (id == f.id) { ::SetTimer(d, TIMER_APPLY, 500, nullptr); return; }
    }
}

INT_PTR CALLBACK pageProc(HWND d, UINT m, WPARAM w, LPARAM)
{
    if (!modules[0]) return FALSE;
    try
    {
        switch (m)
        {
        case WM_INITDIALOG:
            fill(d);
            return FALSE;
        case WM_SHOWWINDOW:
            if (w) fill(d);
            break;
        case WM_TIMER:
            if (w == TIMER_APPLY) { ::KillTimer(d, TIMER_APPLY); applyNumbers(d); refreshPrefPages(); return TRUE; }
            break;
        case WM_COMMAND:
            onCommand(d, LOWORD(w), HIWORD(w));
            return TRUE;
        case WM_DESTROY:
            ::KillTimer(d, TIMER_APPLY);
            break;
        }
    }
    catch (...) { log(L"exception on a KEX Settings page"); }
    return FALSE;
}

void showPage(int p)
{
    if (!g_dialog || p < 0 || p >= PAGE_COUNT || !g_pages[p]) return;
    g_page = p;
    ::SendDlgItemMessageW(g_dialog, IDC_KEX_PAGELIST, LB_SETCURSEL, static_cast<WPARAM>(p), 0);
    for (int i = 0; i < PAGE_COUNT; ++i)
        if (g_pages[i] && i != p) ::ShowWindow(g_pages[i], SW_HIDE);
    ::ShowWindow(g_pages[p], SW_SHOW);
}

// the window closes: what is still typed in a number box counts, as it would half a second later
void closeDialog()
{
    if (!g_dialog) return;
    for (HWND p : g_pages)
        if (p) { ::KillTimer(p, TIMER_APPLY); applyNumbers(p); }
    nppMsg(NPPM_MODELESSDIALOG, MODELESSDIALOGREMOVE, reinterpret_cast<LPARAM>(g_dialog));
    HWND d = g_dialog;
    ::DestroyWindow(d);
}

INT_PTR CALLBACK dialogProc(HWND d, UINT m, WPARAM w, LPARAM)
{
    try
    {
        switch (m)
        {
        case WM_INITDIALOG:
        {
            g_dialog = d;
            HWND list = ::GetDlgItem(d, IDC_KEX_PAGELIST);
            for (const wchar_t* t : kPageTitle) ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
            // the pages go where Notepad++'s Preferences puts its own: right of the list, at the top
            RECT at{ 115, 10, 115 + 460, 10 + 205 };
            ::MapDialogRect(d, &at);
            for (int p = 0; p < PAGE_COUNT; ++p)
            {
                g_pages[p] = ::CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_KEX_PAGE_EDITING + p), d, pageProc, 0);
                if (!g_pages[p]) { log(L"KEX Settings: page %d could not be made (error %lu)", p, ::GetLastError()); continue; }
                ::SetWindowPos(g_pages[p], nullptr, at.left, at.top, at.right - at.left, at.bottom - at.top, SWP_NOZORDER | SWP_NOACTIVATE);
                fill(g_pages[p]);
                nppMsg(NPPM_DARKMODESUBCLASSANDTHEME, 0x1 /* dmfSubclassParent */, reinterpret_cast<LPARAM>(g_pages[p]));
            }
            nppMsg(NPPM_DARKMODESUBCLASSANDTHEME, static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(d));
            nppMsg(NPPM_MODELESSDIALOG, MODELESSDIALOGADD, reinterpret_cast<LPARAM>(d));
            // in the middle of Notepad++, as Preferences opens
            RECT pr{}, dr{};
            ::GetWindowRect(npp._nppHandle, &pr);
            ::GetWindowRect(d, &dr);
            const int w0 = dr.right - dr.left, h0 = dr.bottom - dr.top;
            int x = pr.left + ((pr.right - pr.left) - w0) / 2, y = pr.top + ((pr.bottom - pr.top) - h0) / 2;
            HMONITOR mon = ::MonitorFromWindow(npp._nppHandle, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (::GetMonitorInfoW(mon, &mi))
            {
                x = std::max<int>(mi.rcWork.left, std::min<int>(x, mi.rcWork.right - w0));
                y = std::max<int>(mi.rcWork.top, std::min<int>(y, mi.rcWork.bottom - h0));
            }
            ::SetWindowPos(d, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            showPage(g_page);
            return FALSE;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDC_KEX_PAGELIST && HIWORD(w) == LBN_SELCHANGE)
            {
                const LRESULT sel = ::SendDlgItemMessageW(d, IDC_KEX_PAGELIST, LB_GETCURSEL, 0, 0);
                if (sel >= 0) showPage(static_cast<int>(sel));
                return TRUE;
            }
            if (LOWORD(w) == IDCANCEL || LOWORD(w) == IDC_KEX_CLOSE) { closeDialog(); return TRUE; }
            break;
        case WM_CLOSE:
            closeDialog();
            return TRUE;
        case WM_DESTROY:
            for (HWND& p : g_pages) p = nullptr;   // destroyed with it
            g_dialog = nullptr;
            break;
        }
    }
    catch (...) { log(L"exception in the KEX Settings window"); }
    return FALSE;
}

} // namespace

HWND prefPage(int i) { return (i >= 0 && i < PAGE_COUNT) ? g_pages[i] : nullptr; }
HWND settingsDialog() { return g_dialog; }

int pageOfModule(int module)
{
    switch (module)
    {
    case M_LETTERS: case M_CARET: case M_KEYS: return PAGE_EDITING;
    case M_TABS: case M_SNAP: case M_SWIPE: return PAGE_TABS;
    case M_GLASS: return PAGE_WINDOWS;
    case M_TOKENS: return PAGE_HIGHLIGHT;
    case M_HOTKEY: return PAGE_NOTE;
    case M_FLASH: case M_SLIDE: case M_SPLASH: case M_TABFOLD: case M_GLINT: return PAGE_EFFECTS;
    default: return PAGE_WINDOWS;
    }
}

const wchar_t* pageTitle(int page) { return (page >= 0 && page < PAGE_COUNT) ? kPageTitle[page] : L""; }

void refreshPrefPages()
{
    for (HWND d : g_pages)
        if (d && ::IsWindow(d)) fill(d);
}

// KEX > Settings...: the window on that page (-1: the page shown last), brought forward when it is open already
void openSettings(int page)
{
    if (!modules[0]) return;
    if (page >= 0 && page < PAGE_COUNT) g_page = page;
    if (!g_dialog)
    {
        ::CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_KEX_SETTINGS), npp._nppHandle, dialogProc, 0);
        if (!g_dialog) { log(L"KEX Settings: the window could not be made (error %lu)", ::GetLastError()); return; }
    }
    else showPage(g_page);
    ::ShowWindow(g_dialog, SW_SHOW);
    ::SetForegroundWindow(g_dialog);
    ::SetFocus(::GetDlgItem(g_dialog, IDC_KEX_PAGELIST));
}

void settingsThemeChanged()
{
    if (!g_dialog) return;
    nppMsg(NPPM_DARKMODESUBCLASSANDTHEME, static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(g_dialog));
    ::RedrawWindow(g_dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

} // namespace kex

namespace kexsuite {

// the core: KEX > Settings... (IDM_SETTING_KEX) and NPPM_KEX_SHOWPREFPAGE
void showSettings(int page)
{
    try { kex::openSettings(page); }
    catch (...) { kex::log(L"exception while opening the KEX Settings window"); }
}

} // namespace kexsuite
