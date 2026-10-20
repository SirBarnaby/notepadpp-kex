// EditFlash.cpp - small answers to copy, cut and delete: the selection box flashes faintly when it is
// copied; cut text lifts off the page and fades, as if picked up; deleted text shrinks into the caret's
// side and fades.
//
// Copy: Scintilla puts text on the clipboard with its own window as the clipboard owner, so a
// clipboard listener (WM_CLIPBOARDUPDATE) knows which editor copied. Cut: the text is copied and then
// removed in the same command, so the removal (SC_MOD_BEFOREDELETE) comes while the clipboard still
// names that editor as the owner of a change the listener has not seen yet. Delete: a selection removed
// with Delete, Backspace or Edit > Delete. Cut and delete take a "ghost" of the text at that moment (its
// characters where they are, in their fonts and colours, on the selection's colour), so the text seems
// to leave rather than vanish. Everything is drawn by the shared effects renderer (Effects.h). With the
// falling letters on, the ghost is only the selection's box: the letters already fall out of it.
#include "Effects.h"
#include <cmath>
#include <map>

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
        // the lift and the shrink replaced the old cut and delete flashes, so they have switches of their own
        _on[F_CUT] = settings.getInt(key(), L"CutLift", 1) != 0;
        _on[F_DELETE] = settings.getInt(key(), L"DeleteFade", 1) != 0;
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
            fx::clear(fx::K_GHOST);
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
        if (cut) { ++_counts[F_CUT]; ghost(F_CUT, s, pos, pos + len); return; }
        const bool keyDelete = ::GetKeyState(VK_DELETE) < 0 || ::GetKeyState(VK_BACK) < 0 || _menuDelete;
        if (!keyDelete || sci(s, SCI_GETSELECTIONEMPTY)) return;   // typing over a selection replaces it: nothing
        ++_counts[F_DELETE];
        ghost(F_DELETE, s, pos, pos + len);
    }

    int _counts[3] = {};
    int _lastRects = 0;
    int _lastGhostChars = 0;

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

    static int utf8Len(unsigned char c)
    {
        if (c < 0x80) return 1;
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;
    }

    static fx::FontSpec fontFor(HWND s, int style, int zoom, int dpi)
    {
        fx::FontSpec f;
        char name[128] = {};
        sci(s, SCI_STYLEGETFONT, style, reinterpret_cast<LPARAM>(name));
        wchar_t wname[128] = {};
        ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 127);
        f.face = wname[0] ? wname : L"Courier New";
        double pts = static_cast<double>(sci(s, SCI_STYLEGETSIZEFRACTIONAL, style)) / SC_FONT_SIZE_MULTIPLIER + zoom;
        if (pts < 2) pts = 2;
        f.height = static_cast<int>(pts * dpi / 72.0 + 0.5);
        f.weight = static_cast<int>(sci(s, SCI_STYLEGETWEIGHT, style));
        f.italic = sci(s, SCI_STYLEGETITALIC, style) != 0;
        return f;
    }

    // Cut and delete: a picture of [a, b) as it is on screen now (one piece per visual line, the text in its
    // own fonts and colours on the selection's colour), which lifts away (cut) or shrinks away (delete).
    void ghost(int kind, HWND s, Sci_Position a, Sci_Position b)
    {
        const std::vector<RECT> rects = rangeRects(s, a, b, true);
        _lastRects = static_cast<int>(rects.size());
        _lastGhostChars = 0;
        if (!_on[kind] || rects.empty()) return;
        if (_respectAnim && !fx::animationsOn()) return;
        fx::LetterBatch batch;
        batch.motion = kind == F_CUT ? fx::MOTION_LIFT : fx::MOTION_SHRINK;
        batch.kind = fx::K_GHOST;
        batch.duration = kind == F_CUT ? 0.32f : 0.22f;
        const int dpi = dpiOf(s);
        batch.lift = 12.0f * dpi / 96.0f;
        batch.opacity = std::min(1.0f, _strength / 100.0f);
        batch.extraAscent = static_cast<int>(sci(s, SCI_GETEXTRAASCENT));
        HMONITOR mon = ::MonitorFromWindow(s, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; ::GetMonitorInfoW(mon, &mi);
        batch.monitor = mi.rcMonitor;
        // the selection as it looks: translucent over the glass, otherwise mixed into the background as Scintilla draws it
        COLORREF sel = ::GetSysColor(COLOR_HIGHLIGHT);
        int selAlpha = 255;
        const LRESULT ca = sci(s, SCI_GETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK);
        if (sci(s, SCI_GETELEMENTISSET, SC_ELEMENT_SELECTION_BACK) || (ca & 0xFF000000))
        {
            sel = static_cast<COLORREF>(ca & 0xFFFFFF);
            selAlpha = static_cast<int>((static_cast<unsigned long>(ca) >> 24) & 0xFF);
            if (selAlpha == 0) selAlpha = 255;
        }
        if (glassStats().active) { batch.back = sel; batch.backAlpha = std::max(0.35f, selAlpha / 255.0f); }
        else
        {
            const COLORREF bg = static_cast<COLORREF>(sci(s, SCI_STYLEGETBACK, STYLE_DEFAULT));
            batch.back = mixColour(bg, sel, selAlpha / 255.0);
            batch.backAlpha = 1;
        }
        POINT org{ 0, 0 };
        ::ClientToScreen(s, &org);
        for (const RECT& r : rects)
        {
            fx::CapItem it;
            it.x = r.left; it.y = r.top; it.w = r.right - r.left; it.h = r.bottom - r.top;
            batch.items.push_back(it);
        }
        const int zoom = static_cast<int>(sci(s, SCI_GETZOOM));
        batch.fonts.push_back(fontFor(s, STYLE_DEFAULT, zoom, dpi));   // fonts[0]: the baseline
        // the text, unless the falling letters take it
        if (!(modules[M_LETTERS] && modules[M_LETTERS]->isEnabled())) captureText(s, a, b, org, zoom, dpi, batch);
        fx::submitLetters(std::move(batch));
    }

    void captureText(HWND s, Sci_Position a, Sci_Position b, POINT org, int zoom, int dpi, fx::LetterBatch& batch)
    {
        const SciDirect ds(s);
        const Sci_Position firstVis = ds(SCI_GETFIRSTVISIBLELINE);
        const Sci_Position firstDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis);
        const Sci_Position lastDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis + ds(SCI_LINESONSCREEN) + 1);
        a = std::max(a, static_cast<Sci_Position>(ds(SCI_POSITIONFROMLINE, firstDoc)));
        b = std::min(b, static_cast<Sci_Position>(ds(SCI_GETLINEENDPOSITION, lastDoc)));
        if (a >= b) return;
        b = std::min(b, a + 20000);
        std::string text(static_cast<size_t>(b - a) + 1, '\0');
        Sci_TextRangeFull tr{ { a, b }, &text[0] };
        ds(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&tr));
        text.resize(static_cast<size_t>(b - a));
        const int cp = static_cast<int>(ds(SCI_GETCODEPAGE));
        const UINT winCp = cp == SC_CP_UTF8 ? CP_UTF8 : (cp == 0 ? CP_ACP : static_cast<UINT>(cp));
        // selected text keeps its colours unless the theme gives selected text a colour of its own
        const bool selFore = ds(SCI_GETELEMENTISSET, SC_ELEMENT_SELECTION_TEXT) != 0;
        const COLORREF selForeColour = static_cast<COLORREF>(ds(SCI_GETELEMENTCOLOUR, SC_ELEMENT_SELECTION_TEXT) & 0xFFFFFF);
        std::map<int, std::pair<int, COLORREF>> styles;
        std::map<fx::FontSpec, int> fontIdx{ { batch.fonts[0], 0 } };
        auto styleInfo = [&](int style) {
            auto it = styles.find(style);
            if (it != styles.end()) return it->second;
            const fx::FontSpec f = fontFor(s, style, zoom, dpi);
            auto fi = fontIdx.find(f);
            int idx;
            if (fi == fontIdx.end()) { idx = static_cast<int>(batch.fonts.size()); batch.fonts.push_back(f); fontIdx[f] = idx; }
            else idx = fi->second;
            const COLORREF fore = selFore ? selForeColour : static_cast<COLORREF>(ds(SCI_STYLEGETFORE, style));
            return styles[style] = std::make_pair(idx, fore);
        };
        // each visual line's piece, by its top on screen
        std::map<int, size_t> byTop;
        for (size_t i = 0; i < batch.items.size(); ++i) byTop[batch.items[i].y] = i;
        int chars = 0;
        for (size_t i = 0; i < text.size() && chars < 8000;)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            int n = winCp == CP_UTF8 ? utf8Len(c) : (c >= 0x80 && ::IsDBCSLeadByteEx(winCp, c) ? 2 : 1);
            if (i + n > text.size()) n = static_cast<int>(text.size() - i);
            const Sci_Position p = a + static_cast<Sci_Position>(i);
            i += n;
            if (c <= ' ') continue;   // blanks, tabs, line ends: nothing to draw
            const int x = static_cast<int>(ds(SCI_POINTXFROMPOSITION, 0, p)) + org.x;
            const int y = static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, p)) + org.y;
            auto line = byTop.find(y);
            if (line == byTop.end()) continue;
            fx::CapItem& item = batch.items[line->second];
            if (x < item.x - 1 || x >= item.x + item.w) continue;   // scrolled out of sight sideways
            wchar_t w[4] = {};
            const int wn = ::MultiByteToWideChar(winCp, 0, &text[static_cast<size_t>(p - a)], n, w, 4);
            if (wn <= 0) continue;
            const auto si = styleInfo(static_cast<int>(ds(SCI_GETSTYLEINDEXAT, p)));
            fx::CapRun r; r.text.assign(w, static_cast<size_t>(wn)); r.dx = x - item.x; r.color = si.second; r.font = si.first;
            item.runs.push_back(r);
            ++chars;
        }
        _lastGhostChars = chars;
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
    int _strength = 100;
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
    if (s_self) { st.copies = s_self->_counts[F_COPY]; st.cuts = s_self->_counts[F_CUT]; st.deletes = s_self->_counts[F_DELETE]; st.lastRects = s_self->_lastRects; st.ghostChars = s_self->_lastGhostChars; }
    const fx::Stats fs = fx::stats();
    st.live = fs.live[fx::K_FLASH] + fs.live[fx::K_GHOST];
    st.spawned = fs.spawned[fx::K_FLASH];
    st.ghostsLive = fs.live[fx::K_GHOST];
    st.ghosts = fs.spawned[fx::K_GHOST];
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
