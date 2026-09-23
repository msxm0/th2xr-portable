#include "game.hpp"

#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <imgui.h>
#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#define localtime_r(timep, result) localtime_s(result, timep)
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Browsers only grant fullscreen from a user gesture.  SDL's emscripten
// backend asks emscripten to defer the request until the next event handler
// it registered through the html5 API - but SDL 3.4 registers only keyboard
// and wheel handlers there (pointer input goes through plain
// addEventListener), so on a touch screen the deferred request never runs.
// Ask the browser directly instead: SDL keeps its own state in sync from the
// document-level fullscreenchange handler either way.
EM_JS(void, th2_web_set_fullscreen, (int enable), {
    var canvas = Module['canvas'] || document.getElementById('canvas');
    if (!enable) {
        if (document.fullscreenElement && document.exitFullscreen) {
            document.exitFullscreen();
        }
        return;
    }
    if (!canvas || document.fullscreenElement) {
        return;
    }
    var request = canvas.requestFullscreen || canvas.webkitRequestFullscreen;
    if (!request) {
        return;
    }
    // The game loop runs from requestAnimationFrame, so the tap that flipped
    // the option is already over.  Chrome and Firefox still honour the
    // request while the transient activation lasts; if it is refused, retry
    // once from the next real input event.
    var retry = function() {
        window.removeEventListener('pointerdown', retry, true);
        window.removeEventListener('keydown', retry, true);
        if (!document.fullscreenElement) {
            try {
                var again = request.call(canvas);
                if (again && again.catch) {
                    again.catch(function() {});
                }
            } catch (error) {
            }
        }
    };
    var arm = function() {
        window.addEventListener('pointerdown', retry, true);
        window.addEventListener('keydown', retry, true);
    };
    try {
        var result = request.call(canvas);
        if (result && result.catch) {
            result.catch(arm);
        }
    } catch (error) {
        arm();
    }
});

EM_JS(int, th2_web_fullscreen_active, (void), {
    return document.fullscreenElement ? 1 : 0;
});
#endif

namespace th2app {

float Game::bgm_gain(int volume) const
{
    if (suppress_audio_output_ || config_.bgm_muted) {
        return 0.0f;
    }
    return std::clamp(volume, 0, 255) / 255.0f
        * config_.bgm_volume / 256.0f;
}

float Game::se_gain(int volume) const
{
    if (suppress_audio_output_ || config_.se_muted) {
        return 0.0f;
    }
    return std::clamp(volume, 0, 255) / 255.0f
        * config_.se_volume / 256.0f;
}

std::filesystem::path Game::anime4k_shader_dir() const
{
    const auto base = std::filesystem::path(SDL_GetBasePath());
    const auto executable_relative = base / TH2_ANIME4K_SHADER_DIR;
    if (std::filesystem::exists(executable_relative)) {
        return executable_relative;
    }
    return base / ".." / "Resources" / TH2_ANIME4K_SHADER_DIR;
}

std::filesystem::path Game::blend_shader_dir() const
{
    const auto base = std::filesystem::path(SDL_GetBasePath());
    const auto executable_relative = base / TH2_BLEND_SHADER_DIR;
    if (std::filesystem::exists(executable_relative)) {
        return executable_relative;
    }
    return base / ".." / "Resources" / TH2_BLEND_SHADER_DIR;
}

void Game::ensure_upscaler()
{
    if (last_anime4k_wanted_ == config_.anime4k) {
        return;
    }
    last_anime4k_wanted_ = config_.anime4k;
    upscaler_ = th2::create_upscaler(
        renderer_, anime4k_shader_dir(), config_.anime4k,
        &anime4k_available_);
}

std::size_t Game::voice_character_index(int character) const
{
    if (character >= 1 && character <= 9) {
        return static_cast<std::size_t>(character - 1);
    }
    if (character == 28) {
        return 10;
    }
    if (character == 99) {
        return 9;
    }
    return 10;
}

float Game::voice_gain(int volume, int character) const
{
    const auto index = voice_character_index(character);
    if (suppress_audio_output_ || config_.voice_muted
        || config_.character_voice_muted[index]) {
        return 0.0f;
    }
    return std::clamp(volume, 0, 256) / 256.0f
        * config_.voice_volume / 256.0f
        * config_.character_voice_volume[index] / 256.0f;
}

void Game::apply_audio_gains()
{
    bgm_.set_gain(bgm_gain(bgm_volume_));
    for (std::size_t i = 0; i < transient_se_.size(); ++i) {
        transient_se_[i].set_gain(se_gain(transient_se_volume_[i]));
    }
    for (std::size_t i = 0; i < se_channels_.size(); ++i) {
        se_channels_[i].set_gain(se_gain(se_volume_[i]));
    }
    for (std::size_t i = 0; i < voice_channels_.size(); ++i) {
        voice_channels_[i].set_gain(
            voice_gain(voice_volume_[i], voice_character_[i]));
    }
}

void Game::sync_window_config()
{
#ifdef __EMSCRIPTEN__
    // The browser owns the window geometry; only the fullscreen state is
    // ours to track, and that follows the document, not the SDL flags.
    config_.fullscreen = th2_web_fullscreen_active() != 0;
#elif !defined(__ANDROID__)
    if (!window_) {
        return;
    }
    config_.fullscreen =
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    if (config_.fullscreen) {
        return;
    }
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &width, &height);
    if (width > 0 && height > 0) {
        config_.window_x = x;
        config_.window_y = y;
        config_.window_width = width;
        config_.window_height = height;
    }
#endif
}

void Game::toggle_fullscreen()
{
#if defined(__EMSCRIPTEN__)
    config_.fullscreen = th2_web_fullscreen_active() == 0;
    th2_web_set_fullscreen(config_.fullscreen ? 1 : 0);
    th2::save_config(config_path_, config_);
#elif !defined(__ANDROID__)
    config_.fullscreen =
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0;
    SDL_SetWindowFullscreen(window_, config_.fullscreen);
    sync_window_config();
    th2::save_config(config_path_, config_);
#endif
}

void Game::start_movie(int mode, int number, bool resume_script)
{
    std::string name;
    SDL_FRect destination{0.0f, 0.0f, 800.0f, 600.0f};
    switch (mode) {
    case 0:
        name = "TH2_OP_800x448_5M.avi";
        destination = {0.0f, 76.0f, 800.0f, 448.0f};
        break;
    case 1:
        name = std::format("TH2_ED_{:02d}_800_3M.avi", number);
        break;
    case 2:
        name = "TH2_TR_800x600_5M.avi";
        break;
    case 3:
        name = "Leaf_800x600_5M.avi";
        break;
    default:
        throw std::runtime_error("unsupported movie mode");
    }
    // A traced movie is not decoded, because on the other side it cannot be.
    // reference/shim/stubs.cpp answers the engine's decode loop out of
    // th2ref_movie_decoding(), whose TH2REF_MOVIE_TICKS defaults to zero, so
    // every movie in a reference trace is over before it starts.  Ours has a
    // real decoder that a headless trace never presents a frame for, so it
    // never reports finished and the script parks on SetMovie for good - which
    // is exactly what stopped a sweep dead at pc 20897 of 010301000.sdt, the
    // last instruction of the first day.  The same length on both sides is the
    // only way the two are comparable at all; TH2_MOVIE_TICKS matches the
    // reference's knob, and carries the reference's caveat with it - it moves
    // only how long the script is told to wait, and nothing here draws a
    // movie frame either way.
    if (trace_mode_) {
        trace_movie_live_ = true;
        trace_movie_started_ = trace_tick_;
    } else {
        const auto* entry = movie_archive_.find(name);
        if (!entry) {
            throw std::runtime_error("movie not found: " + name);
        }
        movie_bytes_ = movie_archive_.read(*entry);
        movie_ = std::make_unique<th2::VideoPlayer>(
            renderer_, movie_bytes_, destination);
    }
    movie_resume_script_ = resume_script;
    movie_mode_ = mode;
    // AVG_SetMovie's own calls, `change` and all:
    //     case 0: AVG_PlayBGM( 0, 0, FALSE, 255, TRUE );
    // TRUE is what makes it restart a track that is already playing, and
    // without it a movie that follows its own music left the music running.
    if (mode == 0) {
        play_bgm(0, false, 255, 0, true);
    } else if (mode == 1) {
        play_bgm(50, false, 255, 0, true);
    } else if (mode == 2) {
        play_bgm(99, false, 255, 0, true);
    }
}

std::uint64_t Game::trace_movie_ticks()
{
    static const std::uint64_t ticks = [] {
        const char* value = SDL_getenv("TH2_MOVIE_TICKS");
        if (!value || !*value) {
            return std::uint64_t{0};
        }
        const auto parsed = std::atoll(value);
        return parsed > 0 ? static_cast<std::uint64_t>(parsed) : std::uint64_t{0};
    }();
    return ticks;
}

void Game::update_movie()
{
    // AVG_WaitMovie's AVG_StopBGM( 0 ) fires on the pass that first sees the
    // movie finished - and the pass that started it has already run by then,
    // so it is always the frame after.  Ours notices the end inside the same
    // frame it started (see below), so the stop is held over to the next one
    // rather than issued early: the script timing is measured and right, and
    // only the music was a frame ahead of the reference's.
    if (movie_bgm_stop_pending_) {
        movie_bgm_stop_pending_ = false;
        stop_bgm(0);
    }
    if (trace_movie_live_) {
        // Ended on the first poll, exactly as th2ref_movie_decoding() does
        // with TH2REF_MOVIE_TICKS at its default of zero.  The tick the
        // script actually spends on SetMovie comes from the wait, not from
        // here: update_movie runs after pump_script, and the advance() below
        // is not seen until the pass after that - so a zero-length movie
        // still costs the two ticks the reference spends at pc 20897 of
        // 010301000.sdt.  Making it end a poll later instead cost three.
        if (trace_tick_ - trace_movie_started_ >= trace_movie_ticks()) {
            trace_movie_live_ = false;
            complete_movie();
        }
        return;
    }
    if (!movie_) {
        return;
    }
    movie_->update();
    if (!movie_->finished()) {
        return;
    }
    complete_movie();
}

void Game::complete_movie()
{
    const int completed_mode = movie_mode_;
    movie_.reset();
    movie_bytes_.clear();
    movie_mode_ = -1;
    if (completed_mode != 3) {
        movie_bgm_stop_pending_ = true;
    }
    if (movie_resume_script_) {
        movie_resume_script_ = false;
        advance();
    } else if (completed_mode == 3 && runtime_.game_flag(98) != 0) {
        start_movie(0, 0, false);
    } else {
        title_started_ = std::chrono::steady_clock::now();
    }
}

th2::AudioChannel& Game::waited_audio_channel()
{
    if (audio_wait_->kind == AudioWaitKind::bgm) {
        return bgm_;
    }
    if (audio_wait_->kind == AudioWaitKind::voice) {
        return voice_channels_.at(audio_wait_->channel);
    }
    if (audio_wait_->channel < se_channels_.size()) {
        return se_channels_.at(audio_wait_->channel);
    }
    return transient_se_.at(audio_wait_->channel - se_channels_.size());
}


}  // namespace th2app
