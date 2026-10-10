// Drawing between ticks.
//
// The engine runs at 62.5 ticks a second (MAIN_Loop's 1000/60 ms), and the
// original drew exactly one picture per tick: a whole-tick state, blitted
// with no vsync.  On a faster display the frames between two ticks would
// only repeat the last one.  Instead a frame there draws the ramps that are
// running - fades, slides, the typewriter, the menu's fade - at the count
// the next tick is heading for, `phase` of the way there.
//
// Only presentation: the engine's state is never written, the art target
// keeps exactly what the last tick drew (the original never clears its
// framebuffer, and some effects build up on what the last tick left), and a
// frame that lands on a tick draws exactly what it always drew.  Anything
// whose look is its per-tick sampling stays stepped - every shake, the
// petals, the click indicator, wave characters, the pattern and mesh
// dissolves of the effects-off setting.

#include "game.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace th2app {

double Game::presentation_phase() const
{
    // TH2_SUBTICK_TEST_PHASE: a trace that draws every tick's frame-between
    // too, at this phase, as a check that doing so changes nothing the
    // engine keeps - its state trace and its art-target dumps have to come
    // out byte-identical to a plain run.  TH2_SUBTICK_DUMP keeps the
    // frames-between for looking at.
    if (trace_mode_) {
        static const double forced = [] {
            const char* v = SDL_getenv("TH2_SUBTICK_TEST_PHASE");
            return v ? std::clamp(std::strtod(v, nullptr), 0.0, 0.999) : 0.0;
        }();
        return forced;
    }
    if (soak_ || !config_.smooth_motion || !app_active_
        || character_control_time_ == std::chrono::steady_clock::time_point{}) {
        return 0.0;
    }
    // The ticks were counted at the top of the frame; the picture is for
    // now, a little later.
    const double since = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - character_control_time_).count()
        / engine_tick_ms;
    return std::clamp(character_control_debt_ + since, 0.0, 0.999);
}

bool Game::subtick_scene_steps() const
{
    // The next tick runs the AVG_GAME chain over the scene, and its effects
    // take more than that one tick: not in the system menu's freeze, not
    // while skipping (every AVG_EffCnt is 0 and everything finishes at
    // once), not with effects off.  Nor during a shake, whose look is its
    // per-tick sampling, or under the screens that build up on the last
    // picture (the calendar, the clock) or are drawn by the port alone.
    if (ui_mode_ != UiMode::game || engine_config_step_ || message_cut()
        || config_.effect_speed == 0 || back().sk_flag || calendar_state_
        || clock_state_ || movie_ || name_input_open_ || config_open_) {
        return false;
    }
    // The two zooms with no snapshot draw over the last picture too.
    if (back().fd_flag
        && (back().fd_type == th2::bak_cfzoom1
            || back().fd_type == th2::bak_cfzoom4)) {
        return false;
    }
    return true;
}

bool Game::subtick_menu_steps() const
{
    return ui_mode_ == UiMode::game && engine_config_.flag
        && !config_open_ && effect_frames(-1) > 0;
}

void Game::present_graph(int gno, th2::Graph& graph) const
{
    const double phase = presentation_phase_;
    if (present_scene_) {
        chars().present_graph(gno, phase, graph);
        avgback().present_back_change(gno, phase, graph);
        int r = 0;
        int g = 0;
        int b = 0;
        const auto bright = [&graph](int red, int green, int blue) {
            graph.r = std::clamp(red, 0, 255);
            graph.g = std::clamp(green, 0, 255);
            graph.b = std::clamp(blue, 0, 255);
        };
        // The half tone's wash, then AVG_ControlBackFade over it: that is
        // the order the chain writes GRP_BACK's brightness in a tick, so the
        // colour fade is the one that shows when both run.
        if (gno == th2::grp_back && msg().present_half_tone(phase, r, g, b)) {
            bright(r, g, b);
        }
        const bool back_fade_graph = gno == th2::grp_back
            || (gno >= th2::grp_script && gno < th2::grp_ending)
            || (gno > th2::grp_spback && gno <= th2::grp_spback + th2::max_char);
        if (back_fade_graph && avgback().present_back_fade(phase, r, g, b)) {
            bright(r, g, b);
        }
        // AVG_ColtrolFade's DSP_SetGraphBright( GRP_DISP, r, g, b ).
        if (gno == th2::grp_disp && avgback().present_fade(phase, r, g, b)) {
            bright(r, g, b);
        }
    }
    if (present_menu_) {
        present_engine_config_graph(gno, phase, graph);
    }
}

void Game::end_presentation()
{
    presentation_phase_ = 0.0;
    scroll_present_extra_ = 0.0;
    choice_present_extra_ = 0.0;
    present_scene_ = false;
    present_menu_ = false;
    subtick_drawn_ = false;
    if (avg_msg_) {
        msg().set_presentation_phase(0.0);
    }
    display().set_presenter({});
    if (upscaler_) {
        upscaler_->set_art_source(nullptr);
    }
}

SDL_Texture* Game::ensure_subtick_art()
{
    SDL_Texture* const art = upscaler_->art_target();
    if (subtick_art_) {
        return subtick_art_.get();
    }
    subtick_art_.reset(SDL_CreateTexture(
        renderer_, art->format, SDL_TEXTUREACCESS_TARGET, art->w, art->h));
    if (!subtick_art_) {
        return nullptr;
    }
    SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(art, &blend);
    SDL_SetTextureBlendMode(subtick_art_.get(), blend);
    return subtick_art_.get();
}

}  // namespace th2app
