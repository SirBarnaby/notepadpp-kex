// EditFlash.cpp - small answers to copy, cut and delete. Copy: the selection box flashes faintly. Cut and
// delete: the text closes up. The removed text fades where it was while the text after it glides into its
// place (the rest of the line slides left, the lines below move up), so nothing vanishes in one frame and
// nothing floats over other text. A cut (Ctrl+X, and Shift+Delete, the old Windows key for Cut) also lights
// the removed text up for a moment in the selection's colour, the way a copy flashes: it went somewhere.
//
// Copy: Scintilla puts text on the clipboard with its own window as the clipboard owner, so a clipboard
// listener (WM_CLIPBOARDUPDATE) knows which editor copied. Cut: the text is copied and then removed in the
// same command, so the removal (SC_MOD_BEFOREDELETE) comes while the clipboard still names that editor as
// the owner of a change the listener has not seen yet. Delete: a selection removed with Delete, Backspace or
// Edit > Delete. The close-up itself is drawn by Scintilla (SCI_KEX_COLLAPSE in the KEX core): it paints the
// view before the text goes, then each frame from the new text, so it is crisp, works on glass and in the
// quick note, and any keystroke, scroll or other change ends it at once. Following Apple's guidance for
// motion on frequent actions, it is short (220 ms by default), with no bounce, and never holds up typing.
#include "Effects.h"
#include "KexScintilla.h"
#include <cmath>

namespace kex {

namespace {

const UINT_PTR kSubclassId = 0x4B454653;   // 'KEFS'

class EditFlash;
EditFlash* s_self = nullptr;

enum FlashKind { F_COPY = 0, F_CUT, F_DELETE };

class EditFlash : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"EditFlash"; }
    const wchar_t* title() const override { return L"Copy, cut and delete"; }

    void loadOptions() override
    {
        _on[F_COPY] = settings.getInt(key(), L"Copy", 1) != 0;
        // the close-up replaced the lift and shrink (which replaced the first flashes): switches of its own
        _on[F_CUT] = settings.getInt(key(), L"CutClose", 1) != 0;
        _on[F_DELETE] = settings.getInt(key(), L"DeleteClose", 1) != 0;
        _closeMs = std::max(80, std::min(800, settings.getInt(key(), L"CloseUpMs", 220)));
        _strength = std::max(10, std::min(200, settings.getInt(key(), L"Strength", 100)));
        _respectAnim = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
        _inNote = settings.getInt(key(), L"InQuickNote", 1) != 0;
    }

    bool selfCheck(std::wstring& why) override
    {
        if (!views.sci(0)) { why = L"main Scintilla view not found"; return false; }
        if (!::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "AddClipboardFormatListener"))
        { why = L"this Windows has no clipboard listener"; return false; }
        if (!nppMsg(NPPM_ADDSCNMODIFIEDFLAGS, 0, SC_MOD_BEFOREDELETE)) { why = L"NPPM_ADDSCNMODIFIEDFLAGS not supported by this Notepad++"; return false; }
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexEditFlash")) return false;
        if (!::AddClipboardFormatListener(hwnd())) { destroyMsgWindow(); return false; }
        if (!fx::acquire()) { ::RemoveClipboardFormatListener(hwnd()); destroyMsgWindow(); return false; }
        _seenSeq = ::GetClipboardSequenceNumber();
        if (::SetWindowSubclass(npp._nppHandle, mainProc, kSubclassId, 0)) _subclassed = true;
        s_self = this;
        return true;
    }

    void disable() override
    {
        if (_subclassed) { ::RemoveWindowSubclass(npp._nppHandle, mainProc, kSubclassId); _subclassed = false; }
        if (s_self == this)
        {
            fx::clear(fx::K_FLASH);
            fx::release();
            s_self = nullptr;
        }
        if (hwnd()) ::RemoveClipboardFormatListener(hwnd());
        destroyMsgWindow();
    }

    void onNotify(SCNotification* n) override
    {
        if (n->nmhdr.code != SCN_MODIFIED || !(n->modificationType & SC_MOD_BEFOREDELETE) || n->length <= 0) return;
        HWND s = static_cast<HWND>(n->nmhdr.hwndFrom);
        if (views.viewOf(s) < 0 || !::IsWindowVisible(s)) return;
        if (views.sci(views.current()) != s) return;   // a document in two views notifies from both
        beforeDelete(s, n->position, n->length, n->modificationType);
    }

    // a removal in an editor: the second half of a cut, a deleted selection, or neither
    void beforeDelete(HWND s, Sci_Position pos, Sci_Position len, int modType)
    {
        if (modType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) return;
        const DWORD seq = ::GetClipboardSequenceNumber();
        const double now = nowMs();
        bool cut = false;
        if (seq != _seenSeq && ::GetClipboardOwner() == s) { cut = true; _seenSeq = seq; _cutSeq = seq; _cutAt = now; }
        else if (seq == _cutSeq && now - _cutAt < 60) cut = true;   // a rectangular cut removes line by line
        if (cut) { ++_counts[F_CUT]; closeUp(F_CUT, s, pos, pos + len); return; }
        const bool keyDelete = ::GetKeyState(VK_DELETE) < 0 || ::GetKeyState(VK_BACK) < 0 || _menuDelete;
        if (!keyDelete || sci(s, SCI_GETSELECTIONEMPTY)) return;   // typing over a selection replaces it: nothing
        ++_counts[F_DELETE];
        closeUp(F_DELETE, s, pos, pos + len);
    }

    int _counts[3] = {};
    int _lastRects = 0;
    int _closeUps = 0, _closeUpsRefused = 0;
    HWND _lastCloseUp = nullptr;

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_CLIPBOARDUPDATE)
        {
            const DWORD seq = ::GetClipboardSequenceNumber();
            if (seq == _seenSeq) return 0;   // the cut that removed text has already flashed
            _seenSeq = seq;
            HWND owner = ::GetClipboardOwner();
            if (!owner || !editorOf(owner) || !::IsWindowVisible(owner)) return 0;
            ++_counts[F_COPY];
            if (sci(owner, SCI_GETSELECTIONEMPTY))
            {
                // copying without a selection copies the caret's line
                const Sci_Position line = sci(owner, SCI_LINEFROMPOSITION, sci(owner, SCI_GETCURRENTPOS));
                flash(F_COPY, owner, sci(owner, SCI_POSITIONFROMLINE, line), sci(owner, SCI_POSITIONFROMLINE, line + 1));
            }
            else
            {
                std::vector<RECT> all;
                const int n = static_cast<int>(sci(owner, SCI_GETSELECTIONS));
                for (int i = 0; i < n && all.size() < 400; ++i)
                {
                    std::vector<RECT> r = rangeRects(owner, sci(owner, SCI_GETSELECTIONNSTART, i), sci(owner, SCI_GETSELECTIONNEND, i), true);
                    all.insert(all.end(), r.begin(), r.end());
                }
                spawn(F_COPY, owner, all);
            }
            return 0;
        }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    bool editorOf(HWND h) const
    {
        if (views.viewOf(h) >= 0) return true;
        return _inNote && h == quickNoteEditor();
    }

    void flash(int kind, HWND s, Sci_Position a, Sci_Position b)
    {
        spawn(kind, s, rangeRects(s, a, b, true));
    }

    // Cut and delete: Scintilla closes the text up (SCI_KEX_COLLAPSE), set up now, while the text is still there.
    // One range only (a rectangular or multiple selection removes several, which just go).
    void closeUp(int kind, HWND s, Sci_Position a, Sci_Position b)
    {
        _lastRects = static_cast<int>(rangeRects(s, a, b, true).size());
        if (!_on[kind] || b <= a) return;
        if (_respectAnim && !fx::animationsOn()) return;
        if (sci(s, SCI_GETSELECTIONS) > 1 || sci(s, SCI_SELECTIONISRECTANGLE)) return;
        KexCollapse c{};
        c.start = a;
        c.end = b;
        c.ms = _closeMs;
        if (kind == F_CUT)
        {
            // the selection's colour, lit: what a copy flashes in
            COLORREF sel = ::GetSysColor(COLOR_HIGHLIGHT);
            const LRESULT ca = sci(s, SCI_GETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK);
            if (ca & 0xFF000000) sel = static_cast<COLORREF>(ca & 0xFFFFFF);
            const COLORREF lit = mixColour(sel, RGB(255, 255, 255), 0.45);
            const int alpha = std::min(255, static_cast<int>(0.42 * 255 * _strength / 100.0));
            c.tint = static_cast<int>((static_cast<unsigned>(alpha) << 24) | (lit & 0xFFFFFF));
        }
        if (sci(s, SCI_KEX_COLLAPSE, reinterpret_cast<WPARAM>(&c))) { ++_closeUps; _lastCloseUp = s; }
        else ++_closeUpsRefused;
    }

    void spawn(int kind, HWND s, const std::vector<RECT>& rects)
    {
        _lastRects = static_cast<int>(rects.size());
        if (!_on[kind] || rects.empty()) return;
        if (_respectAnim && !fx::animationsOn()) return;
        COLORREF sel = ::GetSysColor(COLOR_HIGHLIGHT);
        const LRESULT ca = sci(s, SCI_GETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK);
        if (ca & 0xFF000000) sel = static_cast<COLORREF>(ca & 0xFFFFFF);
        const COLORREF colour = mixColour(sel, RGB(255, 255, 255), 0.45);
        const float k = _strength / 100.0f;
        const std::vector<fx::Key> keys = { { 0, 0 }, { 0.04f, 0.42f * k }, { 0.30f, 0 } };
        HMONITOR mon = ::MonitorFromWindow(s, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; ::GetMonitorInfoW(mon, &mi);
        std::vector<fx::Item> items;
        for (const RECT& r : rects)
        {
            fx::Item it;
            it.rect = true;
            it.rw = static_cast<float>(r.right - r.left);
            it.rh = static_cast<float>(r.bottom - r.top);
            it.x0 = (r.left + r.right) * 0.5f;
            it.y0 = (r.top + r.bottom) * 0.5f;
            it.color = colour;
            it.keys = keys;
            it.life = keys.back().t;
            it.monitor = mi.rcMonitor;
            it.kind = fx::K_FLASH;
            items.push_back(std::move(it));
        }
        fx::submit(std::move(items));
    }

    static LRESULT CALLBACK mainProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
    {
        if (m == WM_COMMAND && LOWORD(w) == IDM_EDIT_DELETE && s_self)
        {
            s_self->_menuDelete = true;
            LRESULT r = ::DefSubclassProc(h, m, w, l);
            if (s_self) s_self->_menuDelete = false;
            return r;
        }
        if (m == WM_NCDESTROY) ::RemoveWindowSubclass(h, mainProc, kSubclassId);
        return ::DefSubclassProc(h, m, w, l);
    }

    bool _on[3] = { true, true, true };
    int _strength = 100, _closeMs = 220;
    bool _respectAnim = true, _inNote = true, _subclassed = false, _menuDelete = false;
    DWORD _seenSeq = 0, _cutSeq = 0;
    double _cutAt = 0;
};

} // namespace

Module* createEditFlash() { return new EditFlash(); }

void flashFromNote(HWND sciHwnd, Sci_Position pos, Sci_Position len, int modType)
{
    if (s_self && quickNoteEditor() == sciHwnd) s_self->beforeDelete(sciHwnd, pos, len, modType);
}

FlashStats flashStats()
{
    FlashStats st;
    if (s_self)
    {
        st.copies = s_self->_counts[F_COPY]; st.cuts = s_self->_counts[F_CUT]; st.deletes = s_self->_counts[F_DELETE]; st.lastRects = s_self->_lastRects;
        st.closeUps = s_self->_closeUps; st.closeUpsRefused = s_self->_closeUpsRefused; st.lastCloseUp = s_self->_lastCloseUp;
    }
    const fx::Stats fs = fx::stats();
    st.live = fs.live[fx::K_FLASH];
    st.spawned = fs.spawned[fx::K_FLASH];
    return st;
}

// Screen rectangles of the visible part of [a, b) in an editor, one per visual line (the selection's
// shape). eol: a range that takes in a line end gets the width of a space there, as a selection does.
std::vector<RECT> rangeRects(HWND s, Sci_Position a, Sci_Position b, bool eol)
{
    std::vector<RECT> out;
    if (!s || b < a) return out;
    const SciDirect ds(s);
    RECT client; ::GetClientRect(s, &client);
    int margins = 0;
    const int nm = static_cast<int>(ds(SCI_GETMARGINS));
    for (int i = 0; i < nm; ++i) margins += static_cast<int>(ds(SCI_GETMARGINWIDTHN, i));
    const Sci_Position firstVis = ds(SCI_GETFIRSTVISIBLELINE);
    const Sci_Position onScreen = ds(SCI_LINESONSCREEN);
    const Sci_Position firstDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis);
    const Sci_Position lastDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis + onScreen + 1);
    a = std::max(a, static_cast<Sci_Position>(ds(SCI_POSITIONFROMLINE, firstDoc)));
    const Sci_Position visEnd = ds(SCI_POSITIONFROMLINE, lastDoc + 1);
    b = std::min(b, visEnd >= 0 && visEnd > ds(SCI_POSITIONFROMLINE, lastDoc) ? visEnd : static_cast<Sci_Position>(ds(SCI_GETLENGTH)));
    if (a > b) return out;
    const int spaceW = std::max(4, static_cast<int>(ds(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<sptr_t>(" "))));
    auto add = [&](int l, int t, int r, int btm) {
        l = std::max(l, margins); r = std::min(r, static_cast<int>(client.right));
        t = std::max(t, 0); btm = std::min(btm, static_cast<int>(client.bottom));
        if (r <= l || btm <= t || out.size() >= 400) return;
        POINT p1{ l, t }, p2{ r, btm };
        ::ClientToScreen(s, &p1); ::ClientToScreen(s, &p2);
        out.push_back(RECT{ p1.x, p1.y, p2.x, p2.y });
    };
    const Sci_Position lineA = ds(SCI_LINEFROMPOSITION, a), lineB = ds(SCI_LINEFROMPOSITION, b);
    for (Sci_Position line = lineA; line <= lineB && out.size() < 400; ++line)
    {
        if (!ds(SCI_GETLINEVISIBLE, line)) continue;
        const Sci_Position lineEnd = ds(SCI_GETLINEENDPOSITION, line);
        const Sci_Position ls = std::max(a, static_cast<Sci_Position>(ds(SCI_POSITIONFROMLINE, line)));
        const Sci_Position le = std::min(b, lineEnd);
        const bool takesEol = eol && b > lineEnd;
        if (ls > le || (ls == le && !takesEol)) continue;
        const int lineH = static_cast<int>(ds(SCI_TEXTHEIGHT, line));
        const int x1 = static_cast<int>(ds(SCI_POINTXFROMPOSITION, 0, ls)), y1 = static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, ls));
        int x2 = static_cast<int>(ds(SCI_POINTXFROMPOSITION, 0, le));
        const int y2 = static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, le));
        if (takesEol) x2 += spaceW;
        if (y2 <= y1) { add(x1, y1, x2, y1 + lineH); continue; }
        // a wrapped line: the first piece to the right edge, whole rows, the last piece from the left
        add(x1, y1, client.right, y1 + lineH);
        for (int y = y1 + lineH; y < y2 && y < client.bottom; y += lineH) add(margins, y, client.right, y + lineH);
        add(margins, y2, x2, y2 + lineH);
    }
    return out;
}

} // namespace kex
