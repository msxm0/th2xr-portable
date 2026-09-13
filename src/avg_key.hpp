#pragma once

// GAME_KEY and AVG_GetGameKey, from GM_avg.h and GM_Avg.cpp.
//
// The engine reads the keyboard once a frame into one struct and everything
// downstream reads that struct rather than the device.  Two kinds of field
// live in it and the difference is the whole point: `trg` fields are edges -
// true on the frame the key went down and never again until it comes back up
// - while `btn` fields are levels.  click, cansel and diswin are edges;
// mes_cut is a level, because holding it is what skipping is.
//
// We were polling SDL_GetModState() in the middle of the frame and calling
// that skipping, which is why the skip key behaved differently depending on
// what else was happening that frame.

namespace th2 {

// GAME_KEY.  The fields the retail game actually reads; u/d/l/r and num[10]
// are the map screen's, which reads SDL events directly.
struct GameKey {
    int click = 0;          // enter or left button, on the edge
    int cansel = 0;         // escape, backspace or right button, on the edge
    int diswin = 0;         // space or middle button: hide the window
    int mes_cut = 0;        // control held - a level, not an edge
    int mes_cut_mode = 0;   // shift, on the edge: toggles skip mode
    int end = 0;
    int home = 0;
    int pup = 0;
    int pdown = 0;
};

// The raw device state AVG_GetGameKey folds into a GameKey.  KeyCond in the
// original, filled by KEY_RenewKeybord; ours is filled from SDL at the top of
// the frame.  `trg` is KeyCond.trg (edge), `btn` is KeyCond.btn (level),
// `btrg` is KeyCond.btrg (edge, auto-repeating) - we do not auto-repeat, so
// btrg and trg are the same thing here.
struct KeyCond {
    bool trg_enter = false;
    bool trg_esc = false;
    bool trg_bs = false;
    bool trg_space = false;
    bool trg_shift = false;
    bool trg_home = false;
    bool trg_end = false;
    bool btrg_pup = false;
    bool btrg_pdown = false;

    bool btn_enter = false;
    bool btn_bs = false;
    bool btn_ctrl = false;
    bool btn_alt = false;

    bool mouse_trg_left = false;
    bool mouse_trg_right = false;
    bool mouse_trg_middle = false;
    bool mouse_btn_left = false;
    bool mouse_btn_right = false;
    int wheel = 0;

    void clear_triggers();
};

// AVG_GetGameKey.  `wheel_mode` is Avg.wheel, which decides whether the
// middle button skips or hides the window.
void get_game_key(GameKey& key, const KeyCond& cond, int wheel_mode);

}  // namespace th2
