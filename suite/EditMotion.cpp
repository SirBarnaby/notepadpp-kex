// EditMotion.cpp - editing motion: edits move into place instead of jumping. Cut text is picked up and goes,
// deleted text is wiped away (within a line) or fades while the lines below close up, pasted text makes room
// and lights up for a moment, undo shows what came back, moved lines slide past each other, indented lines
// glide, a duplicated line slides out from under its original, folds open and close like drawers, sorted
// lines glide to their new places, a ring closes onto a found match and a far jump slides the page there.
//
// Scintilla draws all of it (SCI_KEX_SETMOTIONS in the KEX core, KexScintilla.h): the view pictures itself the
// moment an edit starts, and once the edit is done every frame is put together from that picture and the view
// as it is now, so the text stays sharp, works on glass and in the quick note, and nothing floats over other
// text. Following Apple's guidelines for motion (purposeful, brief, optional, cancellable): each one is short,
// moves on a critically damped spring (no bounce), ends at once on typing, any other change or a scroll, and is
// skipped when it comes right after another one (fast editing), so it never holds anyone up. Each has its own
// switch and its own speed.
#include "Effects.h"
#include "KexScintilla.h"

namespace kex {

static_assert(kMotionCount == KEXM_COUNT, "one setting for each of the core's motions");

const MotionDef kMotionDefs[kMotionCount] = {
    { L"Cut", 320, L"Cut: picked up, then gone" },
    { L"Delete", 260, L"Delete in a line: wiped away" },
    { L"DeleteLines", 260, L"Delete across lines: fades, the rest closes up" },
    { L"Paste", 260, L"Paste: room is made, the text lights up" },
    { L"Undo", 260, L"Undo and redo: what changes shows" },
    { L"MoveLine", 220, L"Move lines up or down: they slide" },
    { L"Find", 300, L"Find: a ring closes onto the match" },
    { L"Indent", 220, L"Indent several lines: they glide" },
    { L"Duplicate", 240, L"Duplicate a line: the copy slides out" },
    { L"Jump", 320, L"Go to a far line: the page slides there" },
    { L"Fold", 240, L"Fold and unfold: the lines slide" },
    { L"Sort", 340, L"Sort lines: each glides to its place" },
};

namespace {

class EditMotion;
EditMotion* s_self = nullptr;

class EditMotion : public Module {
public:
    const wchar_t* key() const override { return L"Motion"; }
    const wchar_t* title() const override { return L"Editing motion"; }

    void loadOptions() override
    {
        for (int k = 0; k < kMotionCount; ++k)
        {
            const MotionDef& d = kMotionDefs[k];
            _on[k] = settings.getInt(key(), d.key, 1) != 0;
            _ms[k] = std::min(3000, animMs(d.ms, animSpeed(key(), (std::wstring(d.key) + L"Speed").c_str(), d.ms, (std::wstring(d.key) + L"Ms").c_str())));
        }
        _glowMs = std::min(3000, animMs(600, animSpeed(key(), L"GlowSpeed", 600, L"GlowMs")));
        _rapidMs = std::max(0, std::min(3000, settings.getInt(key(), L"RapidMs", 250)));
        _respectAnim = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
        _inNote = settings.getInt(key(), L"InQuickNote", 1) != 0;
        if (s_self == this) pushAll();
    }

    bool selfCheck(std::wstring& why) override
    {
        HWND s = views.sci(0);
        if (!s) { why = L"main Scintilla view not found"; return false; }
        KexMotionInfo info{};
        info.lastKind = -2;
        ::SendMessageW(s, SCI_KEX_GETMOTIONINFO, 0, reinterpret_cast<LPARAM>(&info));
        if (info.lastKind == -2) { why = L"this Scintilla has no editing motion (not a KEX build)"; return false; }
        return true;
    }

    bool enable() override
    {
        s_self = this;
        pushAll();
        return true;
    }

    void disable() override
    {
        s_self = nullptr;
        pushAll();   // all off
    }

    void onNotify(SCNotification* n) override
    {
        // the light follows the selection's colour, which a theme or dark mode changes
        if (n->nmhdr.code == NPPN_WORDSTYLESUPDATED || n->nmhdr.code == NPPN_DARKMODECHANGED || n->nmhdr.code == NPPN_BUFFERACTIVATED)
            pushAll();
    }

    // what one editor gets: the times of the motions that are on (none when the feature is off)
    KexMotions motionsFor(HWND s) const
    {
        KexMotions m{};
        const bool live = s_self == this && !(_respectAnim && !fx::animationsOn());
        for (int k = 0; k < kMotionCount; ++k) m.ms[k] = (live && _on[k]) ? _ms[k] : 0;
        m.glowMs = _glowMs;
        m.rapidMs = _rapidMs;
        COLORREF sel = ::GetSysColor(COLOR_HIGHLIGHT);
        const LRESULT ca = s ? sci(s, SCI_GETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK) : 0;
        if (ca & 0xFF000000) sel = static_cast<COLORREF>(ca & 0xFFFFFF);
        const COLORREF lit = mixColour(sel, RGB(255, 255, 255), 0.45);
        m.light = static_cast<int>((110u << 24) | (lit & 0xFFFFFF));
        return m;
    }

    void push(HWND s, bool on) const
    {
        if (!s || !::IsWindow(s)) return;
        KexMotions m = motionsFor(s);
        if (!on) for (int& ms : m.ms) ms = 0;
        ::SendMessageW(s, SCI_KEX_SETMOTIONS, reinterpret_cast<WPARAM>(&m), 0);
    }

    void pushAll() const
    {
        for (int v = 0; v < views.count(); ++v) push(views.sci(v), true);
        push(quickNoteEditor(), _inNote);
    }

    bool _on[kMotionCount] = {};
    int _ms[kMotionCount] = {};
    int _glowMs = 600, _rapidMs = 250;
    bool _respectAnim = true, _inNote = true;
};

EditMotion* s_any = nullptr;   // the module, on or off: its settings

} // namespace

Module* createEditMotion() { s_any = new EditMotion(); return s_any; }

void applyMotions(HWND s)
{
    if (!s_any) return;
    if (s == quickNoteEditor()) s_any->push(s, s_any->_inNote);
    else s_any->push(s, true);
}

bool motionOn(int kind) { return s_any && kind >= 0 && kind < kMotionCount && s_any->_on[kind]; }

int motionMs(int kind) { return (s_any && kind >= 0 && kind < kMotionCount) ? s_any->_ms[kind] : 0; }

void setMotionOn(int kind, bool on)
{
    if (!s_any || kind < 0 || kind >= kMotionCount) return;
    settings.setInt(s_any->key(), kMotionDefs[kind].key, on ? 1 : 0);
    s_any->loadOptions();
}

MotionStats motionStats(HWND s)
{
    MotionStats st;
    if (!s) return st;
    KexMotionInfo info{};
    st.active = ::SendMessageW(s, SCI_KEX_GETMOTIONINFO, 0, reinterpret_cast<LPARAM>(&info)) != 0;
    for (int k = 0; k < kMotionCount; ++k) { st.started[k] = info.started[k]; st.total += info.started[k]; }
    st.skipped = info.skipped; st.refused = info.refused; st.ended = info.ended; st.frames = info.frames;
    st.lastKind = info.lastKind; st.fraction = info.fraction; st.gap = info.gap; st.style = info.style;
    st.ringFrames = info.ringFrames; st.jumpLines = info.jumpLines; st.lines = info.lines;
    return st;
}

} // namespace kex
