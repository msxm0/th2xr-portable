#include "game.hpp"

#include "data_source.hpp"
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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <span>
#include <unordered_set>
#include <vector>

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

// SDL's GLES2 renderer compiles its shaders up front but links each
// vertex/fragment pair the first time a draw needs it - in a browser a
// synchronous round trip to the GPU process for the link status, which landed
// in the middle of a scene the first time the overlay and side bar were drawn.
// One throwaway draw of each kind the game uses links them all now: solid
// fills and the two byte orders of 32-bit texture (RGBA32 is SDL's ABGR
// program, BGRA32 its ARGB one; RGBA8888 textures are converted to one of
// those).  Into a 1x1 target that nothing else sees, so no picture changes.
void Game::warm_renderer_programs()
{
    SDL_Texture* const held = SDL_GetRenderTarget(renderer_);
    th2app::Texture scratch(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_TARGET, 1, 1));
    if (!scratch || !SDL_SetRenderTarget(renderer_, scratch.get())) {
        return;
    }
    const SDL_FRect pixel{0.0f, 0.0f, 1.0f, 1.0f};
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderFillRect(renderer_, &pixel);
    const std::uint32_t texel = 0;
    for (const auto format : {SDL_PIXELFORMAT_RGBA32, SDL_PIXELFORMAT_BGRA32}) {
        th2app::Texture source(SDL_CreateTexture(
            renderer_, format, SDL_TEXTUREACCESS_STATIC, 1, 1));
        if (source && SDL_UpdateTexture(source.get(), nullptr, &texel, 4)) {
            SDL_RenderTexture(renderer_, source.get(), nullptr, &pixel);
            SDL_FlushRenderer(renderer_);
        }
    }
    SDL_SetRenderTarget(renderer_, held);
    SDL_FlushRenderer(renderer_);
    // And the targets the first scenes would otherwise make in the middle of
    // themselves: the thumbnail's, a wipe's two frame copies, and the
    // screen-sized bitmaps the scripts ask for (the backdrop, its half-toned
    // copy, a scratch) - 3 was the most ever live at once over ERRATIC2.
    if (!thumbnail_target_) {
        thumbnail_target_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            save_thumbnail_width, save_thumbnail_height));
    }
    float art_width = 0.0f;
    float art_height = 0.0f;
    if (upscaler_
        && SDL_GetTextureSize(upscaler_->art_target(), &art_width, &art_height)) {
        const int width = static_cast<int>(art_width);
        const int height = static_cast<int>(art_height);
        while (spare_frame_targets_.size() < 2) {
            Texture target(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_TARGET, width,
                                             height));
            if (!target) {
                break;
            }
            spare_frame_targets_.push_back(std::move(target));
        }
        if (display_) {
            display_->reserve_targets(width, height, 3);
        }
    }
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

namespace {

// A movie read out of mov.pak as it plays, a chunk at a time.  The movies
// are stored uncompressed, so an entry's byte range is the .avi itself.
// In the browser every chunk is also an HTTP range, so the next few are
// asked for ahead of the decoder (a no-op natively, where a chunk is a disk
// read of a millisecond).  Reading the whole entry before the first frame
// was 18 ms on the frame the opening movie started, and up to 80 MB of
// wasm heap for an ending.
class ArchiveMovie final : public th2::VideoSource {
public:
    ArchiveMovie(std::filesystem::path path, std::uint64_t offset,
                 std::uint64_t size)
        : path_(std::move(path)), base_(offset), size_(size)
    {
    }

    std::uint64_t size() const override { return size_; }

    bool read(std::uint64_t offset, std::span<std::uint8_t> out) override
    {
        while (!out.empty()) {
            if (offset >= size_) {
                return false;
            }
            const std::uint64_t index = offset / chunk_size;
            if (index != chunk_index_ || chunk_.empty()) {
                if (!load(index)) {
                    return false;
                }
            }
            const auto within = static_cast<std::size_t>(offset - index * chunk_size);
            const auto n = std::min(out.size(), chunk_.size() - within);
            std::copy_n(chunk_.data() + within, n, out.data());
            out = out.subspan(n);
            offset += n;
        }
        return true;
    }

private:
    static constexpr std::uint64_t chunk_size = 1 << 20;
    static constexpr int read_ahead = 3;

    std::uint64_t chunk_length(std::uint64_t index) const
    {
        const auto start = index * chunk_size;
        return start >= size_ ? 0 : std::min(chunk_size, size_ - start);
    }

    bool load(std::uint64_t index)
    {
        const auto length = chunk_length(index);
        if (length == 0) {
            return false;
        }
        chunk_.resize(static_cast<std::size_t>(length));
        if (!th2::data_read(path_, base_ + index * chunk_size, chunk_)) {
            chunk_.clear();
            return false;
        }
        chunk_index_ = index;
        // The demuxer reads forward, apart from the index at the very end
        // that it looks at when it opens the file.
        for (int ahead = 1; ahead <= read_ahead; ++ahead) {
            const auto next = index + static_cast<std::uint64_t>(ahead);
            if (const auto bytes = chunk_length(next)) {
                th2::data_prefetch_pin(path_, base_ + next * chunk_size,
                                       static_cast<std::size_t>(bytes), false);
            }
        }
        return true;
    }

    std::filesystem::path path_;
    std::uint64_t base_;
    std::uint64_t size_;
    std::vector<std::uint8_t> chunk_;
    std::uint64_t chunk_index_ = 0;
};

}  // namespace

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
        if (entry->compressed) {
            // Not in the shipped archive, but a compressed entry's range is
            // an LZS stream, not the file: that one is read whole.
            movie_bytes_ = movie_archive_.read(*entry);
            movie_ = std::make_unique<th2::VideoPlayer>(
                renderer_, movie_bytes_, destination);
        } else {
            const auto range = movie_archive_.range_of(*entry);
            movie_ = std::make_unique<th2::VideoPlayer>(
                renderer_,
                std::make_unique<ArchiveMovie>(range.path, range.offset,
                                               range.size),
                destination);
        }
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
