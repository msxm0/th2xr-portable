#include "avg_key.hpp"

namespace th2 {

void KeyCond::clear_triggers()
{
    trg_enter = trg_esc = trg_bs = trg_space = trg_shift = false;
    trg_home = trg_end = btrg_pup = btrg_pdown = false;
    mouse_trg_left = mouse_trg_right = mouse_trg_middle = false;
    wheel = 0;
}

void get_game_key(GameKey& key, const KeyCond& cond, int wheel_mode)
{
    const int wclick = cond.btn_enter || cond.mouse_btn_left;
    const int wheel = cond.wheel;

    // Alt+Enter is the fullscreen toggle, so it must not also advance.
    bool trg_enter = cond.trg_enter;
    if (trg_enter && cond.btn_alt) {
        trg_enter = false;
    }

    key.click = trg_enter || cond.mouse_trg_left;
    key.cansel = cond.trg_esc || cond.trg_bs || cond.mouse_trg_right;

    switch (wheel_mode) {
    case 1:
        key.diswin = cond.trg_space;
        key.mes_cut = cond.btn_ctrl;
        key.mes_cut_mode = cond.trg_shift;
        break;
    default:
        key.diswin = cond.trg_space || cond.mouse_trg_middle;
        key.mes_cut = cond.btn_ctrl;
        key.mes_cut_mode = cond.trg_shift;
        break;
    }

    key.end = cond.trg_end && !wclick;
    key.home = cond.trg_home && !wclick;
    if (wheel > 0) {
        key.pup = !wclick;
        key.pdown = 0;
    } else if (wheel < 0) {
        key.pup = 0;
        key.pdown = !wclick;
    } else {
        key.pup = cond.btrg_pup && !wclick;
        key.pdown = cond.btrg_pdown && !wclick;
    }

    // Held combinations resolve to one thing, and the skip key always loses.
    // This is why tapping control while clicking never half-skips in the
    // original: the click wins outright and mes_cut is cleared for the frame.
    if (key.click && key.cansel) {
        key.click = 0;
        key.cansel = 1;
    }
    if (key.click && key.mes_cut) {
        key.mes_cut = 0;
        key.mes_cut_mode = 0;
        key.click = 1;
    }
    if (key.cansel && key.mes_cut) {
        key.mes_cut = 0;
        key.mes_cut_mode = 0;
        key.cansel = 1;
    }
    if (key.end && key.home) {
        key.home = 1;
        key.end = 0;
    }
    if (key.pup && key.pdown) {
        key.pup = 1;
        key.pdown = 0;
    }
}

}  // namespace th2
