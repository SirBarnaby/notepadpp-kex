// Effects.cpp - the shared effects renderer (Effects.h): one thread, one layered window sized to the
// pieces that are moving, shown only while there are any.
#include "Effects.h"
#include <atomic>
#include <cmath>
#include <deque>
#include <map>

namespace kex {
namespace fx {

namespace {

struct Live {
    Item it;
    float age = 0;
    float x = 0, y = 0, a = 0, alpha = 1, sx = 1, sy = 1;
    float radius = 0;   // half the diagonal: the piece stays inside x +- radius at any angle
    float w = 0;        // the picture's width (for the anchor of a piece that changes size)
};

class Renderer {
public:
    Renderer() { ::InitializeCriticalSection(&_lock); }
    ~Renderer() { stop(); ::DeleteCriticalSection(&_lock); }

    bool start()
    {
        if (_thread) return _wnd != nullptr;
        _quit = false;
        _event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        _thread = ::CreateThread(nullptr, 0, threadMain, this, 0, nullptr);
        if (!_thread) return false;
        ::WaitForSingleObject(_ready, 3000);
        return _wnd != nullptr;
    }

    void stop()
    {
        if (!_thread) return;
        _quit = true;
        ::SetEvent(_event);
        if (::WaitForSingleObject(_thread, 3000) == WAIT_TIMEOUT) ::TerminateThread(_thread, 0);
        ::CloseHandle(_thread); _thread = nullptr;
        ::CloseHandle(_event); _event = nullptr;
        ::CloseHandle(_ready); _ready = nullptr;
        ::EnterCriticalSection(&_lock);
        _items.clear(); _letters.clear(); _clears.clear();
        ::LeaveCriticalSection(&_lock);
        for (int k = 0; k < K_COUNT; ++k) _live[k] = 0;
        _visible = false;
    }

    void submit(std::vector<Item>&& items)
    {
        if (!_thread || items.empty()) return;
        ::EnterCriticalSection(&_lock);
        for (Item& i : items) { ++_spawned[std::max(0, std::min(K_COUNT - 1, i.kind))]; _items.push_back(std::move(i)); }
        ::LeaveCriticalSection(&_lock);
        ::SetEvent(_event);
    }

    void submitLetters(LetterBatch&& b)
    {
        if (!_thread || b.items.empty()) return;
        ::EnterCriticalSection(&_lock);
        _spawned[std::max(0, std::min(K_COUNT - 1, b.kind))] += static_cast<int>(b.items.size());
        _letters.push_back(std::move(b));
        ::LeaveCriticalSection(&_lock);
        ::SetEvent(_event);
    }

    void clear(int kind)
    {
        if (!_thread) return;
        ::EnterCriticalSection(&_lock);
        _clears.push_back(kind);
        ::LeaveCriticalSection(&_lock);
        ::SetEvent(_event);
    }

    Stats stats()
    {
        Stats st;
        for (int k = 0; k < K_COUNT; ++k) { st.live[k] = _live[k]; st.spawned[k] = _spawned[k]; }
        st.frames = _frames;
        st.visible = _visible;
        ::EnterCriticalSection(&_lock);
        st.box = _box; st.frameMs = _avgMs; st.lastFrameMs = _lastMs;
        ::LeaveCriticalSection(&_lock);
        return st;
    }

    std::atomic<int> freeze{ -1 };
    int users = 0;

private:
    static DWORD WINAPI threadMain(LPVOID p) { static_cast<Renderer*>(p)->run(); return 0; }

    HFONT font(const FontSpec& f)
    {
        auto it = _fonts.find(f);
        if (it != _fonts.end()) return it->second;
        LOGFONTW lf{};
        lf.lfHeight = -f.height;
        lf.lfWeight = f.weight;
        lf.lfItalic = f.italic ? TRUE : FALSE;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcsncpy(lf.lfFaceName, f.face.c_str(), LF_FACESIZE - 1);
        HFONT h = ::CreateFontIndirectW(&lf);
        _fonts[f] = h;
        return h;
    }

    float rnd() { _seed = _seed * 1664525u + 1013904223u; return static_cast<float>((_seed >> 8) & 0xFFFF) / 65535.0f; }

    void addLetters(LetterBatch& b)
    {
        // baseline: Scintilla draws text at line top + maxAscent (+ extra ascent)
        HDC dc = ::CreateCompatibleDC(nullptr);
        TEXTMETRICW tm{};
        HGDIOBJ of = ::SelectObject(dc, font(b.fonts.empty() ? FontSpec{} : b.fonts[0]));
        ::GetTextMetricsW(dc, &tm);
        ::SelectObject(dc, of);
        ::DeleteDC(dc);
        const int baseline = b.extraAscent + tm.tmAscent;
        for (CapItem& ci : b.items)
        {
            std::vector<TextRun> runs;
            for (CapRun& r : ci.runs)
            {
                TextRun t; t.text = r.text; t.dx = r.dx; t.color = r.color;
                t.font = font(b.fonts[r.font >= 0 && r.font < static_cast<int>(b.fonts.size()) ? r.font : 0]);
                runs.push_back(t);
            }
            auto sp = std::make_shared<Sprite>();
            if (!renderRuns(*sp, runs, std::max(1, ci.w), std::max(1, ci.h), baseline, 3, b.back, b.backAlpha)) continue;
            Item it;
            it.sprite = sp;
            it.x0 = ci.x + ci.w * 0.5f;
            it.y0 = ci.y + ci.h * 0.5f;
            if (b.motion != MOTION_FALL)
            {
                // a ghost of the text where it was: cut text lifts away (easing out), deleted text shrinks into its left edge
                const float T = std::max(0.05f, b.duration);
                it.life = T;
                it.opacity = b.opacity;
                it.monitor = b.monitor;
                it.kind = b.kind;
                if (b.motion == MOTION_LIFT)
                {
                    it.vy = -2.0f * b.lift / T;
                    it.g = 2.0f * b.lift / (T * T);
                    it.sx1 = it.sy1 = 1.04f;
                    it.keys = { { 0, 1 }, { T * 0.2f, 1 }, { T, 0 } };
                }
                else
                {
                    it.sx1 = 0.86f;
                    it.sy1 = 0.86f;
                    it.anchorX = static_cast<float>(3) / static_cast<float>(sp->w);   // the text's left edge, inside the padding
                    it.keys = { { 0, 1 }, { T * 0.15f, 1 }, { T, 0 } };
                }
                add(std::move(it));
                continue;
            }
            const float r1 = rnd(), r2 = rnd(), r3 = rnd();
            it.vx = (r1 * 2 - 1) * b.drift;
            it.vy = -(0.25f + 0.75f * r2) * b.pop;
            it.spin = (r3 * 2 - 1) * b.spin;
            it.g = b.gravity;
            it.opacity = b.opacity;
            it.monitor = b.monitor;
            it.kind = K_LETTER;
            add(std::move(it));
        }
        // too many letters: the oldest go first
        int letters = 0;
        for (const Live& l : _parts) if (l.it.kind == K_LETTER) ++letters;
        for (auto i = _parts.begin(); letters > b.maxLive && i != _parts.end();)
            if (i->it.kind == K_LETTER) { i = _parts.erase(i); --letters; } else ++i;
    }

    void add(Item&& it)
    {
        Live l;
        l.it = std::move(it);
        const float w = l.it.rect ? l.it.rw : (l.it.sprite ? static_cast<float>(l.it.sprite->w) : 0.0f);
        const float h = l.it.rect ? l.it.rh : (l.it.sprite ? static_cast<float>(l.it.sprite->h) : 0.0f);
        l.radius = l.it.rect ? 0.0f : 0.5f * std::sqrt(w * w + h * h) + 1;
        l.w = w;
        l.x = l.it.x0; l.y = l.it.y0;
        _parts.push_back(std::move(l));
    }

    static float keyAlpha(const std::vector<Key>& keys, float t)
    {
        if (keys.empty()) return 1;
        if (t <= keys.front().t) return keys.front().a;
        for (size_t i = 1; i < keys.size(); ++i)
            if (t <= keys[i].t)
            {
                const float span = keys[i].t - keys[i - 1].t;
                const float f = span > 0 ? (t - keys[i - 1].t) / span : 1;
                return keys[i - 1].a + (keys[i].a - keys[i - 1].a) * f;
            }
        return keys.back().a;
    }

    void run()
    {
        _wnd = createOverlayWindow(nullptr, true, L"KEX effects");
        ::SetEvent(_ready);
        if (!_wnd) return;
        typedef HRESULT(WINAPI * DwmFlushFn)();
        HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
        DwmFlushFn dwmFlush = dwm ? reinterpret_cast<DwmFlushFn>(::GetProcAddress(dwm, "DwmFlush")) : nullptr;
        bool shown = false;
        double last = nowMs();
        double windowStart = last, windowMs = 0; int windowFrames = 0;
        auto hide = [&]() {
            if (shown) { ::ShowWindow(_wnd, SW_HIDE); shown = false; }
            _visible = false;
        };
        while (!_quit)
        {
            MSG m;
            while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&m); ::DispatchMessageW(&m); }
            std::deque<Item> items;
            std::deque<LetterBatch> letters;
            std::vector<int> clears;
            ::EnterCriticalSection(&_lock);
            items.swap(_items); letters.swap(_letters); clears.swap(_clears);
            ::LeaveCriticalSection(&_lock);
            for (int k : clears)
                for (auto i = _parts.begin(); i != _parts.end();) if (i->it.kind == k) i = _parts.erase(i); else ++i;
            for (Item& i : items) add(std::move(i));
            for (LetterBatch& b : letters) addLetters(b);

            if (_parts.empty())
            {
                hide();
                for (int k = 0; k < K_COUNT; ++k) _live[k] = 0;
                ::MsgWaitForMultipleObjects(1, &_event, FALSE, INFINITE, QS_ALLINPUT);
                last = nowMs();
                continue;
            }
            const double now = nowMs();
            float dt = static_cast<float>((now - last) / 1000.0);
            last = now;
            if (dt > 0.05f) dt = 0.05f;
            const int fz = freeze.load();
            if (fz >= 0)
            {
                float youngest = 1e9f;
                for (const Live& l : _parts) youngest = std::min(youngest, l.age - l.it.delay);
                if (youngest * 1000.0f >= fz) dt = 0;   // test hook
            }

            const double t0 = nowMs();
            RECT bb{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
            int live[K_COUNT] = {};
            for (auto i = _parts.begin(); i != _parts.end();)
            {
                Live& p = *i;
                p.age += dt;
                const float t = p.age - p.it.delay;
                if (t < 0) { ++live[p.it.kind]; ++i; continue; }
                p.x = p.it.x0 + p.it.vx * t;
                p.y = p.it.y0 + p.it.vy * t + 0.5f * p.it.g * t * t;
                p.a = p.it.angle + p.it.spin * t;
                p.alpha = keyAlpha(p.it.keys, t) * p.it.opacity;
                p.sx = p.sy = 1;
                if (p.it.life > 0 && (p.it.sx0 != 1 || p.it.sx1 != 1 || p.it.sy0 != 1 || p.it.sy1 != 1))
                {
                    const float u = std::min(1.0f, t / p.it.life), e = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
                    p.sx = p.it.sx0 + (p.it.sx1 - p.it.sx0) * e;
                    p.sy = p.it.sy0 + (p.it.sy1 - p.it.sy0) * e;
                    p.x += (p.it.anchorX - 0.5f) * p.w * (1.0f - p.sx);
                }
                const float grow = std::max(p.sx, p.sy);
                const float rx = p.it.rect ? p.it.rw * 0.5f : p.radius * grow, ry = p.it.rect ? p.it.rh * 0.5f : p.radius * grow;
                const RECT& mon = p.it.monitor;
                const bool gone = (p.it.life > 0 && t >= p.it.life) || t > 8.0f ||
                                  (p.it.life <= 0 && ((p.y - ry > mon.bottom) || (p.x + rx < mon.left - 400) || (p.x - rx > mon.right + 400)));
                if (gone) { i = _parts.erase(i); continue; }
                ++live[p.it.kind];
                if (p.alpha > 0.002f)
                {
                    bb.left = std::min(bb.left, static_cast<LONG>(std::floor(p.x - rx)));
                    bb.top = std::min(bb.top, static_cast<LONG>(std::floor(p.y - ry)));
                    bb.right = std::max(bb.right, static_cast<LONG>(std::ceil(p.x + rx)));
                    bb.bottom = std::max(bb.bottom, static_cast<LONG>(std::ceil(p.y + ry)));
                }
                ++i;
            }
            for (int k = 0; k < K_COUNT; ++k) _live[k] = live[k];
            if (_parts.empty()) continue;
            RECT vs{ ::GetSystemMetrics(SM_XVIRTUALSCREEN), ::GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
            vs.right = vs.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
            vs.bottom = vs.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
            RECT clip;
            if (bb.left >= bb.right || !::IntersectRect(&clip, &bb, &vs))
            {
                hide();
                waitFrame(dwmFlush, now);
                continue;
            }
            const int w = clip.right - clip.left, h = clip.bottom - clip.top;
            if (_surf.ensure(w, h))
            {
                _surf.clear();
                for (const Live& p : _parts)
                {
                    if (p.age < p.it.delay || p.alpha <= 0.002f) continue;
                    if (p.it.rect)
                        fillRectF(_surf, p.x - p.it.rw * 0.5f - clip.left, p.y - p.it.rh * 0.5f - clip.top,
                                  p.x + p.it.rw * 0.5f - clip.left, p.y + p.it.rh * 0.5f - clip.top, p.it.color, p.alpha);
                    else if (p.it.sprite)
                        drawSprite(_surf, *p.it.sprite, p.x - clip.left, p.y - clip.top, p.a, p.alpha, p.sx, p.sy);
                }
                POINT pos{ clip.left, clip.top }; SIZE sz{ w, h };
                pushLayered(_wnd, _surf, pos, sz);
                if (!shown)
                {
                    ::SetWindowPos(_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    shown = true;
                }
                _visible = true;
                ++_frames;
                const double spent = nowMs() - t0;
                windowMs += spent; ++windowFrames;
                ::EnterCriticalSection(&_lock);
                _box = clip;
                _lastMs = spent;
                if (now - windowStart > 3000 || windowFrames >= 180)
                {
                    _avgMs = windowMs / windowFrames;
                    windowStart = now; windowMs = 0; windowFrames = 0;
                }
                else if (_avgMs == 0 && windowFrames >= 10) _avgMs = windowMs / windowFrames;
                ::LeaveCriticalSection(&_lock);
            }
            waitFrame(dwmFlush, now);
        }
        if (_wnd) { ::DestroyWindow(_wnd); _wnd = nullptr; }
        _visible = false;
        _parts.clear();
        for (auto& f : _fonts) if (f.second) ::DeleteObject(f.second);
        _fonts.clear();
        _surf.release();
        if (dwm) ::FreeLibrary(dwm);
    }

    void waitFrame(HRESULT(WINAPI* dwmFlush)(), double frameStart)
    {
        if (dwmFlush && !underWine)
        {
            const double b = nowMs();
            if (SUCCEEDED(dwmFlush()) && nowMs() - b > 2.0) return;   // paced by the compositor's frames
        }
        const double spent = nowMs() - frameStart;
        const DWORD wait = spent < 15.0 ? static_cast<DWORD>(16.0 - spent) : 1;
        ::MsgWaitForMultipleObjects(1, &_event, FALSE, wait, QS_ALLINPUT);
    }

    HANDLE _thread = nullptr, _event = nullptr, _ready = nullptr;
    volatile bool _quit = false;
    HWND _wnd = nullptr;
    CRITICAL_SECTION _lock;
    std::deque<Item> _items;
    std::deque<LetterBatch> _letters;
    std::vector<int> _clears;
    std::deque<Live> _parts;
    std::map<FontSpec, HFONT> _fonts;
    Surface _surf;
    uint32_t _seed = 0x1234567u;
    std::atomic<int> _live[K_COUNT] = {}, _spawned[K_COUNT] = {}, _frames{ 0 };
    std::atomic<bool> _visible{ false };
    RECT _box{};
    double _avgMs = 0, _lastMs = 0;
};

Renderer& renderer()
{
    static Renderer r;
    return r;
}

} // namespace

bool acquire()
{
    Renderer& r = renderer();
    if (!r.start()) { r.stop(); return false; }
    ++r.users;
    return true;
}

void release()
{
    Renderer& r = renderer();
    if (r.users > 0 && --r.users == 0) r.stop();
}

void submit(std::vector<Item>&& items) { renderer().submit(std::move(items)); }
void submitLetters(LetterBatch&& batch) { renderer().submitLetters(std::move(batch)); }
void clear(int kind) { renderer().clear(kind); }
Stats stats() { return renderer().stats(); }
void freezeAfter(int ms) { renderer().freeze = ms; }

bool animationsOn()
{
    BOOL anim = TRUE;
    return !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) || anim;
}

std::shared_ptr<const Sprite> dotSprite(float radius, COLORREF c)
{
    auto sp = std::make_shared<Sprite>();
    const int n = static_cast<int>(std::ceil(radius * 2)) + 2;
    sp->w = sp->h = n;
    sp->px.assign(static_cast<size_t>(n) * n, 0);
    const float cc = n * 0.5f;
    const uint32_t r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const float d = std::sqrt((x + 0.5f - cc) * (x + 0.5f - cc) + (y + 0.5f - cc) * (y + 0.5f - cc));
            const float cov = std::max(0.0f, std::min(1.0f, radius - d + 0.5f));   // anti-aliased edge
            if (cov <= 0) continue;
            const uint32_t a = static_cast<uint32_t>(cov * 255.0f + 0.5f);
            sp->px[static_cast<size_t>(y) * n + x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
        }
    return sp;
}

} // namespace fx

std::wstring effectsInfo()
{
    const fx::Stats st = fx::stats();
    wchar_t b[320];
    if (!st.frames)
        swprintf(b, 320, L"The falling letters, flashes and splashes are drawn on a thread of their own, in a window that only exists while something moves; Windows' compositor puts it on screen with the graphics card.");
    else
        swprintf(b, 320, L"The falling letters, flashes and splashes are drawn on a thread of their own (%d frames so far, %.2f ms of drawing per frame lately, the last one %.2f ms); Windows' compositor puts them on screen with the graphics card.",
                 st.frames, st.frameMs, st.lastFrameMs);
    return b;
}

} // namespace kex
