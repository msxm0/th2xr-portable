#pragma once

// Trace mode: run the engine as a pure function of a tick counter and an
// input script, and write out what it draws.
//
// The reference build does exactly the same thing (reference/shim), reading
// the same script format and writing the same frame format, so the two can be
// compared tick by tick.  The point of both is that a tick is a unit of
// engine progress rather than of real time: a tick taking three milliseconds
// here and four hundred under Wine changes nothing about what is computed,
// because neither side is allowed to look at a clock.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace th2app {

// One line of an input script.  The format is the reference's:
//
//     150 move 400 403     pointer to (400,403) in 800x600 client coordinates
//     200 lclick 3         left button held three ticks
//     320 enter            a single-tick key press
//     420 ctrl 90          a key held ninety ticks
//     30 every 30 lclick 3 that click, repeated every thirty ticks forever
//
// `every` exists because a route is not a window.  Clicking through one takes
// hundreds of thousands of ticks, and spelling that out one line per click
// costs the reference shim a fixed-size table it silently overflows and both
// sides a scan of every event on every tick - quadratic in the length of the
// run, on the one run long enough to care.  A repeat is one event whose
// firing is arithmetic.
struct TraceEvent {
    enum class Kind { key, move, lclick, rclick, mappick };
    std::uint64_t tick = 0;
    Kind kind = Kind::key;
    std::string key;          // for Kind::key
    int x = 0, y = 0;         // for Kind::move
    std::uint64_t hold = 1;
    // 0 is a one-shot.  Otherwise the event repeats every `period` ticks from
    // `tick` onwards, holding for `hold` out of every `period`.
    std::uint64_t period = 0;
};

// What the script says is true on a given tick.
struct TraceInputState {
    bool click = false;       // left button edge
    bool cancel = false;      // right button edge
    bool click_held = false;
    int mouse_x = 0, mouse_y = 0;
    // Keys held this tick, by the reference's names: enter, space, esc, bs,
    // ctrl, shift, alt, up, down, left, right, pup, pdown, home, end, and
    // num0..num9 for the number row.
    std::vector<std::string> held;
    std::vector<std::string> pressed;   // only on the first tick of a hold

    // "mappick": stand on a map destination, whichever page it is on.  The
    // map is answered by the pointer being inside a destination's rect when a
    // click lands, and those rects are per scene, so the script asks for the
    // first selectable one rather than naming a coordinate.
    bool map_pick = false;

    bool is_pressed(std::string_view name) const;
    bool is_held(std::string_view name) const;
};

class TraceScript {
public:
    // Throws if the file cannot be read or names a key the reference does not
    // know - a typo should fail the run, not silently do nothing.
    void load(const std::filesystem::path& path);
    TraceInputState at(std::uint64_t tick) const;
    std::size_t size() const { return events_.size(); }

private:
    std::vector<TraceEvent> events_;
};

// Writes one frame in the reference's format: a text header line
// "TH2REFZ <w> <h> 24 <key> <bytes>\n" followed by a deflate stream of
// either the picture (key) or its per-byte difference from the previous
// tick's (not key).  The difference is mod 256 and exactly reversible - this
// is a storage format, not a codec, and nothing about a pixel is
// approximated, because the comparison downstream tolerates two levels of
// difference and must never be handed one it created itself.
//
// Raw, a frame is 800*600*3 = 1.37MB, and that is what used to cap a
// comparison window at 300 ticks and force the harness to replay the whole
// game to reach each one.  Successive frames differ in the glyphs the
// typewriter has just drawn, so the difference deflates about eight to one
// and a twelve thousand tick window fits in about 2GB a side.
//
// A key frame is written when there is no predecessor to subtract - the
// first tick of a window, or after a gap - and every 120 ticks after that,
// so reading a single tick costs at most that many decodes.  The caller
// passes the tick for exactly that reason; passing them out of order or with
// gaps is safe, it only costs key frames.
bool write_trace_frame(
    const std::filesystem::path& path, std::uint64_t tick,
    const std::uint8_t* bgr, int width, int height);

}  // namespace th2app
