// KexApi.h - messages added to Notepad++ by the KEX four-view patch.
//
// The patch gives Notepad++ four editing views (0 = main, 1 = second, 2 = third, 3 = fourth)
// laid out as a 2x2 grid, and lets any view be torn out into its own top-level window.
// On an unpatched Notepad++ every message below returns 0, so a plugin calls NPPM_KEX_GETINFO
// first and falls back to the standard two-view API when it returns 0.
//
// Legacy plugin messages keep their two-view meaning (NPPM_GETCURRENTVIEW etc. only ever
// report 0 or 1), so existing plugins never see an index they don't expect.
//
// A plugin whose commands should also run while view 3 or 4 has focus exports
//     extern "C" __declspec(dllexport) BOOL kexAware() { return TRUE; }
// Without that export the patched Notepad++ blocks its menu commands in views 3/4 (with a
// message box) and sends it Scintilla notifications from views 0/1 only.
#pragma once
#include <windows.h>

#define KEX_USER (WM_USER + 9000)   // Notepad++ uses WM_USER+0..7999 ranges; 9000+ is free

// LRESULT: MAKELONG(viewCount, patchVersion). 0 = unpatched Notepad++.
#define NPPM_KEX_GETINFO              (KEX_USER + 1)
// LRESULT: index (0..3) of the view that has focus.
#define NPPM_KEX_GETCURRENTVIEW       (KEX_USER + 2)
// wParam: view. LRESULT: that view's Scintilla HWND (NULL if view >= count).
#define NPPM_KEX_GETVIEWSCINTILLA     (KEX_USER + 3)
// wParam: view. LRESULT: that view's tab bar HWND.
#define NPPM_KEX_GETVIEWTABBAR        (KEX_USER + 4)
// wParam: view. LRESULT: TRUE if the view is shown (it has at least one tab).
#define NPPM_KEX_ISVIEWVISIBLE        (KEX_USER + 5)
// wParam: view. LRESULT: number of tabs open in that view.
#define NPPM_KEX_GETNBVIEWFILES       (KEX_USER + 6)
// wParam: view, lParam: tab index. LRESULT: BufferID (0 if out of range).
#define NPPM_KEX_GETVIEWBUFFERID      (KEX_USER + 7)
// wParam: view. LRESULT: index of the active tab in that view, -1 if none.
#define NPPM_KEX_GETVIEWCURRENTINDEX  (KEX_USER + 8)
// wParam: view, lParam: tab index. Activates that tab in that view (and focuses the view).
#define NPPM_KEX_ACTIVATEDOC          (KEX_USER + 9)
// wParam: view, lParam: TRUE = tear the view out into its own window, FALSE = snap it back.
#define NPPM_KEX_TEAROUTVIEW          (KEX_USER + 10)
// wParam: view. LRESULT: top-level HWND currently hosting the view
// (the Notepad++ main window, or the view's own window when torn out).
#define NPPM_KEX_GETVIEWHOSTWINDOW    (KEX_USER + 11)
// "A Notepad++ for each desktop" (KEX suite). lParam: a folder (const wchar_t*). Writes this instance's
// tabs there: a session file plus a copy of each untitled tab. LRESULT: tabs written, -1 on failure.
#define NPPM_KEX_SAVEDESKTOPCOPY      (KEX_USER + 12)
// lParam: a folder written by NPPM_KEX_SAVEDESKTOPCOPY. Opens those tabs here; files already open from
// the command line stay last and active. LRESULT: tabs opened, -1 on failure.
#define NPPM_KEX_LOADDESKTOPCOPY      (KEX_USER + 13)
// wParam: a KEX Preferences page (0 "KEX Editing", 1 "KEX Tabs", 2 "KEX Windows", 3 "KEX Highlighting",
// 4 "KEX Quick Note", 5 "KEX Effects"). Opens Settings > Preferences on that page. LRESULT: TRUE.
#define NPPM_KEX_SHOWPREFPAGE         (KEX_USER + 14)

// Commands of the KEX features. They are added to a menu of the feature's, Settings > Shortcut Mapper
// lists them under Plugin commands (plugin "KEX") where their keys can be changed, and their keys go
// before Notepad++'s own, as the feature's keys did before.
struct KexCommand {
	int internalId;          // stable number, so Shortcut Mapper changes (shortcuts.xml) find it again
	const wchar_t* name;
	void (*run)();
	bool ctrl, alt, shift;
	unsigned char key;       // default key, 0 = none
};
struct KexCommandList {
	const KexCommand* items;
	int count;
	int* ids;                // receives each command's menu command id
	int position;            // where in the menu they go (-1: at the end)
};
// wParam: HMENU to add the commands to, lParam: KexCommandList*. LRESULT: TRUE.
#define NPPM_KEX_ADDCOMMANDS          (KEX_USER + 15)
// wParam: a command id from NPPM_KEX_ADDCOMMANDS, lParam: TRUE gives the command its key, FALSE takes it
// away (while its feature is off, so the key does what it does without KEX). LRESULT: TRUE.
#define NPPM_KEX_ENABLECOMMAND        (KEX_USER + 16)
// Opens Settings > Shortcut Mapper on Plugin commands, showing the KEX commands. LRESULT: TRUE.
#define NPPM_KEX_SHOWSHORTCUTS        (KEX_USER + 17)
// lParam: MSG*. For a message loop of its own: runs Notepad++'s shortcuts (its menu commands, the
// plugins' and KEX's) as its main loop does. LRESULT: TRUE when the message was a shortcut (then don't
// dispatch it).
#define NPPM_KEX_TRANSLATEACCEL       (KEX_USER + 18)

// wParam: a BufferID. LRESULT: the Scintilla document of that buffer (as SCI_GETDOCPOINTER returns it for
// a view showing it), 0 if there is no such buffer. Lets a feature read and mark any open document.
#define NPPM_KEX_GETBUFFERDOC         (KEX_USER + 19)
// wParam: a BufferID, lParam: 0 = read, 1 = make it the most recent tab. LRESULT: the buffer's place in the
// Ctrl+Tab order (a higher number is more recent; Notepad++ raises it on every switch), -1 if there is no
// such buffer. A tab shown only for a moment (a touchpad swipe that slides back) puts the order back with it.
#define NPPM_KEX_RECENTTAG            (KEX_USER + 20)
