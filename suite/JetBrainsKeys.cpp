// JetBrainsKeys.cpp - "JetBrains shortcuts": the IntelliJ editor keys in Notepad++.
// Each key is a real Notepad++ command of KEX's (KEX > Editing > JetBrains Commands), so
// Settings > Shortcut Mapper lists them under Plugin commands (plugin "KEX"), where any of them can get
// another key or none, and Notepad++ warns about clashes. The KEX patch puts these keys before
// Notepad++'s own. Switching the feature off takes the keys away (the menu items still work); with
// "Also the keys Notepad++ uses" off, the commands whose default key Notepad++ uses for something else
// leave that key to Notepad++.
#include "KexSuite.h"
#include <array>
#include <cwctype>
#include <utility>

namespace kex {

namespace {

enum Mod { C = 1, S = 2, A = 4 };
enum Act {
    A_CMD,          // a Notepad++ menu command
    A_NEXT_OCCURRENCE, A_ALL_OCCURRENCES, A_DUPLICATE, A_DELETE_LINE, A_GROW_SELECTION, A_SHRINK_SELECTION,
    A_TOGGLE_CASE, A_LINE_AFTER, A_LINE_BEFORE
};

struct Binding {
    int id;                 // the command's internal id: never reuse or renumber (shortcuts.xml keeps keys by it)
    UINT vk; int mods; Act act; UINT cmd;
    bool npp;               // Notepad++ already uses this key for something else
    const wchar_t* keys; const wchar_t* name;
};

const Binding kBindings[] = {
    { 1, 'J', A, A_NEXT_OCCURRENCE, 0, false, L"Alt+J", L"Select Next Occurrence" },
    { 2, 'J', A | S, A_CMD, IDM_EDIT_MULTISELECTUNDO, false, L"Shift+Alt+J", L"Unselect Occurrence" },
    { 3, 'J', C | A | S, A_ALL_OCCURRENCES, 0, false, L"Ctrl+Shift+Alt+J", L"Select All Occurrences" },
    { 4, 'W', C, A_GROW_SELECTION, 0, true, L"Ctrl+W", L"Extend Selection" },
    { 5, 'W', C | S, A_SHRINK_SELECTION, 0, true, L"Ctrl+Shift+W", L"Shrink Selection" },
    { 6, VK_OEM_2, C, A_CMD, IDM_EDIT_BLOCK_COMMENT, false, L"Ctrl+/", L"Comment with Line Comment" },
    { 7, VK_DIVIDE, C, A_CMD, IDM_EDIT_BLOCK_COMMENT, false, L"Ctrl+Num /", L"Comment with Line Comment (numpad key)" },
    { 8, VK_OEM_2, C | S, A_CMD, IDM_EDIT_STREAM_COMMENT, false, L"Ctrl+Shift+/", L"Comment with Block Comment" },
    { 9, 'D', C, A_DUPLICATE, 0, false, L"Ctrl+D", L"Duplicate Line or Selection" },
    { 10, 'Y', C, A_DELETE_LINE, 0, true, L"Ctrl+Y", L"Delete Line" },
    { 11, VK_UP, A | S, A_CMD, IDM_EDIT_LINE_UP, true, L"Shift+Alt+Up", L"Move Line Up" },
    { 12, VK_DOWN, A | S, A_CMD, IDM_EDIT_LINE_DOWN, true, L"Shift+Alt+Down", L"Move Line Down" },
    { 13, VK_LEFT, A, A_CMD, IDM_VIEW_TAB_PREV, false, L"Alt+Left", L"Select Previous Tab" },
    { 14, VK_RIGHT, A, A_CMD, IDM_VIEW_TAB_NEXT, false, L"Alt+Right", L"Select Next Tab" },
    { 15, VK_F4, C, A_CMD, IDM_FILE_CLOSE, false, L"Ctrl+F4", L"Close Tab" },
    { 16, 'J', C | S, A_CMD, IDM_EDIT_JOIN_LINES, false, L"Ctrl+Shift+J", L"Join Lines" },
    { 17, 'U', C | S, A_TOGGLE_CASE, 0, false, L"Ctrl+Shift+U", L"Toggle Case" },
    { 18, 'M', C | S, A_CMD, IDM_SEARCH_GOTOMATCHINGBRACE, false, L"Ctrl+Shift+M", L"Move Caret to Matching Brace" },
    { 19, 'R', C, A_CMD, IDM_SEARCH_REPLACE, true, L"Ctrl+R", L"Replace" },
    { 20, VK_RETURN, S, A_LINE_AFTER, 0, false, L"Shift+Enter", L"Start New Line" },
    { 21, VK_RETURN, C | A, A_LINE_BEFORE, 0, false, L"Ctrl+Alt+Enter", L"Start New Line Before Current" },
    { 22, VK_SUBTRACT, C, A_CMD, IDM_VIEW_FOLD_CURRENT, true, L"Ctrl+Num -", L"Collapse" },
    { 23, VK_ADD, C, A_CMD, IDM_VIEW_UNFOLD_CURRENT, true, L"Ctrl+Num +", L"Expand" },
    { 24, VK_SUBTRACT, C | S, A_CMD, IDM_VIEW_FOLDALL, false, L"Ctrl+Shift+Num -", L"Collapse All" },
    { 25, VK_ADD, C | S, A_CMD, IDM_VIEW_UNFOLDALL, false, L"Ctrl+Shift+Num +", L"Expand All" },
    { 26, VK_F12, C, A_CMD, IDM_VIEW_FUNC_LIST, false, L"Ctrl+F12", L"File Structure (Function List)" },
    { 27, VK_F11, 0, A_CMD, IDM_SEARCH_TOGGLE_BOOKMARK, true, L"F11", L"Toggle Bookmark" },
};
constexpr int kBindingCount = static_cast<int>(sizeof(kBindings) / sizeof(kBindings[0]));
JetBrainsStats s_stats;
int g_cmdIds[kBindingCount] = {};   // the commands' Notepad++ command ids, once added

struct Range { intptr_t a = 0, b = 0; };

class JetBrainsKeys;
JetBrainsKeys* s_keys = nullptr;

class JetBrainsKeys : public Module {
public:
    JetBrainsKeys() { s_keys = this; }   // the menu items work whether or not the keys are on
    const wchar_t* key() const override { return L"JetBrainsKeys"; }
    const wchar_t* title() const override { return L"JetBrains shortcuts"; }

    void loadOptions() override
    {
        _all = settings.getInt(key(), L"AlsoNotepadKeys", 1) != 0;
        _caseMode = settings.getInt(key(), L"OccurrenceCase", 0);
        _wordMode = settings.getInt(key(), L"OccurrenceWords", 0);
        if (_on) applyKeys();
    }

    bool selfCheck(std::wstring& why) override
    {
        if (!g_cmdIds[0]) { why = L"the JetBrains commands could not be added to Notepad++"; return false; }
        return true;
    }

    bool enable() override
    {
        _on = true;
        loadOptions();
        applyKeys();
        return true;
    }

    void disable() override
    {
        _on = false;
        applyKeys();
        _history.clear();
    }

    // which commands have their key: all while the feature is on, except, with "Also the keys
    // Notepad++ uses" off, those still on a default key that Notepad++ uses
    void applyKeys()
    {
        for (int i = 0; i < kBindingCount; ++i)
        {
            if (!g_cmdIds[i]) continue;
            bool keyed = _on;
            if (keyed && !_all && kBindings[i].npp)
            {
                ShortcutKey sk{};
                const bool has = nppMsg(NPPM_GETSHORTCUTBYCMDID, static_cast<WPARAM>(g_cmdIds[i]), reinterpret_cast<LPARAM>(&sk)) != 0;
                const int mods = (sk._isCtrl ? C : 0) | (sk._isShift ? S : 0) | (sk._isAlt ? A : 0);
                keyed = !has || !(sk._key == kBindings[i].vk && mods == kBindings[i].mods);
            }
            nppMsg(NPPM_KEX_ENABLECOMMAND, static_cast<WPARAM>(g_cmdIds[i]), keyed ? TRUE : FALSE);
        }
    }

    // a command, from its key or its menu item: on the focused editing view, or else the current one
    void runCommand(int index)
    {
        if (index < 0 || index >= kBindingCount) return;
        HWND focus = ::GetFocus();
        HWND s = views.viewOf(focus) >= 0 ? focus : views.sci(views.current());
        const Binding& b = kBindings[index];
        // Enter pressed in another box of the window (a docked panel, the incremental search): it stays Enter there
        if ((b.act == A_LINE_AFTER || b.act == A_LINE_BEFORE) && s != focus && focus && ::GetKeyState(VK_RETURN) < 0)
        {
            ::SendMessageW(focus, WM_KEYDOWN, VK_RETURN, 1);
            ::SendMessageW(focus, WM_CHAR, L'\r', 1);
            return;
        }
        if (!s) return;
        ++s_stats.handled;
        s_stats.last = b.keys;
        try { run(index, s); }
        catch (...) { log(L"JetBrains shortcuts: exception in %ls", b.name); }
    }

private:
    void command(UINT id) { nppMsg(WM_COMMAND, id, 0); }

    // Alt+J matches what Smart Highlighting shows, unless the KEX Editing page picks otherwise:
    // case 0 like Smart Highlighting, 1 match case, 2 ignore case; whole words 0 like Smart
    // Highlighting, 1 from a bare caret (JetBrains), 2 always, 3 never
    void decideMatching(HWND s)
    {
        bool hlCase = false, hlWord = true;
        kexsuite::smartHighlightOptions(&hlCase, &hlWord);
        _matchCase = _caseMode == 1 ? true : _caseMode == 2 ? false : hlCase;
        _wholeWord = _wordMode == 1 ? sci(s, SCI_GETSELECTIONEMPTY) != 0 : _wordMode == 2 ? true : _wordMode == 3 ? false : hlWord;
    }

    void run(int index, HWND s)
    {
        const Binding& b = kBindings[index];
        switch (b.act)
        {
        case A_CMD: command(b.cmd); break;
        case A_NEXT_OCCURRENCE:
        case A_ALL_OCCURRENCES:
        {
            // the matching is settled on the first press and kept while occurrences are added
            if (sci(s, SCI_GETSELECTIONS) == 1) decideMatching(s);
            const bool next = b.act == A_NEXT_OCCURRENCE;
            const UINT id = _matchCase ? (_wholeWord ? (next ? IDM_EDIT_MULTISELECTNEXTMATCHCASEWHOLEWORD : IDM_EDIT_MULTISELECTALLMATCHCASEWHOLEWORD)
                                                     : (next ? IDM_EDIT_MULTISELECTNEXTMATCHCASE : IDM_EDIT_MULTISELECTALLMATCHCASE))
                                       : (_wholeWord ? (next ? IDM_EDIT_MULTISELECTNEXTWHOLEWORD : IDM_EDIT_MULTISELECTALLWHOLEWORD)
                                                     : (next ? IDM_EDIT_MULTISELECTNEXT : IDM_EDIT_MULTISELECTALL));
            command(id);
            break;
        }
        case A_DUPLICATE:
            sci(s, sci(s, SCI_GETSELECTIONEMPTY) ? SCI_LINEDUPLICATE : SCI_SELECTIONDUPLICATE);
            break;
        case A_DELETE_LINE:
            sci(s, SCI_LINEDELETE);
            break;
        case A_GROW_SELECTION: grow(s); break;
        case A_SHRINK_SELECTION: shrink(s); break;
        case A_TOGGLE_CASE:
        {
            if (sci(s, SCI_GETSELECTIONEMPTY))
            {
                intptr_t p = sci(s, SCI_GETCURRENTPOS);
                intptr_t a = sci(s, SCI_WORDSTARTPOSITION, p, TRUE), e = sci(s, SCI_WORDENDPOSITION, p, TRUE);
                if (e <= a) break;
                sci(s, SCI_SETSEL, a, e);
            }
            intptr_t n = sci(s, SCI_GETSELTEXT, 0, 0);
            std::string t(static_cast<size_t>(n) + 1, '\0');
            sci(s, SCI_GETSELTEXT, 0, reinterpret_cast<LPARAM>(&t[0]));
            int wn = ::MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0);
            std::wstring wt(static_cast<size_t>(wn > 0 ? wn : 1), L'\0');
            if (wn > 0) ::MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, &wt[0], wn);
            bool anyLower = false;
            for (wchar_t c : wt) if (std::iswlower(c)) { anyLower = true; break; }
            command(anyLower ? IDM_EDIT_UPPERCASE : IDM_EDIT_LOWERCASE);
            break;
        }
        case A_LINE_AFTER:
            sci(s, SCI_LINEEND);
            sci(s, SCI_NEWLINE);   // Notepad++ indents the new line as usual
            break;
        case A_LINE_BEFORE:
        {
            sci(s, SCI_BEGINUNDOACTION);
            intptr_t line = sci(s, SCI_LINEFROMPOSITION, sci(s, SCI_GETCURRENTPOS));
            int indent = static_cast<int>(sci(s, SCI_GETLINEINDENTATION, line));
            sci(s, SCI_GOTOPOS, sci(s, SCI_POSITIONFROMLINE, line));
            sci(s, SCI_NEWLINE);
            sci(s, SCI_SETLINEINDENTATION, line, indent);
            sci(s, SCI_GOTOPOS, sci(s, SCI_GETLINEINDENTPOSITION, line));
            sci(s, SCI_ENDUNDOACTION);
            break;
        }
        }
    }

    // ---- extend / shrink selection ---------------------------------------------------------------
    static bool strictlyContains(const Range& outer, const Range& in)
    {
        return outer.a <= in.a && outer.b >= in.b && (outer.a < in.a || outer.b > in.b);
    }

    static Range wordAt(HWND s, intptr_t p)
    {
        return Range{ sci(s, SCI_WORDSTARTPOSITION, p, TRUE), sci(s, SCI_WORDENDPOSITION, p, TRUE) };
    }

    // innermost quoted string on the line around cur: its inside and the string with its quotes
    static void quoteCandidates(HWND s, const Range& cur, std::vector<Range>& out)
    {
        intptr_t line = sci(s, SCI_LINEFROMPOSITION, cur.a);
        if (sci(s, SCI_LINEFROMPOSITION, cur.b) != line) return;
        intptr_t ls = sci(s, SCI_POSITIONFROMLINE, line), le = sci(s, SCI_GETLINEENDPOSITION, line);
        if (le - ls > 20000) return;
        const char* t = reinterpret_cast<const char*>(sci(s, SCI_GETRANGEPOINTER, ls, le - ls));
        if (!t) return;
        char q = 0; intptr_t open = -1;
        for (intptr_t i = 0; i < le - ls; ++i)
        {
            char c = t[i];
            if (q)
            {
                if (c == '\\') { ++i; continue; }
                if (c == q)
                {
                    Range in{ ls + open + 1, ls + i }, whole{ ls + open, ls + i + 1 };
                    if (in.a <= cur.a && in.b >= cur.b) { out.push_back(in); out.push_back(whole); }
                    else if (whole.a <= cur.a && whole.b >= cur.b) out.push_back(whole);
                    q = 0;
                }
            }
            else if (c == '"' || c == '\'' || c == '`') { q = c; open = i; }
        }
    }

    // innermost bracket pair around cur: its inside and the pair itself
    static void bracketCandidates(HWND s, const Range& cur, std::vector<Range>& out)
    {
        const intptr_t limit = 200000;
        intptr_t from = std::max<intptr_t>(0, cur.a - limit);
        if (cur.a <= from) return;
        const char* t = reinterpret_cast<const char*>(sci(s, SCI_GETRANGEPOINTER, from, cur.a - from));
        if (!t) return;
        int depth[3] = {};
        const char* opens = "([{";
        const char* closes = ")]}";
        for (intptr_t i = cur.a - 1; i >= from; --i)
        {
            char c = t[i - from];
            for (int k = 0; k < 3; ++k)
            {
                if (c == closes[k]) ++depth[k];
                else if (c == opens[k])
                {
                    if (depth[k] > 0) { --depth[k]; continue; }
                    intptr_t m = sci(s, SCI_BRACEMATCH, i, 0);
                    if (m >= cur.b)
                    {
                        out.push_back(Range{ i + 1, m });
                        out.push_back(Range{ i, m + 1 });
                        return;
                    }
                }
            }
        }
    }

    void grow(HWND s)
    {
        Range cur{ sci(s, SCI_GETSELECTIONSTART), sci(s, SCI_GETSELECTIONEND) };
        std::vector<Range>& hist = _history[s];
        if (!(cur.a == _last.a && cur.b == _last.b && _lastWnd == s)) hist.clear();   // the user selected something else
        std::vector<Range> cand;
        if (cur.a == cur.b) cand.push_back(wordAt(s, cur.a));
        else { Range w{ sci(s, SCI_WORDSTARTPOSITION, cur.a, TRUE), sci(s, SCI_WORDENDPOSITION, cur.b, TRUE) }; cand.push_back(w); }
        quoteCandidates(s, cur, cand);
        bracketCandidates(s, cur, cand);
        intptr_t la = sci(s, SCI_LINEFROMPOSITION, cur.a), lb = sci(s, SCI_LINEFROMPOSITION, cur.b);
        cand.push_back(Range{ sci(s, SCI_GETLINEINDENTPOSITION, la), sci(s, SCI_GETLINEENDPOSITION, lb) });
        cand.push_back(Range{ sci(s, SCI_POSITIONFROMLINE, la), sci(s, SCI_POSITIONFROMLINE, lb + 1) });
        cand.push_back(Range{ 0, sci(s, SCI_GETLENGTH) });
        const Range* best = nullptr;
        for (const Range& r : cand)
            if (r.b > r.a && strictlyContains(r, cur) && (!best || r.b - r.a < best->b - best->a)) best = &r;
        if (!best) return;
        hist.push_back(cur);
        _last = *best; _lastWnd = s;
        sci(s, SCI_SETSEL, best->a, best->b);
        ++s_stats.grows;
    }

    void shrink(HWND s)
    {
        Range cur{ sci(s, SCI_GETSELECTIONSTART), sci(s, SCI_GETSELECTIONEND) };
        std::vector<Range>& hist = _history[s];
        if (hist.empty() || !(cur.a == _last.a && cur.b == _last.b && _lastWnd == s)) return;
        Range prev = hist.back(); hist.pop_back();
        _last = prev; _lastWnd = s;
        sci(s, SCI_SETSEL, prev.a, prev.b);
    }

    bool _on = false, _all = true, _wholeWord = true, _matchCase = false;
    int _caseMode = 0, _wordMode = 0;
    struct Hist { HWND wnd; std::vector<Range> ranges; };
    struct HistMap {
        std::vector<Hist> items;
        std::vector<Range>& operator[](HWND w) { for (Hist& h : items) if (h.wnd == w) return h.ranges; items.push_back(Hist{ w, {} }); return items.back().ranges; }
        void clear() { items.clear(); }
    } _history;
    Range _last{ -1, -1 };
    HWND _lastWnd = nullptr;
};

template<int I> void runBinding() { if (s_keys) s_keys->runCommand(I); }
template<int... I> std::array<void (*)(), sizeof...(I)> makeRunners(std::integer_sequence<int, I...>) { return { { &runBinding<I>... } }; }

} // namespace

Module* createJetBrainsKeys() { return new JetBrainsKeys(); }
JetBrainsStats jetBrainsStats() { return s_stats; }

// KEX > Editing > JetBrains Commands: the commands, added once (Notepad++ keeps them until it closes)
void addJetBrainsCommands(HMENU parent, int position)
{
    if (g_cmdIds[0] || !parent) return;
    static const std::array<void (*)(), kBindingCount> runners = makeRunners(std::make_integer_sequence<int, kBindingCount>());
    std::vector<KexCommand> cmds;
    for (int i = 0; i < kBindingCount; ++i)
    {
        const Binding& b = kBindings[i];
        cmds.push_back(KexCommand{ b.id, b.name, runners[i], (b.mods & C) != 0, (b.mods & A) != 0, (b.mods & S) != 0, static_cast<unsigned char>(b.vk) });
    }
    HMENU sub = ::CreatePopupMenu();
    KexCommandList list{ cmds.data(), kBindingCount, g_cmdIds, -1 };
    if (!nppMsg(NPPM_KEX_ADDCOMMANDS, reinterpret_cast<WPARAM>(sub), reinterpret_cast<LPARAM>(&list)))
    {
        ::DestroyMenu(sub);
        g_cmdIds[0] = 0;
        log(L"JetBrains shortcuts: Notepad++ did not take the commands");
        return;
    }
    ::InsertMenuW(parent, static_cast<UINT>(position), MF_BYPOSITION | MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"JetBrains Commands");
    if (s_keys) s_keys->applyKeys();
}

int jetBrainsCommandId(int index) { return (index >= 0 && index < kBindingCount) ? g_cmdIds[index] : 0; }

} // namespace kex
