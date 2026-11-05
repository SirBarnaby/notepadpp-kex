// EditFlash.cpp - copy flash: the copied selection lights up faintly for a moment, so a copy, which changes
// nothing on screen, still answers. Scintilla puts text on the clipboard with its own window as the clipboard
// owner, so a clipboard listener (WM_CLIPBOARDUPDATE) knows which editor copied.
//
// Cut and delete are motions drawn by the editor itself now (EditMotion.cpp): the cut text is picked up and
// goes, deleted text is wiped away or fades while the rest closes up.
#include "Effects.h"
#include <cmath>

namespace kex {

namespace {

class EditFlash;
EditFlash* s_self = nullptr;


class EditFlash : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"EditFlash"; }
    const wchar_t* title() const override { return L"Copy flash"; }

    void loadOptions() override
    {
        _on = settings.getInt(key(), L"Copy", 1) != 0;
        _strength = std::max(10, std::min(200, settings.getInt(key(), L"Strength", 100)));
        _respectAnim = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
        _inNote = settings.getInt(key(), L"InQuickNote", 1) != 0;
        fx::setSpeed(fx::K_FLASH, animSpeed(key(), L"Speed"));
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
        s_self = this;
        return true;
    }

    void disable() override
    {
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
        if (n->nmhdr.code == SCN_MODIFIED && (n->modificationType & SC_MOD_BEFOREDELETE))
            beforeDelete(static_cast<HWND>(n->nmhdr.hwndFrom));
    }

    // Text going right after its editor put something on the clipboard: a cut. Its clipboard change is not a copy
    // to flash (the text is gone; the cut has a motion of its own).
    void beforeDelete(HWND s)
    {
        const DWORD seq = ::GetClipboardSequenceNumber();
        if (seq != _seenSeq && ::GetClipboardOwner() == s) _seenSeq = seq;
    }

    int _copies = 0;
    int _lastRects = 0;

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_CLIPBOARDUPDATE)
        {
            const DWORD seq = ::GetClipboardSequenceNumber();
            if (seq == _seenSeq) return 0;
            _seenSeq = seq;
            HWND owner = ::GetClipboardOwner();
            if (!owner || !editorOf(owner) || !::IsWindowVisible(owner)) return 0;
            ++_copies;
            if (sci(owner, SCI_GETSELECTIONEMPTY))
            {
                // copying without a selection copies the caret's line
                const Sci_Position line = sci(owner, SCI_LINEFROMPOSITION, sci(owner, SCI_GETCURRENTPOS));
                flash(owner, sci(owner, SCI_POSITIONFROMLINE, line), sci(owner, SCI_POSITIONFROMLINE, line + 1));
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
                spawn(owner, all);
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

    void flash(HWND s, Sci_Position a, Sci_Position b)
    {
        spawn(s, rangeRects(s, a, b, true));
    }

    void spawn(HWND s, const std::vector<RECT>& rects)
    {
        _lastRects = static_cast<int>(rects.size());
        if (!_on || rects.empty()) return;
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

public:
    bool _on = true;
    int _strength = 100;
    bool _respectAnim = true, _inNote = true;
    DWORD _seenSeq = 0;

};

} // namespace

Module* createEditFlash() { return new EditFlash(); }

void flashFromNote(HWND sciHwnd, Sci_Position, Sci_Position, int)
{
    if (s_self && quickNoteEditor() == sciHwnd) s_self->beforeDelete(sciHwnd);
}

FlashStats flashStats()
{
    FlashStats st;
    if (s_self)
    {
        st.copies = s_self->_copies; st.lastRects = s_self->_lastRects;
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
