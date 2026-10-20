// TabSlide.cpp - switching tabs slides the new document in, the way Windows 10 slides between
// virtual desktops: the old document moves out to one side while the new one follows it in.
//
// Scintilla does the drawing (SCI_KEX_SLIDE, KexScintilla.h): when a view is about to show another
// document the core tells the suite (kexsuite::bufferSwitching), the view paints what it shows into a
// picture at once, and its next paints move both pictures with an ease-out curve. Under DirectWrite
// (Notepad++'s default) that is Direct2D on the graphics card; the window itself never moves.
#include "KexSuite.h"
#include "KexScintilla.h"

namespace kex {

namespace {

class TabSlide;
TabSlide* s_self = nullptr;

class TabSlide : public Module {
public:
    const wchar_t* key() const override { return L"TabSlide"; }
    const wchar_t* title() const override { return L"Tab slide"; }

    void loadOptions() override
    {
        _ms = std::max(60, std::min(800, settings.getInt(key(), L"DurationMs", 200)));
        _respectAnim = settings.getInt(key(), L"RespectSystemAnimationSetting", 1) != 0;
    }

    bool selfCheck(std::wstring& why) override
    {
        HWND s = views.sci(0);
        if (!s) { why = L"main Scintilla view not found"; return false; }
        KexSlideInfo info{};
        info.active = -1;
        ::SendMessageW(s, SCI_KEX_GETSLIDEINFO, 0, reinterpret_cast<LPARAM>(&info));
        if (info.active < 0) { why = L"this Scintilla cannot slide (not a KEX build)"; return false; }
        return true;
    }

    bool enable() override { s_self = this; return true; }

    void disable() override
    {
        for (int v = 0; v < views.count(); ++v)
            if (HWND s = views.sci(v)) ::SendMessageW(s, SCI_KEX_SLIDE, 0, 0);
        s_self = nullptr;
    }

    // the view still shows oldBuffer; newBuffer comes next
    void switching(HWND view, void* oldBuffer, void* newBuffer)
    {
        ++_seen;
        const int v = views.viewOf(view);
        if (v < 0 || !oldBuffer || !newBuffer || oldBuffer == newBuffer) return;
        if (!::IsWindowVisible(view) || ::IsIconic(npp._nppHandle)) return;
        if (_respectAnim && !animationsOn()) { ++_skipped; return; }
        // which way: the tab further right comes in from the right, as the next desktop does
        int oldIdx = -1, newIdx = -1;
        const int n = views.nbFiles(v);
        for (int i = 0; i < n; ++i)
        {
            const intptr_t id = views.bufferId(v, i);
            if (id == reinterpret_cast<intptr_t>(oldBuffer)) oldIdx = i;
            if (id == reinterpret_cast<intptr_t>(newBuffer)) newIdx = i;
        }
        if (oldIdx < 0) return;   // the old document is already gone from this view (closing it): nothing to slide from
        const int dir = (newIdx < 0 || newIdx > oldIdx) ? 1 : -1;
        if (::SendMessageW(view, SCI_KEX_SLIDE, static_cast<WPARAM>(dir), _ms)) ++_started;
        _lastDir = dir;
    }

    static bool animationsOn()
    {
        BOOL anim = TRUE;
        return !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) || anim;
    }

    int _ms = 200;
    bool _respectAnim = true;
    int _seen = 0, _started = 0, _skipped = 0, _lastDir = 0;
};

} // namespace

Module* createTabSlide() { return new TabSlide(); }

SlideStats slideStats(HWND view)
{
    SlideStats st;
    if (s_self) { st.seen = s_self->_seen; st.started = s_self->_started; st.skipped = s_self->_skipped; st.lastDir = s_self->_lastDir; }
    KexSlideInfo info{};
    if (view) st.active = ::SendMessageW(view, SCI_KEX_GETSLIDEINFO, 0, reinterpret_cast<LPARAM>(&info)) != 0;
    st.slides = info.slides; st.frames = info.frames; st.captures = info.captures; st.cancelled = info.cancelled; st.stopReason = info.stopReason; st.fraction = info.fraction;
    return st;
}

void slideTestFreeze(double fraction)
{
    const WPARAM w = fraction < 0 ? static_cast<WPARAM>(-1) : static_cast<WPARAM>(fraction * 1000);
    for (int v = 0; v < views.count(); ++v)
        if (HWND s = views.sci(v)) ::SendMessageW(s, SCI_KEX_SLIDETEST, w, 0);
}

} // namespace kex

namespace kexsuite {

void bufferSwitching(HWND view, void* oldBuffer, void* newBuffer)
{
    try
    {
        if (kex::swipeSwitching(view)) return;   // a touchpad swipe moves this slide itself
        if (kex::tabFoldSwitching(view, oldBuffer, newBuffer)) return;   // its tab closed: the document fades out instead
        if (kex::s_self) kex::s_self->switching(view, oldBuffer, newBuffer);
    }
    catch (...) { kex::log(L"Tab slide: exception while switching documents"); }
}

} // namespace kexsuite
