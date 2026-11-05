// SmoothCaret.cpp - a Word-like caret: it glides to each new position with an ease-out curve and fades
// instead of blinking hard.
//
// Scintilla draws it (the KEX patch adds this to Scintilla's own caret drawing, see KexScintilla.h): the
// caret is painted right after the text in the same paint, so it moves in the same frame as the text,
// scrolls with it, keeps Notepad++'s caret colour, width and style, and needs no window of its own. This
// feature only switches it on in every view and passes the options on.
#include "KexSuite.h"
#include "KexScintilla.h"

namespace kex {

namespace {

class SmoothCaret;
SmoothCaret* s_self = nullptr;
std::vector<HWND> g_extra;   // editors other than the views that follow the feature (the quick note)

class SmoothCaret : public Module {
public:
    const wchar_t* key() const override { return L"SmoothCaret"; }
    const wchar_t* title() const override { return L"Smooth caret"; }

    void loadOptions() override
    {
        _glideMs = std::min(1000, animMs(80, animSpeed(key(), L"GlideSpeed", 80, L"GlideMs")));
        _snapLines = std::max(1, std::min(255, settings.getInt(key(), L"SnapAfterLines", 12)));
        // how it blinks: 0 fades, 1 on and off like Notepad++'s caret, 2 not at all (once a checkbox, FadeBlink)
        _blink = settings.getInt(key(), L"Blink", -1);
        if (_blink < 0 || _blink > 2)
        {
            _blink = settings.getInt(key(), L"FadeBlink", 1) ? 0 : 1;
            settings.setInt(key(), L"Blink", _blink);
        }
        _overtypeBlock = settings.getInt(key(), L"OvertypeBlock", 1) != 0;
        if (s_self == this) applyAll();
    }

    bool selfCheck(std::wstring& why) override
    {
        HWND s = views.sci(0);
        if (!s) { why = L"main Scintilla view not found"; return false; }
        KexCaretInfo info{};
        info.on = -1;
        ::SendMessageW(s, SCI_KEX_GETCARETINFO, 0, reinterpret_cast<LPARAM>(&info));
        if (info.on < 0) { why = L"this Scintilla has no smooth caret (not a KEX build)"; return false; }
        return true;
    }

    bool enable() override
    {
        s_self = this;
        applyAll();
        return true;
    }

    void disable() override
    {
        for (int v = 0; v < views.count(); ++v)
            if (HWND s = views.sci(v)) ::SendMessageW(s, SCI_KEX_SETSMOOTHCARET, 0, 0);
        for (HWND n : g_extra) if (::IsWindow(n)) ::SendMessageW(n, SCI_KEX_SETSMOOTHCARET, 0, 0);
        _applied.clear();
        s_self = nullptr;
    }

    void onNotify(SCNotification* n) override
    {
        // a view that appeared since (Notepad++ creates views 3 and 4 when they are first shown)
        if (n->nmhdr.code == NPPN_BUFFERACTIVATED || n->nmhdr.code == NPPN_READY) applyAll(false);
    }

private:
    void applyAll(bool force = true)
    {
        const LPARAM flags = this->flags();
        if (force) for (HWND n : g_extra) if (::IsWindow(n)) ::SendMessageW(n, SCI_KEX_SETSMOOTHCARET, static_cast<WPARAM>(_glideMs), flags);
        for (int v = 0; v < views.count(); ++v)
        {
            HWND s = views.sci(v);
            if (!s) continue;
            const bool known = std::find(_applied.begin(), _applied.end(), s) != _applied.end();
            if (known && !force) continue;
            ::SendMessageW(s, SCI_KEX_SETSMOOTHCARET, static_cast<WPARAM>(_glideMs), flags);
            if (!known) _applied.push_back(s);
        }
    }

public:
    LPARAM flags() const
    {
        return KEXCARET_ON | (_blink == 0 ? KEXCARET_FADE : 0) | (_blink == 2 ? KEXCARET_NOBLINK : 0) |
               (_overtypeBlock ? KEXCARET_OVERTYPE_BLOCK : 0) | KEXCARET_SNAPLINES(_snapLines);
    }
    int glideMs() const { return _glideMs; }

private:
    int _glideMs = 80, _snapLines = 12, _blink = 0;
    bool _overtypeBlock = true;
    std::vector<HWND> _applied;
};

} // namespace

Module* createSmoothCaret() { return new SmoothCaret(); }

// what the focused view (or, without focus in a view, the current one) drew at its last paint
CaretStats caretStats()
{
    CaretStats st;
    HWND s = ::GetFocus();
    if (views.viewOf(s) < 0) s = views.sci(views.current());
    KexCaretInfo info{};
    if (!s || !::SendMessageW(s, SCI_KEX_GETCARETINFO, 0, reinterpret_cast<LPARAM>(&info))) return st;
    st.visible = info.visible != 0;
    st.target = st.visible ? s : nullptr;
    st.carets = info.carets;
    st.mainX = info.mainX; st.mainY = info.mainY; st.mainH = info.mainH;
    st.block = info.block != 0;
    st.pushes = info.frames;
    st.workMs = info.workMs;
    st.alpha = info.alpha;
    return st;
}

void smoothCaretApply(HWND sciHwnd, bool on)
{
    if (!sciHwnd) return;
    g_extra.erase(std::remove_if(g_extra.begin(), g_extra.end(), [&](HWND h) { return h == sciHwnd || !::IsWindow(h); }), g_extra.end());
    if (on) g_extra.push_back(sciHwnd);
    if (on && s_self) ::SendMessageW(sciHwnd, SCI_KEX_SETSMOOTHCARET, static_cast<WPARAM>(s_self->glideMs()), s_self->flags());
    else ::SendMessageW(sciHwnd, SCI_KEX_SETSMOOTHCARET, 0, 0);
}

bool smoothCaretOn(HWND sciHwnd)
{
    KexCaretInfo info{};
    return sciHwnd && ::SendMessageW(sciHwnd, SCI_KEX_GETCARETINFO, 0, reinterpret_cast<LPARAM>(&info)) != 0;
}

void caretTestFreeze(double f, bool solid)
{
    const WPARAM w = f < 0 ? static_cast<WPARAM>(-1) : static_cast<WPARAM>(f * 1000);
    for (int v = 0; v < views.count(); ++v)
        if (HWND s = views.sci(v)) ::SendMessageW(s, SCI_KEX_CARETTEST, w, solid ? 1 : 0);
}

} // namespace kex
