// SettingsMenu.cpp - the KEX menu in Notepad++'s menu bar (between Plugins and Window; the core's
// Notepad_plus.rc gives it, holding "Settings..."): one submenu for each page of the KEX Settings window
// ("Editing", "Tabs", "Windows", "Highlighting", "Quick Note", "Effects"), holding an on/off switch for every
// feature on that page, the features' commands and "Settings..." to open the window on that page.
#include "KexSuite.h"

namespace kex {

namespace {

UINT g_first = 0;               // our command ids (NPPM_ALLOCATECMDID): a switch per feature, then "Settings..." per page
HMENU g_menu = nullptr;
HMENU g_sub[PAGE_COUNT] = {};

// the KEX menu: the one in the menu bar holding KEX > Settings...
HMENU findKexMenu()
{
    HMENU mm = reinterpret_cast<HMENU>(nppMsg(NPPM_GETMENUHANDLE, NPPMAINMENU, 0));
    for (int i = 0, n = mm ? ::GetMenuItemCount(mm) : 0; i < n; ++i)
        if (HMENU sub = ::GetSubMenu(mm, i))
            if (::GetMenuState(sub, IDM_SETTING_KEX, MF_BYCOMMAND) != static_cast<UINT>(-1))
                return sub;
    return nullptr;
}

} // namespace

bool buildSettingsMenu()
{
    if (g_first) return true;
    HMENU kex = findKexMenu();
    if (!kex) { log(L"KEX menu: not found in the menu bar"); return false; }
    int first = 0;
    if (!nppMsg(NPPM_ALLOCATECMDID, M_COUNT + PAGE_COUNT, reinterpret_cast<LPARAM>(&first)) || first <= 0)
    {
        log(L"KEX menu: no command ids");
        return false;
    }
    g_first = static_cast<UINT>(first);
    g_menu = kex;
    // the page submenus, then a line, above the core's "Settings..."
    ::InsertMenuW(kex, 0, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    for (int p = PAGE_COUNT - 1; p >= 0; --p)
    {
        HMENU sub = ::CreatePopupMenu();
        for (int i = 0; i < M_COUNT; ++i)
            if (pageOfModule(i) == p) ::AppendMenuW(sub, MF_STRING, g_first + i, modules[i]->title());
        ::AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(sub, MF_STRING, g_first + M_COUNT + p, L"Settings...");
        ::InsertMenuW(kex, 0, MF_BYPOSITION | MF_POPUP, reinterpret_cast<UINT_PTR>(sub), pageTitle(p));
        g_sub[p] = sub;
    }
    // the features' commands, before the separator (Shortcut Mapper lists them under plugin "KEX")
    addJetBrainsCommands(g_sub[PAGE_EDITING], ::GetMenuItemCount(g_sub[PAGE_EDITING]) - 2);
    addTokenCommands(g_sub[PAGE_HIGHLIGHT], ::GetMenuItemCount(g_sub[PAGE_HIGHLIGHT]) - 2);
    addNoteCommands(g_sub[PAGE_NOTE], ::GetMenuItemCount(g_sub[PAGE_NOTE]) - 2);
    ::DrawMenuBar(npp._nppHandle);
    return true;
}

// WM_INITMENUPOPUP: the switches show which features are running
void settingsMenuPopup(HMENU m)
{
    for (int p = 0; p < PAGE_COUNT; ++p)
    {
        if (!m || m != g_sub[p]) continue;
        for (int i = 0; i < M_COUNT; ++i)
            if (pageOfModule(i) == p)
                ::CheckMenuItem(m, g_first + i, MF_BYCOMMAND | (modules[i]->isEnabled() ? MF_CHECKED : MF_UNCHECKED));
    }
}

// WM_COMMAND: true when the command was one of the menu's
bool settingsMenuCommand(UINT id)
{
    if (!g_first || id < g_first || id >= g_first + M_COUNT + PAGE_COUNT) return false;
    const int i = static_cast<int>(id - g_first);
    if (i < M_COUNT) setModuleEnabled(i, !modules[i]->isEnabled(), true);   // stored, as the Settings tick is
    else openSettings(i - M_COUNT);
    return true;
}

HMENU kexMenu() { return g_menu; }
HMENU settingsSubmenu(int page) { return (page >= 0 && page < PAGE_COUNT) ? g_sub[page] : nullptr; }
UINT settingsSwitchId(int module) { return g_first ? g_first + module : 0; }
UINT settingsPreferencesId(int page) { return g_first ? g_first + M_COUNT + page : 0; }

} // namespace kex
