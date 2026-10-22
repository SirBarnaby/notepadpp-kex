// KexSuite.h - shared declarations for the KEX suite, built into the KEX Notepad++ (KexSuiteHost.h).
// It drives Notepad++ through the documented plugin messages, like a plugin that is always loaded.
#pragma once
// Notepad++ is compiled for Windows 7; the suite uses Windows 10 APIs (virtual desktops, DWM cloaking)
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#undef WINVER
#define WINVER 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>
#include "PluginInterface.h"
#include "menuCmdID.h"
#include "KexApi.h"
#include "KexSuiteHost.h"

namespace kex {

extern NppData npp;
extern HINSTANCE hInst;
extern bool selfTestMode;      // set by -pluginMessage="KexSuite_selftest=1"
extern bool underWine;         // ntdll exports wine_get_version

// ---- logging (kex.log in Notepad++'s settings folder; the self-test writes its own log) --------
void log(const wchar_t* fmt, ...);

// ---- small helpers ----------------------------------------------------------------------------
inline LRESULT sci(HWND h, UINT m, WPARAM w = 0, LPARAM l = 0) { return ::SendMessageW(h, m, w, l); }

// Scintilla's direct function: a call without a window message, so without the views' subclass chain.
// For many calls in a row (a whole visible delete, a token pass); keep one for the length of that work.
struct SciDirect {
    HWND h = nullptr; SciFnDirect fn = nullptr; sptr_t p = 0;
    explicit SciDirect(HWND w);
    sptr_t operator()(UINT m, uptr_t w = 0, sptr_t l = 0) const { return fn ? fn(p, m, w, l) : static_cast<sptr_t>(::SendMessageW(h, m, w, l)); }
};

int dpiOf(HWND h);                                   // the window's DPI (its monitor's), 96 when unknown
int dpiAt(POINT pt);                                 // the DPI of the monitor at pt
COLORREF mixColour(COLORREF a, COLORREF b, double t); // a, moved t (0..1) of the way to b
bool isCloaked(HWND h);                              // on another virtual desktop (or otherwise cloaked by DWM)
extern const CLSID kCLSID_VirtualDesktopManager;     // shobjidl.h's, which MinGW's headers may lack
extern const IID kIID_IVirtualDesktopManager;
inline LRESULT nppMsg(UINT m, WPARAM w = 0, LPARAM l = 0) { return ::SendMessageW(npp._nppHandle, m, w, l); }
std::wstring bufferPath(intptr_t bufferId);
std::wstring fileNameOf(const std::wstring& path);
double nowMs();   // QueryPerformanceCounter in ms
bool rectsIntersect(const RECT& a, const RECT& b);

// A hidden message-only window that forwards to a member function.
class MsgWindow {
public:
    virtual ~MsgWindow() { destroyMsgWindow(); }
    bool createMsgWindow(const wchar_t* name);
    void destroyMsgWindow();
    HWND hwnd() const { return _hwnd; }
protected:
    virtual LRESULT onMessage(UINT m, WPARAM w, LPARAM l) { return ::DefWindowProcW(_hwnd, m, w, l); }
private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
    HWND _hwnd = nullptr;
};

// ---- views: the KEX core's four-view API (KexApi.h) ----
class Views {
public:
    void init();                       // call at NPPN_READY
    bool patched() const { return _patched; }
    int patchVersion() const { return _patchVersion; }
    int count() const { return _count; }
    HWND sci(int v) const;             // that view's Scintilla (nullptr if none)
    HWND tabBar(int v) const;          // that view's tab control
    int current() const;               // focused view
    bool visible(int v) const;
    int nbFiles(int v) const;
    intptr_t bufferId(int v, int idx) const;
    int currentIndex(int v) const;
    void activate(int v, int idx) const;
    HWND host(int v) const;            // top-level window hosting the view
    int viewOf(HWND sciHwnd) const;    // -1 if not an editing view
    std::vector<HWND> hostWindows() const;   // unique top-level hosts other than the main window
private:
    bool _patched = false;
    int _count = 2, _patchVersion = 0;
};
extern Views views;

// ---- settings (kex.ini in Notepad++'s settings folder, NPPM_GETNPPSETTINGSDIRPATH) -------------
class Settings {
public:
    void init();
    const std::wstring& path() const { return _path; }
    const std::wstring& dir() const { return _dir; }
    int getInt(const wchar_t* sec, const wchar_t* key, int def) const;
    void setInt(const wchar_t* sec, const wchar_t* key, int v) const;
    std::wstring getStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def = L"") const;
    void setStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) const;
    void deleteSection(const wchar_t* sec) const;
    std::vector<std::wstring> sectionNames() const;
private:
    std::wstring _path, _dir;
};
extern Settings settings;

// ---- feature modules ------------------------------------------------------------------------
class Module {
public:
    virtual ~Module() {}
    virtual const wchar_t* key() const = 0;       // INI section name
    virtual const wchar_t* title() const = 0;     // menu text
    // Verify the Notepad++ internals this feature relies on. Must not change any state.
    virtual bool selfCheck(std::wstring& why) = 0;
    virtual bool enable() = 0;                    // install hooks; false = could not
    virtual void disable() = 0;                   // remove hooks, restore Notepad++ state exactly
    virtual void loadOptions() {}
    virtual bool defaultOn() const { return true; }   // on until the user switches it off
    virtual void onNotify(SCNotification*) {}
    bool isEnabled() const { return _enabled; }

    bool _enabled = false;
    bool _checkOk = false;
    std::wstring _checkMsg;
    int _cmdIndex = -1;
};

// Implemented in the feature files.
Module* createFallingLetters();
Module* createSmoothCaret();
Module* createHotkeyDrop();
Module* createHoverTabs();
Module* createDesktopSync();
Module* createTabSnap();
Module* createJetBrainsKeys();
Module* createGlassText();
Module* createTokenStyles();
Module* createEditFlash();
Module* createTabSlide();
Module* createPaintSplash();
Module* createTabSwipe();
Module* createTabFold();
Module* createSaveGlint();

enum ModuleId { M_LETTERS = 0, M_CARET, M_HOTKEY, M_TABS, M_DESKTOP, M_SNAP, M_KEYS, M_GLASS, M_TOKENS, M_FLASH, M_SLIDE, M_SPLASH, M_SWIPE,
                M_TABFOLD, M_GLINT, M_COUNT };
extern Module* modules[M_COUNT];

// Core services used across modules.
bool setModuleEnabled(int id, bool on, bool interactive);
void refreshPrefPages();           // after a feature was switched on/off or failed
void showNote(const std::wstring& text, int ms);   // non-modal note in the corner of Notepad++
bool isPostItMode();
enum PrefPageId { PAGE_EDITING = 0, PAGE_TABS, PAGE_WINDOWS, PAGE_HIGHLIGHT, PAGE_NOTE, PAGE_EFFECTS, PAGE_COUNT };
// The KEX Settings window (PrefPages.cpp): KEX's own settings, apart from Notepad++'s Preferences.
HWND prefPage(int i);              // a page of it (PrefPageId), while the window is open
HWND settingsDialog();             // the window, while it is open
int pageOfModule(int module);      // the page a feature's settings are on
const wchar_t* pageTitle(int page);   // "Editing", "Tabs", ...
void openSettings(int page = -1);  // open it on a page (-1: the one shown last)
void settingsThemeChanged();       // NPPN_DARKMODECHANGED
// The KEX menu in the menu bar (SettingsMenu.cpp): a submenu per settings page, with a switch for each
// feature on it, the features' commands and "Settings..." to open the page; then "Settings...".
bool buildSettingsMenu();
void settingsMenuPopup(HMENU m);   // WM_INITMENUPOPUP
bool settingsMenuCommand(UINT id); // WM_COMMAND; true when it was one of the menu's
HMENU kexMenu();                   // the KEX menu itself
HMENU settingsSubmenu(int page);
UINT settingsSwitchId(int module);
UINT settingsPreferencesId(int page);

// Main-window subclass hooks (owned by the core, consulted by modules).
struct MainHooks {
    // main window moved/sized/activated
    void (*onMainMoved)() = nullptr;
};
extern MainHooks mainHooks;

// "A Notepad++ for each desktop" (DesktopSync.cpp)
void desktopLoadPendingCopy();     // at NPPN_READY: a new window opens the copy of the first window's tabs
LRESULT desktopServeCopy(HWND sender, const COPYDATASTRUCT* cds);   // WM_COPYDATA 'KEXT' from a new window

// Screen rectangles of the visible part of [a, b) in an editor, one per visual line (EditFlash.cpp)
std::vector<RECT> rangeRects(HWND sciHwnd, Sci_Position a, Sci_Position b, bool eol);
// Paint splash (PaintSplash.cpp): around a command that may colour tokens; the new ones splash
void paintSplashBegin();
void paintSplashEnd();
struct SplashScope { SplashScope() { paintSplashBegin(); } ~SplashScope() { paintSplashEnd(); } };

// Self-test entry points
void selfTestStart(const std::wstring& args);
void selfTestOnNotify(SCNotification* n);

// Wine compatibility: layered windows there have no partial alpha, so use a threshold.
inline bool binaryAlpha() { return underWine; }

// Per-module test hooks (only used by the self-test)
struct LettersStats { int lastSpawned = 0; int lastCandidates = 0; int totalSpawned = 0; int live = 0; int frames = 0;
                      RECT overlay{}; bool overlayVisible = false; int skippedUndo = 0; int skippedAnimOff = 0; int skippedGlobal = 0; };
LettersStats lettersStats();
void lettersFreezeAfter(int ms);   // <0 = run normally
void lettersFromNote(HWND sciHwnd, Sci_Position pos, Sci_Position len, int modType);   // the quick note's deletes
void flashFromNote(HWND sciHwnd, Sci_Position pos, Sci_Position len, int modType);
struct FlashStats { int copies = 0, cuts = 0, deletes = 0, lastRects = 0, live = 0, spawned = 0;
                    int ghosts = 0, ghostsLive = 0, ghostChars = 0; };   // cut and delete: ghost pieces made, on screen, characters in the last
FlashStats flashStats();
struct SplashStats { int splashes = 0, lastTokens = 0, live = 0, spawned = 0; };
SplashStats splashStats();
struct SlideStats { int seen = 0, started = 0, skipped = 0, lastDir = 0, afterClose = 0; bool active = false;
                    int slides = 0, frames = 0, captures = 0, cancelled = 0, stopReason = 0; float fraction = 0; };
SlideStats slideStats(HWND view);
void slideTestFreeze(double fraction);   // <0 = run normally
// Touchpad tab swipe (TabSwipe.cpp). swipeSwitching: a view is about to show another document; true when the
// swipe is doing that (then the tab slide leaves it alone).
bool swipeSwitching(HWND view);
struct SwipeStats { int gestures = 0, slides = 0, commits = 0, cancels = 0, textScrolls = 0, notches = 0, edges = 0, lastDir = 0;
                    double lastPos = 0; bool active = false, holding = false, rawRegistered = false; int fingers = -1;
                    std::wstring trace; };   // the last decisions, for the self-test's log
SwipeStats swipeStats();
void swipeTestFingers(int n);   // self-test: pretend this many fingers are on a precision touchpad (-1 = the real one)
std::wstring effectsInfo();       // the line on the KEX Effects page
// Tab close and open, save glint (TabMotion.cpp)
struct TabMotionStats { int folds = 0, unfolds = 0, foldBulk = 0, foldSkipped = 0, foldFrames = 0, lastDx = 0; float lastFraction = 0, lastWidth = 0;
                        int saves = 0, glints = 0, glintLines = 0, glintSkipped = 0, glintFrames = 0; bool overlayShown = false; };
TabMotionStats tabMotionStats();
void tabMotionTestFreeze(double fraction);   // <0 = run normally
bool tabMotionTestSave(HWND target, const std::wstring& file);   // the overlay's picture now, as a .bmp
// what Scintilla's smooth caret drew in the focused view at its last paint (KexScintilla.h)
struct CaretStats { int carets = 0; bool visible = false; HWND target = nullptr;
                    float mainX = 0, mainY = 0, mainH = 0; bool block = false;
                    int pushes = 0;   // paints that drew the caret
                    double workMs = 0;   // time Scintilla spent working out and drawing carets
                    float alpha = 0;     // the main caret's blink opacity at the last paint
};
CaretStats caretStats();
void caretTestFreeze(double glideFraction, bool solidBlink);   // fraction <0 = off
bool smoothCaretOn(HWND sciHwnd);   // the smooth caret is on in this view
void smoothCaretApply(HWND sciHwnd, bool on);   // another editor (the quick note) follows the feature when on
struct HotkeyStats { bool registered = false; bool byOtherNpp = false; DWORD lastError = 0; UINT mods = 0, vk = 0; int presses = 0; std::wstring text; bool noteOpen = false; };
HotkeyStats hotkeyStats();
void hotkeyPress();              // run the same code path as WM_HOTKEY
bool hotkeyApply(UINT mods, UINT vk);   // store + re-register; true when registered
std::wstring hotkeyText(UINT mods, UINT vk);
HWND quickNoteWindow();          // the open quick note (nullptr when closed or fading out)
HWND quickNoteEditor();
std::wstring quickNoteFile();
std::wstring quickNoteFileError();   // why the chosen note file couldn't be read ("" when fine)
void quickNoteOpenFile();            // Settings > KEX Quick Note > Open Note File in Notepad++
void quickNoteOptionsChanged();      // the Preferences page changed an option of the open note
void addNoteCommands(HMENU parent, int position);
struct TabsStats { bool stripVisible = false; RECT strip{}; int tabCount = 0; int activeIndex = -1; int view = 0; std::vector<RECT> tabRects; std::vector<int> dirty; };
TabsStats tabsStats();
void tabsTestClick(int index);
struct SnapStats { bool visible = false; RECT target{}; RECT window{}; int alpha = 0; int shows = 0; int moves = 0; int cancels = 0;
                   bool lifted = false; RECT lift{}; int liftAlpha = 0; int lifts = 0; };
SnapStats snapStats();
struct JetBrainsStats { int handled = 0; int grows = 0; std::wstring last; };
JetBrainsStats jetBrainsStats();
void addJetBrainsCommands(HMENU parent, int position);   // Settings > KEX Editing > JetBrains Commands
int jetBrainsCommandId(int index);  // Notepad++'s command id of binding `index` (0 before they are added)
struct GlassStats { bool active = false; COLORREF key = 0; int hosts = 0; int remaps = 0; int placements = 0; int clicks = 0; int wheels = 0;
                    int dragSolids = 0; bool dragSolidNow = false; int accent = 0; int accentFailures = 0; int rendererSwitches = 0;
                    int layerViews = 0; int fallbackViews = 0; int layerStarts = 0; unsigned long layerError = 0;   // views on their see-through layer, views drawing as before
                    int swaps = 0; int swapsBack = 0;   // smooth moving: the plain blur took over from the acrylic, and gave back
                    HWND lastTarget = nullptr; std::wstring state; };
GlassStats glassStats();
std::wstring glassInfo();          // the line under the glass settings
void glassTestDragEnter();         // what a drag entering the glass does
struct TokenStats { int wrapperCalls = 0; int liveWrappers = 0; int docs = 0; int lexCalls = 0; int variantsMade = 0; int collisions = 0;
                    int fullPasses = 0; int menus = 0; int augmented = 0; bool menuKeyOn = false; std::wstring menuKey; std::wstring last; };
TokenStats tokenStats();
int tokenCommandId(int which);     // our menu commands: 0 Bold, 1 Italic, 2 Clear This Token, 3 Bold / 4 Italic (one token)
std::wstring tokenStoreFile();     // kex-tokens.txt
std::wstring tokenInfo();          // the line on the KEX Highlighting page
int tokenRuleCount(HWND view);     // tokens styled in the document of that view
int tokenVariantBase(HWND view, int style, int* flags);   // lexer style behind a bold/italic variant, -1 if not one
bool tokenDocWrapped(HWND view);   // the document's lexer goes through the suite
void addTokenCommands(HMENU parent, int position);       // Settings > KEX Highlighting > Token Style Menu
int tokenMenuCommandId();
int tokenOneOffAt(HWND view);      // bold/italic of the single range at the selection or caret (Style One Token)
void tokenExportCopy(const std::wstring& dir);   // the first window: the styles of its tabs, for a new window's copy
void tokenImportCopy(const std::wstring& dir);   // the new window: read them before the copy's folder goes
std::wstring desktopSessionKey();  // this window's desktop session ("" for the first window's own)

// Desktop provider (real IVirtualDesktopManager or the fake used by the self-test)
class DesktopProvider {
public:
    virtual ~DesktopProvider() {}
    virtual const wchar_t* name() const = 0;
    virtual bool ok() const = 0;
    virtual std::wstring error() const { return L""; }
    virtual bool currentDesktop(GUID* out) = 0;
    virtual bool windowDesktop(HWND h, GUID* out) = 0;
    virtual bool isOnCurrent(HWND h) = 0;
    virtual bool moveWindow(HWND h, const GUID& d) = 0;
    virtual bool exists(const GUID&) { return true; }
};
DesktopProvider* createRealDesktopProvider();
class FakeDesktopProvider;
DesktopProvider* desktopProvider();      // the active one (may be null)
void useFakeDesktops(bool on);
// Fake provider control for the self-test.
GUID fakeDesktopGuid(int i);             // deterministic GUID for fake desktop i (0..)
void fakeSwitchTo(int i);
int fakeWindowDesktop(HWND h);
std::wstring guidStr(const GUID& g);
bool guidFromStr(const std::wstring& s, GUID* g);
struct DesktopStats { bool newWindow = false; int copiedTabs = -1; int served = 0; std::wstring lastAction; };
DesktopStats desktopStats();
const std::wstring& logFileName();   // kex.log, or kex-desktop-<id>.log in a window for another desktop

} // namespace kex
