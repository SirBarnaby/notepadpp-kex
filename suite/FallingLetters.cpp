// FallingLetters.cpp - CS2D-style falling letters for deleted text.
//
// UI thread: on SCN_MODIFIED/SC_MOD_BEFOREDELETE (requested with NPPM_ADDSCNMODIFIEDFLAGS) we read
// every deleted character that is visible on screen, with its exact position, style colour and
// font, before Scintilla removes it. The batch is handed to the shared effects renderer (Effects.h),
// which turns it into pictures and lets them fall on its own thread.
#include "Effects.h"
#include <atomic>
#include <cmath>
#include <map>
#include <deque>

namespace kex {

namespace {

using fx::FontSpec;
using fx::CapRun;
using fx::CapItem;
typedef fx::LetterBatch CapBatch;

std::atomic<int> s_lastSpawned{ 0 }, s_lastCandidates{ 0 }, s_totalSpawned{ 0 };
std::atomic<int> s_skipUndo{ 0 }, s_skipAnim{ 0 }, s_skipGlobal{ 0 };

class FallingLetters;
FallingLetters* s_self = nullptr;

// ---------------------------------------------------------------------------------------------
// module

const UINT WM_KEX_SPAWN = WM_APP + 21;

class FallingLetters : public Module, public MsgWindow {
public:
    const wchar_t* key() const override { return L"FallingLetters"; }
    const wchar_t* title() const override { return L"Falling letters"; }

    void loadOptions() override
    {
        _gravity = settings.getInt(key(), L"Gravity", 2200);
        _maxParticles = std::max(1, std::min(2000, settings.getInt(key(), L"MaxLetters", 200)));
        _spinDeg = settings.getInt(key(), L"SpinDegPerSec", 260);
        _drift = settings.getInt(key(), L"DriftPxPerSec", 90);
        _pop = settings.getInt(key(), L"PopPxPerSec", 240);
        _respectAnim = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
        _undoToo = settings.getInt(key(), L"AnimateUndoRedo", 0) != 0;
        _opacity = std::max(5, std::min(100, settings.getInt(key(), L"Opacity", 100)));
        _inNote = settings.getInt(key(), L"InQuickNote", 1) != 0;
        fx::setSpeed(fx::K_LETTER, animSpeed(key(), kSpeedLetters));
    }

    bool selfCheck(std::wstring& why) override
    {
        HWND s = views.sci(0);
        if (!s || !::IsWindow(s)) { why = L"main Scintilla view not found"; return false; }
        // NPPM_ADDSCNMODIFIEDFLAGS exists since Notepad++ 8.7.7; an older Notepad++ returns 0.
        if (!_flagsAdded)
        {
            if (!nppMsg(NPPM_ADDSCNMODIFIEDFLAGS, 0, SC_MOD_BEFOREDELETE))
            { why = L"NPPM_ADDSCNMODIFIEDFLAGS not supported by this Notepad++"; return false; }
            _flagsAdded = true;
        }
        if (sci(s, SCI_GETCODEPAGE) < 0) { why = L"Scintilla does not answer"; return false; }
        HWND probe = createOverlayWindow(nullptr, true, L"probe");
        if (!probe) { why = L"cannot create a layered overlay window"; return false; }
        ::DestroyWindow(probe);
        return true;
    }

    bool enable() override
    {
        if (!createMsgWindow(L"KexFallingLetters")) return false;
        if (!fx::acquire()) { destroyMsgWindow(); return false; }
        s_self = this;
        return true;
    }

    void disable() override
    {
        if (s_self == this)
        {
            fx::clear(fx::K_LETTER);
            fx::release();
            s_self = nullptr;
        }
        _pending.clear();
        destroyMsgWindow();
    }

    // the quick note's editor (HotkeyDrop.cpp): it is not one of the views, so it hands its deletes over
    void fromNote(HWND s, Sci_Position pos, Sci_Position len, int modType)
    {
        if (!_inNote || len <= 0 || !::IsWindowVisible(s)) return;
        if ((modType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) && !_undoToo) { ++s_skipUndo; return; }
        if (_respectAnim && !fx::animationsOn()) { ++s_skipAnim; return; }
        capture(s, pos, len);
    }

    void onNotify(SCNotification* n) override
    {
        if (n->nmhdr.code == NPPN_GLOBALMODIFIED)
        {
            // Replace All: Notepad++ switches SCN_MODIFIED off for it (FindReplaceDlg.cpp), and reports it here.
            // Drop anything captured in the same message-loop turn so a Replace All never rains letters.
            if (!_pending.empty()) { _pending.clear(); ++s_skipGlobal; }
            return;
        }
        if (n->nmhdr.code != SCN_MODIFIED || !(n->modificationType & SC_MOD_BEFOREDELETE)) return;
        if (n->length <= 0) return;
        HWND s = static_cast<HWND>(n->nmhdr.hwndFrom);
        if (views.viewOf(s) < 0 || !::IsWindowVisible(s)) return;
        // a document shown in two views notifies from both: letters fall only where the edit was made
        if (views.sci(views.current()) != s) return;
        if ((n->modificationType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) && !_undoToo) { ++s_skipUndo; return; }
        if (_respectAnim && !fx::animationsOn()) { ++s_skipAnim; return; }
        capture(s, n->position, n->length);
    }

protected:
    LRESULT onMessage(UINT m, WPARAM w, LPARAM l) override
    {
        if (m == WM_KEX_SPAWN)
        {
            _posted = false;
            for (CapBatch& b : _pending) fx::submitLetters(std::move(b));
            _pending.clear();
            return 0;
        }
        return MsgWindow::onMessage(m, w, l);
    }

private:
    struct Cand { Sci_Position pos; int x, y, w; std::wstring ch; int style; };

    static int utf8Len(unsigned char c)
    {
        if (c < 0x80) return 1;
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;
    }

    void capture(HWND s, Sci_Position pos, Sci_Position len)
    {
        // a big visible delete means many calls per character: Scintilla's direct function, no messages
        const SciDirect ds(s);
        RECT client; ::GetClientRect(s, &client);
        int margins = 0;
        int nm = static_cast<int>(ds(SCI_GETMARGINS));
        for (int i = 0; i < nm; ++i) margins += static_cast<int>(ds(SCI_GETMARGINWIDTHN, i));
        Sci_Position firstVis = ds(SCI_GETFIRSTVISIBLELINE);
        Sci_Position onScreen = ds(SCI_LINESONSCREEN);
        Sci_Position firstDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis);
        Sci_Position lastDoc = ds(SCI_DOCLINEFROMVISIBLE, firstVis + onScreen + 1);
        Sci_Position visStart = ds(SCI_POSITIONFROMLINE, firstDoc);
        Sci_Position visEnd = ds(SCI_GETLINEENDPOSITION, lastDoc);
        Sci_Position a = std::max(pos, visStart), b = std::min(pos + len, visEnd);
        s_lastCandidates = 0; s_lastSpawned = 0;
        if (a >= b) return;
        if (b - a > 400000) b = a + 400000;

        std::string text(static_cast<size_t>(b - a) + 1, '\0');
        Sci_TextRangeFull tr{ { a, b }, &text[0] };
        ds(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&tr));
        text.resize(static_cast<size_t>(b - a));
        int cp = static_cast<int>(ds(SCI_GETCODEPAGE));
        UINT winCp = cp == SC_CP_UTF8 ? CP_UTF8 : (cp == 0 ? CP_ACP : static_cast<UINT>(cp));

        const bool wrapped = ds(SCI_GETWRAPMODE) != SC_WRAP_NONE;
        std::vector<Cand> cands;
        Sci_Position lineA = ds(SCI_LINEFROMPOSITION, a), lineB = ds(SCI_LINEFROMPOSITION, b);
        size_t i = 0;
        for (Sci_Position line = lineA; line <= lineB; ++line)
        {
            Sci_Position ls = std::max(a, static_cast<Sci_Position>(ds(SCI_POSITIONFROMLINE, line)));
            Sci_Position le = std::min(b, static_cast<Sci_Position>(ds(SCI_GETLINEENDPOSITION, line)));
            Sci_Position next = std::min(b, static_cast<Sci_Position>(ds(SCI_POSITIONFROMLINE, line + 1)));
            if (!ds(SCI_GETLINEVISIBLE, line)) { i = static_cast<size_t>(next - a); continue; }
            i = static_cast<size_t>(ls - a);
            int prevX = 0; bool havePrev = false; Cand* prev = nullptr;
            // unwrapped, the whole line has one y
            const int lineY = wrapped ? 0 : static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, ls));
            while (static_cast<Sci_Position>(i) + a < le)
            {
                unsigned char c = static_cast<unsigned char>(text[i]);
                int n = 1;
                if (winCp == CP_UTF8) n = utf8Len(c);
                else if (c >= 0x80 && ::IsDBCSLeadByteEx(winCp, c)) n = 2;
                if (i + n > text.size()) n = static_cast<int>(text.size() - i);
                Sci_Position p = a + static_cast<Sci_Position>(i);
                int x = static_cast<int>(ds(SCI_POINTXFROMPOSITION, 0, p));
                int y = wrapped ? static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, p)) : lineY;
                if (prev && havePrev && prev->y == y && x > prevX) prev->w = x - prevX;
                prev = nullptr; havePrev = false;
                bool blank = (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c < 0x20);
                if (!blank)
                {
                    wchar_t wbuf[4] = {};
                    int wn = ::MultiByteToWideChar(winCp, 0, &text[i], n, wbuf, 4);
                    bool onScreenH = x >= margins - 2 && x < client.right && y >= -40 && y < client.bottom;
                    if (wn > 0 && onScreenH)
                    {
                        Cand cd{ p, x, y, 0, std::wstring(wbuf, wn), static_cast<int>(ds(SCI_GETSTYLEINDEXAT, p)) };
                        cands.push_back(cd);
                        prev = &cands.back(); prevX = x; havePrev = true;
                    }
                }
                i += n;
            }
            if (prev && havePrev)
            {
                // last char of the line segment: measure it with the end of line position
                Sci_Position p2 = a + static_cast<Sci_Position>(i);
                int x2 = static_cast<int>(ds(SCI_POINTXFROMPOSITION, 0, p2));
                int y2 = static_cast<int>(ds(SCI_POINTYFROMPOSITION, 0, p2));
                prev->w = (y2 == prev->y && x2 > prev->x) ? x2 - prev->x : 0;
            }
            i = static_cast<size_t>(next - a);
        }
        s_lastCandidates = static_cast<int>(cands.size());
        if (cands.empty()) return;

        CapBatch batch;
        batch.gravity = static_cast<float>(_gravity);
        batch.drift = static_cast<float>(_drift);
        batch.spin = static_cast<float>(_spinDeg) * 3.14159265f / 180.0f;
        batch.pop = static_cast<float>(_pop);
        batch.opacity = _opacity / 100.0f;
        batch.maxLive = _maxParticles * 2;
        batch.extraAscent = static_cast<int>(sci(s, SCI_GETEXTRAASCENT));
        HMONITOR mon = ::MonitorFromWindow(s, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; ::GetMonitorInfoW(mon, &mi);
        batch.monitor = mi.rcMonitor;
        int lineH = static_cast<int>(sci(s, SCI_TEXTHEIGHT, sci(s, SCI_LINEFROMPOSITION, cands[0].pos)));
        int zoom = static_cast<int>(sci(s, SCI_GETZOOM));
        int dpi = dpiOf(s);

        std::map<int, std::pair<int, COLORREF>> styleCache;   // style -> (font index, colour)
        std::map<FontSpec, int> fontIdx;
        auto styleInfo = [&](int style) -> std::pair<int, COLORREF> {
            auto it = styleCache.find(style);
            if (it != styleCache.end()) return it->second;
            FontSpec f = fontFor(s, style, zoom, dpi);
            int idx;
            auto fi = fontIdx.find(f);
            if (fi == fontIdx.end()) { idx = static_cast<int>(batch.fonts.size()); batch.fonts.push_back(f); fontIdx[f] = idx; }
            else idx = fi->second;
            std::pair<int, COLORREF> r{ idx, static_cast<COLORREF>(sci(s, SCI_STYLEGETFORE, style)) };
            styleCache[style] = r;
            return r;
        };
        styleInfo(STYLE_DEFAULT);   // fonts[0]: baseline reference

        // Too many letters for the cap: let neighbours on a line fall together as chunks.
        // Chunks never span lines, so widen them until the pieces fit the cap.
        size_t chunk = (cands.size() + _maxParticles - 1) / _maxParticles;
        if (chunk < 1) chunk = 1;
        auto countPieces = [&](size_t ch) {
            size_t pieces = 0;
            for (size_t k = 0; k < cands.size(); ++pieces)
            {
                const Cand& first = cands[k];
                int right = first.x; size_t taken = 0;
                while (k < cands.size() && taken < ch)
                {
                    const Cand& c = cands[k];
                    if (taken && (c.y != first.y || c.x < right - 1 || c.x - right > 3 * lineH)) break;
                    right = c.x + (c.w > 0 ? c.w : lineH / 2);
                    ++k; ++taken;
                }
            }
            return pieces;
        };
        while (chunk < cands.size() && countPieces(chunk) > static_cast<size_t>(_maxParticles))
            chunk += std::max<size_t>(1, chunk / 4);
        for (size_t k = 0; k < cands.size() && batch.items.size() < static_cast<size_t>(_maxParticles);)
        {
            CapItem item;
            const Cand& first = cands[k];
            POINT org{ first.x, first.y }; ::ClientToScreen(s, &org);
            item.x = org.x; item.y = org.y; item.h = lineH;
            int right = first.x;
            size_t taken = 0;
            while (k < cands.size() && taken < chunk)
            {
                const Cand& c = cands[k];
                if (taken && (c.y != first.y || c.x < right - 1 || c.x - right > 3 * lineH)) break;
                auto si = styleInfo(c.style);
                CapRun r; r.text = c.ch; r.dx = c.x - first.x; r.color = si.second; r.font = si.first;
                item.runs.push_back(r);
                int cw = c.w > 0 ? c.w : lineH / 2;
                right = c.x + cw;
                ++k; ++taken;
            }
            item.w = std::max(1, right - first.x);
            batch.items.push_back(std::move(item));
        }
        s_lastSpawned = static_cast<int>(batch.items.size());
        s_totalSpawned += s_lastSpawned;
        _pending.push_back(std::move(batch));
        if (!_posted && hwnd()) { _posted = true; ::PostMessageW(hwnd(), WM_KEX_SPAWN, 0, 0); }
    }

    static FontSpec fontFor(HWND s, int style, int zoom, int dpi)
    {
        FontSpec f;
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

    std::vector<CapBatch> _pending;
    bool _posted = false, _flagsAdded = false;
    int _gravity = 2200, _maxParticles = 200, _spinDeg = 260, _drift = 90, _pop = 240;
    bool _respectAnim = true, _undoToo = false, _inNote = true;
    int _opacity = 100;
};

} // namespace

Module* createFallingLetters() { return new FallingLetters(); }

LettersStats lettersStats()
{
    LettersStats st;
    st.lastSpawned = s_lastSpawned; st.lastCandidates = s_lastCandidates; st.totalSpawned = s_totalSpawned;
    const fx::Stats fs = fx::stats();
    st.live = fs.live[fx::K_LETTER]; st.frames = fs.frames; st.overlayVisible = fs.visible && fs.live[fx::K_LETTER] > 0;
    st.skippedUndo = s_skipUndo; st.skippedAnimOff = s_skipAnim; st.skippedGlobal = s_skipGlobal;
    st.overlay = fs.box;
    return st;
}

void lettersFreezeAfter(int ms) { fx::freezeAfter(ms); }

void lettersFromNote(HWND sciHwnd, Sci_Position pos, Sci_Position len, int modType)
{
    if (s_self) s_self->fromNote(sciHwnd, pos, len, modType);
}

} // namespace kex
