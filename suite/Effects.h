// Effects.h - the screen effects of the KEX features (falling letters, paint splashes, the copy flash,
// the cut and delete ghosts) are drawn by one renderer on a thread of its own, into a click-through,
// non-activating topmost layered window that only exists while something moves. Windows' compositor
// puts that window on the screen with the graphics card; the pieces themselves are drawn in software
// (fixed-point, only the pixels a piece covers), which costs about a millisecond a frame for 200 letters.
#pragma once
#include "Overlay.h"
#include <memory>

namespace kex {
namespace fx {

enum Kind { K_LETTER = 0, K_DROP, K_FLASH, K_GHOST, K_COUNT };

struct Key { float t, a; };          // opacity keyframe: at t seconds, opacity a (linear in between)

// One moving piece: a picture, or a plain rectangle (rect = true, rw x rh in `color`). Its centre
// follows x0 + vx t, y0 + vy t + g t^2 / 2 (screen pixels), it turns by spin radians a second, and its
// opacity follows the keyframes (1 without any, the last one held) times `opacity`.
struct Item {
    std::shared_ptr<const Sprite> sprite;
    bool rect = false;
    float rw = 0, rh = 0;
    COLORREF color = 0;
    float x0 = 0, y0 = 0, vx = 0, vy = 0, g = 0, spin = 0, angle = 0;
    float delay = 0;                  // seconds before it appears
    float opacity = 1;
    std::vector<Key> keys;
    float life = 0;                   // seconds; 0 = until it has fallen off its monitor (8 s at most)
    RECT monitor{};
    int kind = K_LETTER;
    // size: from sx0/sy0 to sx1/sy1 over its life, easing out; anchorX is the point across the piece that
    // stays where it is (0 its left edge, 0.5 its centre)
    float sx0 = 1, sx1 = 1, sy0 = 1, sy1 = 1, anchorX = 0.5f;
};

// Letters: the text is turned into pictures on the effects thread (GDI text in the editor's fonts).
struct FontSpec {
    std::wstring face; int height = 13; int weight = 400; bool italic = false;
    bool operator<(const FontSpec& o) const
    {
        if (face != o.face) return face < o.face;
        if (height != o.height) return height < o.height;
        if (weight != o.weight) return weight < o.weight;
        return italic < o.italic;
    }
};
struct CapRun { std::wstring text; int dx = 0; COLORREF color = 0; int font = 0; };
struct CapItem { int x = 0, y = 0, w = 0, h = 0; std::vector<CapRun> runs; };
// How the pieces of a batch move: letters fall; a ghost of cut text lifts away; a ghost of deleted text
// shrinks into its left edge.
enum Motion { MOTION_FALL = 0, MOTION_LIFT, MOTION_SHRINK };
struct LetterBatch {
    std::vector<FontSpec> fonts;     // fonts[0] = STYLE_DEFAULT font (baseline reference)
    std::vector<CapItem> items;
    RECT monitor{};
    int extraAscent = 0;
    float gravity = 2200, drift = 90, spin = 4.0f, pop = 220, opacity = 1;
    int maxLive = 400;               // letters on screen at most: the oldest go first
    int motion = MOTION_FALL;
    int kind = K_LETTER;
    COLORREF back = 0;               // a box behind each piece's text (ghosts: the selection), backAlpha 0 = none
    float backAlpha = 0;
    float duration = 0.3f;           // ghosts: seconds
    float lift = 12;                 // MOTION_LIFT: pixels up
};

struct Stats {
    int live[K_COUNT] = {};           // pieces on screen now
    int spawned[K_COUNT] = {};        // pieces handed to the renderer so far
    int frames = 0;                   // frames drawn
    bool visible = false;             // the effects window is shown
    RECT box{};                       // the screen area of the last frame
    double frameMs = 0;               // average time to draw and show a frame (ms), last few seconds
    double lastFrameMs = 0;
};

bool acquire();                       // a feature that draws starts using the renderer (started on first use)
void release();                       // ... stops using it (stopped with the last one)
void submit(std::vector<Item>&& items);
void submitLetters(LetterBatch&& batch);
void clear(int kind);                 // take every piece of that kind off the screen
Stats stats();
void freezeAfter(int ms);             // self-test: pieces stop moving once the newest is ms old (<0 = run)
bool animationsOn();                  // Windows' "Show animations in Windows" (client area animation)

// pictures for the pieces
std::shared_ptr<const Sprite> dotSprite(float radius, COLORREF c);

} // namespace fx
} // namespace kex
