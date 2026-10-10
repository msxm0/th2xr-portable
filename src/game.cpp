#include "game.hpp"
#include "engine_rand.hpp"

#include "data_source.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_video.h>

extern "C" {
    #include <libavutil/log.h>
}

#include <algorithm>
#include <sstream>
#include <fstream>
#include <format>
#include <limits>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <exception>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace th2app {

// The shader pattern wipe, by default: it avoids a readback per wipe frame,
// which is the difference between a wipe costing nothing and costing a
// pipeline sync twice a frame - and in the browser that matters most.
//
// It earns the default by being exact.  It was not, for a long time, and the
// cause was not arithmetic: the mask was read upside down.  The normalised
// lookup flipped it (v_mask_uv = 1.0 - clip.y) on the assumption that GL's y
// runs opposite to SDL's, which is not true of a render target - SDL's
// projection already lines them up.  Reading row 599-y of the mask instead
// of row y put the wipe front a step out wherever the mask is a gradient.
//
// --cpu-transitions still forces the CPU blend, which is also exact and is
// what the shader was checked against.
bool force_cpu_transitions = false;
// The web build's frame loop: 0 requestAnimationFrame, 1 --frame-step, 2
// --frame-free (see wait_for_next_frame).
int web_frame_pacing = 0;
bool trace_prefetch = false;

namespace {

#ifdef __EMSCRIPTEN__
// Blocks the frame loop until the next frame may start.  JSPI suspends the
// whole call stack on the await, the same mechanism the streaming archive
// reads rely on, so the loop stays shaped like the native one instead of
// becoming a callback.
//
// Normally that is the browser being ready to paint again.  Two debugging
// modes take the loop off requestAnimationFrame:
//   --frame-step  each frame waits for the page to ask for it -
//                 th2Step(n) runs n more frames and returns a promise that
//                 settles once they are drawn, so a harness can capture the
//                 canvas, the trace dumps or the GL state between any two;
//   --frame-free  frames run back to back, yielding to the event loop only
//                 (a trace replay at the machine's speed, as natively).
EM_ASYNC_JS(void, wait_for_next_frame, (int mode), {
    if (mode === 0) {
        await new Promise(requestAnimationFrame);
        return;
    }
    if (mode === 2) {
        await new Promise((resume) => {
            const channel = new MessageChannel();
            channel.port1.onmessage = () => resume();
            channel.port2.postMessage(0);
        });
        return;
    }
    const step = globalThis.th2Frames || (globalThis.th2Frames = (() => {
        const state = { drawn: 0, allowed: 0, resume: null, waiters: [] };
        // Frames drawn so far, and how many may be: th2Step(n) lets n more
        // run and settles when the last of them is drawn.
        globalThis.th2Step = (count = 1) => {
            state.allowed = Math.max(state.allowed, state.drawn) + count;
            const target = state.allowed;
            const done = new Promise((settle) =>
                state.waiters.push({ target, settle }));
            if (state.resume) {
                const resume = state.resume;
                state.resume = null;
                resume();
            }
            return done;
        };
        return state;
    })());
    // A frame has just been drawn.
    step.drawn += 1;
    step.waiters = step.waiters.filter((waiter) => {
        if (waiter.target <= step.drawn) {
            waiter.settle(step.drawn);
            return false;
        }
        return true;
    });
    if (step.drawn >= step.allowed) {
        await new Promise((resume) => { step.resume = resume; });
    }
});

// The loop has ended: every th2Step still waiting settles (with -1), and
// any later one does at once, rather than waiting for a frame that will
// never come.
EM_JS(void, frames_ended, (void), {
    const step = globalThis.th2Frames;
    if (!step) {
        return;
    }
    for (const waiter of step.waiters) {
        waiter.settle(-1);
    }
    step.waiters = [];
    globalThis.th2Step = () => Promise.resolve(-1);
});
#endif

std::filesystem::path ensure_parent_directory(std::filesystem::path path)
{
    std::filesystem::create_directories(path.parent_path());
    return path;
}

}  // namespace

struct SdlSubsystem {
    SdlSubsystem()
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    ~SdlSubsystem() { SDL_Quit(); }

    SdlSubsystem(const SdlSubsystem&) = delete;
    SdlSubsystem& operator=(const SdlSubsystem&) = delete;
};

Game::Game(
    const std::filesystem::path& data,
    const std::optional<std::filesystem::path>& scenario,
    const std::optional<std::filesystem::path>& soak_directory,
    std::size_t soak_runs)
    : scripts_(data / "SDT.PAK"), graphics_(data / "GRP.PAK"),
      backgrounds_(data / "bak.pak"), fonts_(data / "FNT.PAK"),
      bgm_archive_(data / "bgm.PAK"), se_archive_(data / "SE.PAK"),
      voice_archive_(data / "voice.pak"), movie_archive_(data / "mov.pak"),
      runtime_(scripts_),
      config_path_(ensure_parent_directory(soak_directory
          ? *soak_directory / "config.ini"
          : profile_directory() / "toheart2-config.ini")),
      state_path_(ensure_parent_directory(soak_directory
          ? *soak_directory / "state.sqlite3"
          : profile_directory() / "toheart2-state.sqlite3")),
      config_(th2::load_config(config_path_)),
      persistent_state_(state_path_),
      suppress_audio_output_(soak_directory.has_value()),
      font_(fonts_)
{
    SDL_Log("Config path: %s", config_path_.string().c_str());
    SDL_Log("State path: %s", state_path_.string().c_str());
    persistent_game_flags_ = persistent_state_.load_game_flags();
    unlocked_visual_cgs_ = persistent_state_.load_unlocks(
        th2::PersistentState::UnlockKind::visual_cg);
    unlocked_h_cgs_ = persistent_state_.load_unlocks(
        th2::PersistentState::UnlockKind::h_cg);
    unlocked_replays_ = persistent_state_.load_unlocks(
        th2::PersistentState::UnlockKind::replay);
    const auto non_zero_flags = std::ranges::count_if(
        persistent_game_flags_, [](std::int32_t v) { return v != 0; });
    SDL_Log(
        "Loaded %zu non-zero game flags (flag98=%d)",
        static_cast<std::size_t>(non_zero_flags), persistent_game_flags_[98]);
    default_player_name_ =
        th2::load_default_player_name(data / "TOHEART2.EXE");
    player_name_ = default_player_name_;
    for (std::size_t i = 0; i < persistent_game_flags_.size(); ++i) {
        runtime_.set_game_flag(i, persistent_game_flags_[i]);
    }
    SDL_WindowFlags window_flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __EMSCRIPTEN__
    // WebGL 2, asked for as such.  Every shader here is #version 300 es, and
    // SDL's GLES2 renderer otherwise asks for ES 2.0 - WebGL 1 - which this
    // build cannot create (MIN_WEBGL_VERSION is 2), so the page logged a
    // warning and was upgraded anyway.  The renderer keeps a context that is
    // already ES 2.0 or newer.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    window_flags |= SDL_WINDOW_OPENGL;
#endif
    window_ = SDL_CreateWindow(
        "ToHeart2 XRATED", config_.window_width, config_.window_height,
        window_flags);
    window_holder_.reset(window_);
    if (window_ && config_.window_x >= 0 && config_.window_y >= 0) {
        SDL_SetWindowPosition(window_, config_.window_x, config_.window_y);
    }
#ifdef __EMSCRIPTEN__
    // The page sizes the canvas to the viewport, and the saved desktop window
    // size means nothing here, so match the window to the page.  That makes
    // SDL scale the backing store by the device pixel ratio without touching
    // the canvas CSS; later browser resizes are handled by SDL itself,
    // because the window is resizable and the canvas is sized by CSS.
    if (window_) {
        SDL_SetWindowSize(
            window_, EM_ASM_INT({ return window.innerWidth; }),
            EM_ASM_INT({ return window.innerHeight; }));
    }
#endif
    if (window_) {
        SDL_PropertiesID renderer_properties = SDL_CreateProperties();
        // GLES, everywhere.  SDL_GPU has no WebGL backend, so anything built
        // on it is desktop-only by construction and the browser gets a
        // different renderer with a different set of features - two shader
        // stacks, each platform missing half of them.  GLES is the one that
        // runs in both places, and with the exact-blend shader it now
        // measures identically to what SDL_GPU achieved: 343 of 2000 ticks
        // over a two-level gate, the same first tick, the same worst pixel.
        //
        // TH2_RENDERER still overrides it, which is how the two were
        // compared on one machine.
        const char* wanted = SDL_getenv("TH2_RENDERER");
        SDL_SetStringProperty(
            renderer_properties, SDL_PROP_RENDERER_CREATE_NAME_STRING,
            (wanted && *wanted) ? wanted : "opengles2");
        SDL_SetPointerProperty(
            renderer_properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER,
            window_);
        SDL_SetBooleanProperty(
            renderer_properties,
            SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
        SDL_SetBooleanProperty(
            renderer_properties,
            SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
        renderer_ = SDL_CreateRendererWithProperties(renderer_properties);
        const char* gpu_error = renderer_ ? nullptr : SDL_GetError();
        SDL_DestroyProperties(renderer_properties);
        if (!renderer_) {
            SDL_Log(
                "GPU renderer unavailable, falling back to default renderer: %s",
                gpu_error ? gpu_error : "unknown error");
            renderer_ = SDL_CreateRenderer(window_, nullptr);
        }
    }
    renderer_holder_.reset(renderer_);
    if (renderer_) {
        const char* got = SDL_GetRendererName(renderer_);
        SDL_Log("Renderer backend: %s", got ? got : "?");
    }
    if (!window_ || !renderer_) {
        throw std::runtime_error(SDL_GetError());
    }
#ifndef __EMSCRIPTEN__
    // Present on the display's own refresh, so frames between ticks are
    // drawn at its rate rather than at the engine's (see game_subtick.cpp).
    // Where vsync cannot be had the loop paces itself to the tick instead.
    // A trace turns it off again in enable_trace.
    vsync_paced_ = SDL_SetRenderVSync(renderer_, 1);
#endif
    if (auto icon = th2::load_executable_icon(data / "TOHEART2.EXE")) {
        SDL_SetWindowIcon(window_, icon.get());
    }
    SDL_Log("SDL window and renderer created");
    build_display();
#ifdef __EMSCRIPTEN__
    // Fullscreen in a browser needs a user gesture, so it cannot be restored
    // at startup; the page starts windowed and the option starts off with it.
    config_.fullscreen = false;
#elif !defined(__ANDROID__)
    if (config_.fullscreen) {
        SDL_SetWindowFullscreen(window_, true);
    }
#endif
    upscaler_ = th2::create_upscaler(
        renderer_, anime4k_shader_dir(), config_.anime4k,
        &anime4k_available_);
    last_anime4k_wanted_ = config_.anime4k;
    imgui_ = std::make_unique<th2::ImGuiLayer>(window_, renderer_);
    SDL_Log("ImGui layer initialized");
    // Every one of these is a separate archive entry, and in the browser a
    // separate network round trip.  Asking for them all up front turns a
    // queue of thirty waits into thirty transfers running at once; the loads
    // below then find the bytes waiting for them.
    static constexpr std::array startup_textures{
        "sys0100.tga", "sys0110.tga", "sys0111.tga", "sys0000.tga",
        "sys0001.tga", "sys0011.tga", "sys0010.tga", "sys0200.tga",
        "sys0300.tga", "sys0201.tga", "sys0202.tga", "sys0210.tga",
        "sys0230.tga", "sys0250.tga", "sys0350.tga", "sys0251.tga",
        "sys0203.tga", "t0000.tga", "t0010.tga", "t1000.tga", "t1100.tga",
        "t2000.tga", "t2001.tga", "t2010.tga", "t2020.tga", "t2021.tga",
        "t2100.tga", "t3000.tga", "t0001.tga",
        "f0052.bmp",  // the title screen's transition mask
    };
    for (const auto* asset : startup_textures) {
        if (const auto* entry = graphics_.find(asset)) {
            const auto range = graphics_.range_of(*entry);
            th2::data_prefetch_pin(
                range.path, range.offset, range.size, false);
        }
    }
    // The interface sounds are played by menus rather than by the script, so
    // the lookahead never sees them coming; there are only a handful.
    for (const int sound : {9002, 9012, 9014, 9015, 9104, 9107, 9108, 9111}) {
        if (const auto* entry =
                se_archive_.find(std::format("SE_{:04d}.WAV", sound))) {
            const auto range = se_archive_.range_of(*entry);
            th2::data_prefetch_pin(
                range.path, range.offset, range.size, false);
        }
    }
    auto try_load = [&](std::string_view name) -> Texture {
        const auto* entry = graphics_.find(name);
        if (!entry) return {};
        try {
            return load_texture(renderer_, graphics_, name);
        } catch (...) {
            return {};
        }
    };
    // Every script up front: the walk needs them and they never change, so
    // this is the one place the cost can be paid without a frame noticing.
    preload_scripts();
    ui_sys_menu_bg_ = try_load("sys0100.tga");
    ui_sys_menu_btns_ = try_load("sys0110.tga");
    ui_sys_cancel_ = try_load("sys0111.tga");
    ui_sidebar_track_ = try_load("sys0000.tga");
    ui_sidebar_btns_ = try_load("sys0001.tga");
    ui_keywait_ = try_load("sys0011.tga");
    ui_pageend_ = try_load("sys0010.tga");
    ui_save_bg_ = try_load("sys0200.tga");
    ui_load_bg_ = try_load("sys0300.tga");
    ui_save_rows_ = try_load("sys0201.tga");
    ui_save_rows_hover_ = try_load("sys0202.tga");
    ui_save_new_ = try_load("sys0210.tga");
    ui_save_digits_ = try_load("sys0230.tga");
    ui_save_prompt_ = try_load("sys0250.tga");
    ui_load_prompt_ = try_load("sys0350.tga");
    ui_confirm_buttons_ = try_load("sys0251.tga");
    ui_save_controls_ = try_load("sys0203.tga");
    title_background_ = try_load("t0000.tga");
    title_menu_ = try_load("t0010.tga");
    omake_cg_background_ = try_load("t1000.tga");
    omake_cg_locked_ = try_load("t1100.tga");
    omake_music_background_ = try_load("t2000.tga");
    omake_music_selection_ = try_load("t2001.tga");
    omake_music_labels_ = try_load("t2010.tga");
    omake_music_title_ = try_load("t2020.tga");
    omake_music_artist_ = try_load("t2021.tga");
    omake_music_playing_ = try_load("t2100.tga");
    omake_replay_background_ = try_load("t3000.tga");
    if (const auto* entry = graphics_.find("t0001.tga")) {
        Surface loaded(th2::load_image(graphics_.read(*entry), entry->name));
        // The title's pixel pass reads it as a full 800x600 screen.
        if (loaded && loaded->w >= 800 && loaded->h >= 600) {
            title_foreground_pixels_.reset(
                SDL_ConvertSurface(loaded.get(), SDL_PIXELFORMAT_RGBA32));
        }
    }
    title_mask_ = load_transition_mask(
        0x80 + 52, title_mask_width_, title_mask_height_);
    title_masked_ = Texture(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
        800, 600));
    if (title_masked_) {
        SDL_SetTextureBlendMode(title_masked_.get(), SDL_BLENDMODE_BLEND);
    }
    title_started_ = std::chrono::steady_clock::now();
    // The player's input sampled once a tick, in every run - not only in a
    // trace.  A recording compares exactly this against the reference;
    // normal play is the same machine with nobody keeping the file.
    recorder_.begin_live();
    // The audio channels stay on the wall clock outside a trace: they also
    // time when a sound has really finished playing and when a track's
    // intro hands over to its loop, and the hardware plays in real time.
    if (soak_directory) {
        soak_ = std::make_unique<th2::SoakGameDriver<Game>>(
            *this,
            *soak_directory, soak_runs);
        if (!soak_->start()) {
            running_ = false;
        }
    } else if (scenario) {
        reset_play_state();
        initialize_scenario_flags();
        direct_scenario_ = true;
        runtime_.load_file(*scenario);
        ui_mode_ = UiMode::game;
        advance();
    } else {
        SDL_Log("Starting opening movie");
        start_movie(3, 0, false);
    }
    // One playback device held open for the whole run, unused.  Every
    // channel opens its own stream, and SDL closes the physical device when
    // the last of them goes - which happens between any two sound effects -
    // and its shutdown waits up to a hundred milliseconds for the audio to
    // drain.  On the web that wait is on the main thread, where SDL also
    // feeds the device: a frame hitch whenever the sounds stopped, and the
    // next sound's opening stuttering while the device came back up.
    if (!suppress_audio_output_) {
        audio_keepalive_ =
            SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    }
    SDL_Log("Game constructor finished");
}

Game::~Game()
{
    // A recording that ends any way but a closed window - --trace-ticks
    // running out, an error - still gets its open presses written.
    if (recorder_.active()) {
        recorder_.finish(trace_tick_);
    }
    // The trace clock captures `this`, and it lives in a static, so it has
    // to go before this object does.
    th2::AudioChannel::set_clock(nullptr);
    if (audio_keepalive_ != 0) {
        SDL_CloseAudioDevice(audio_keepalive_);
    }
    // All SDL resources are owned by members declared after
    // window_holder_/renderer_holder_, so they are destroyed before the
    // renderer/window and before SDL_Quit() - except the wipe shader, which
    // is declared among the transitions and makes raw GL calls in its
    // destructor.  After the renderer has taken the context with it those
    // throw in WebGL, and a throw out of a destructor is std::terminate: the
    // browser build aborted on every exit.  It goes first, explicitly.
    gl_transition_.reset();
    sync_window_config();
    th2::save_config(config_path_, config_);
}

int Game::run()
{
    try {
        return run_loop();
    } catch (const std::exception& error) {
        if (soak_) {
            soak_->fail(error.what());
        }
        throw;
    }
}

bool Game::is_confirm_key(SDL_Keycode key)
{
    return key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE;
}

bool Game::is_alt_enter(const SDL_KeyboardEvent& key)
{
    return (key.mod & SDL_KMOD_ALT) != 0
        && (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER);
}

int Game::run_loop()
{
    SDL_Log("Entering main loop");
    int dbg_w = 0, dbg_h = 0, dbg_pw = 0, dbg_ph = 0;
    SDL_GetWindowSize(window_, &dbg_w, &dbg_h);
    SDL_GetWindowSizeInPixels(window_, &dbg_pw, &dbg_ph);
    SDL_Log(
        "Window size: %dx%d (points), %dx%d (pixels), display scale %.2f",
        dbg_w, dbg_h, dbg_pw, dbg_ph, SDL_GetWindowDisplayScale(window_));
    next_frame_ = std::chrono::steady_clock::now();
    while (running_) {
        finish_save_snapshot(false);
        iterate();
#ifdef __EMSCRIPTEN__
        wait_for_next_frame(web_frame_pacing);
#endif
    }
    finish_save_snapshot(true);
#ifdef __EMSCRIPTEN__
    frames_ended();
#endif
    SDL_Log("Main loop exited cleanly");
    return 0;
}

#ifdef __EMSCRIPTEN__
// Reads the box the canvas actually occupies, which is what SDL measures
// touches against, plus the device pixel ratio.
// JS writes the canvas box straight into these instead of the engine asking
// for it, so the common case - nothing moved - costs a load and a compare and
// never crosses into JS at all.  generation is bumped on every write so a
// resize back to a previous size is still seen.
struct WebViewportSlot {
    int width;
    int height;
    int ratio_milli;
    int generation;
    // The canvas's real backing store.  SDL derives its own pixel size from
    // the window size and the pixel ratio and never reads this, so the two
    // can disagree with nothing noticing.
    int buffer_width;
    int buffer_height;
};
WebViewportSlot web_viewport_slot{};

// Sets the canvas backing store to what SDL thinks it already is.  This is
// what emscripten_set_canvas_element_size() does; going through
// SDL_SetWindowSize() would not, because it returns early when the window
// size it is given matches the one it already holds - which is exactly the
// case here.
EM_JS(void, th2_web_force_canvas_buffer, (int width, int height), {
    var canvas = Module['canvas'] || document.getElementById('canvas');
    if (canvas && width > 0 && height > 0) {
        canvas.width = width;
        canvas.height = height;
    }
});

EM_JS(void, th2_web_install_viewport_bridge, (WebViewportSlot* slot), {
    // This body runs in the module's scope, so HEAP32 resolves here and keeps
    // resolving after a memory growth reassigns it.
    var base = slot >> 2;
    window.__th2PublishViewport = function(width, height, ratio,
                                           bufferWidth, bufferHeight) {
        if (!(width > 0) || !(height > 0)) {
            return;
        }
        HEAP32[base] = Math.round(width);
        HEAP32[base + 1] = Math.round(height);
        HEAP32[base + 2] = Math.round(ratio * 1000);
        HEAP32[base + 4] = bufferWidth | 0;
        HEAP32[base + 5] = bufferHeight | 0;
        HEAP32[base + 3] = (HEAP32[base + 3] | 0) + 1;
    };
    // Whatever the page already measured, before the engine was up to hear it.
    if (window.__th2Viewport) {
        var canvas = Module['canvas'] || document.getElementById('canvas');
        window.__th2PublishViewport(
            window.__th2Viewport.width, window.__th2Viewport.height,
            window.devicePixelRatio,
            canvas ? canvas.width : 0, canvas ? canvas.height : 0);
    }
});

// Read once and remembered, so this does not cost a crossing per frame.
int th2_web_debug_panel()
{
    static const int enabled = []() {
        return EM_ASM_INT({
            return location.search.indexOf('vpdebug') >= 0 ? 1 : 0;
        });
    }();
    return enabled;
}

EM_JS(void, th2_web_publish_metrics,
      (int window_w, int window_h, int pixel_w, int pixel_h,
       int output_w, int output_h, int box_w, int box_h,
       double density, double scale), {
    // Everything that has to agree for the picture to land in the right
    // place, published for the page to show when ?vpdebug=1 is set.  Read
    // these off the device rather than guessing which one is lying.
    window.__th2Debug = {
        fullscreen: !!document.fullscreenElement,
        window: window_w + 'x' + window_h,
        pixels: pixel_w + 'x' + pixel_h,
        output: output_w + 'x' + output_h,
        published: box_w + 'x' + box_h,
        density: density.toFixed(3),
        scale: scale.toFixed(3)
    };
});

EM_JS(void, th2_web_canvas_box, (int* out_width, int* out_height,
                                 double* out_ratio), {
    // The page publishes the box it pinned the canvas to; measuring it here
    // instead would flush layout on every frame.  Fall back to measuring for
    // pages that do not (someone hosting the engine with their own shell).
    var box = window.__th2Viewport;
    var width;
    var height;
    if (box) {
        width = box.width;
        height = box.height;
    } else {
        var canvas = Module['canvas'] || document.getElementById('canvas');
        var rect = canvas ? canvas.getBoundingClientRect() : null;
        width = rect && rect.width > 0 ? rect.width : window.innerWidth;
        height = rect && rect.height > 0 ? rect.height : window.innerHeight;
    }
    HEAP32[out_width >> 2] = Math.round(width);
    HEAP32[out_height >> 2] = Math.round(height);
    HEAPF64[out_ratio >> 3] = window.devicePixelRatio;
});
#endif

void Game::sync_web_viewport()
{
#ifdef __EMSCRIPTEN__
    // SDL only learns about a browser resize from the window's resize event,
    // and only refreshes the device pixel ratio while handling one.  On a
    // phone that is not enough: a URL bar sliding away, a fold, or a display
    // change can leave SDL sized to the old canvas, which both stops the
    // game rescaling and skews touch coordinates, because finger positions
    // arrive normalized against the real canvas and are scaled back up by
    // the window size SDL believes in.  Measure it ourselves every frame and
    // push the truth into SDL when it drifts.
    // This has to keep running while fullscreen.  SDL sizes the canvas from
    // the fullscreen change event, and on a phone that arrives before the
    // browser chrome has finished retracting, so the size it keeps is the
    // smaller one - and nothing else would ever correct it, which is why a
    // rotation used to be the only way out.
    if (!viewport_bridge_installed_) {
        viewport_bridge_installed_ = true;
        th2_web_install_viewport_bridge(&web_viewport_slot);
    }

    int width = 0;
    int height = 0;
    double ratio = 1.0;
    if (web_viewport_slot.generation != 0) {
        // The usual path: a plain read of our own memory.
        if (web_viewport_slot.generation == web_viewport_generation_) {
            return;
        }
        web_viewport_generation_ = web_viewport_slot.generation;
        width = web_viewport_slot.width;
        height = web_viewport_slot.height;
        ratio = web_viewport_slot.ratio_milli / 1000.0;
    } else {
        // A page hosting the engine with its own shell may never publish.
        // Fall back to asking, but four times a second rather than every
        // frame, since this only exists to catch a page that is not talking
        // to us.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_viewport_poll_ < std::chrono::milliseconds(250)) {
            return;
        }
        last_viewport_poll_ = now;
        th2_web_canvas_box(&width, &height, &ratio);
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    int current_width = 0;
    int current_height = 0;
    SDL_GetWindowSize(window_, &current_width, &current_height);
    if (width == current_width && height == current_height
        && ratio == web_pixel_ratio_) {
        return;
    }
    web_pixel_ratio_ = ratio;
    // SDL_SetWindowSize always reaches the emscripten backend, which rereads
    // the pixel ratio and resizes the canvas backing store, so this also
    // covers a ratio change that kept the same CSS size.
    SDL_SetWindowSize(window_, width, height);
#endif
}

// What the page last published, for the config panel to show.  These are the
// readings the engine cannot take for itself - the canvas's measured box and
// its real backing store - and they are already in the slot, so displaying
// them costs nothing.
#ifdef __EMSCRIPTEN__
void Game::web_published_sizes(
    int* box_width, int* box_height,
    int* buffer_width, int* buffer_height) const
{
    *box_width = web_viewport_slot.width;
    *box_height = web_viewport_slot.height;
    *buffer_width = web_viewport_slot.buffer_width;
    *buffer_height = web_viewport_slot.buffer_height;
}
#endif

void Game::sync_web_canvas_buffer()
{
#ifdef __EMSCRIPTEN__
    // Entering fullscreen on a phone, emscripten sizes the backing store from
    // whatever the screen measured mid-transition, which can be short of the
    // settled height - 1967x2056 against 1967x2183 on the device this was
    // found on.  SDL never notices, because it derives its pixel size from
    // the window size and the pixel ratio rather than reading the canvas, so
    // both sides agree with each other while the canvas agrees with neither
    // and the frame is drawn into a viewport taller than the buffer holding
    // it.  Nothing else corrects this; a rotation only helped because it
    // resized the canvas from scratch.
    const int actual_width = web_viewport_slot.buffer_width;
    const int actual_height = web_viewport_slot.buffer_height;
    if (actual_width <= 0 || actual_height <= 0) {
        return;
    }
    int expected_width = 0;
    int expected_height = 0;
    if (!SDL_GetWindowSizeInPixels(
            window_, &expected_width, &expected_height)
        || expected_width <= 0 || expected_height <= 0) {
        return;
    }
    if (actual_width == expected_width && actual_height == expected_height) {
        return;
    }
    SDL_Log("canvas backing store is %dx%d but SDL draws %dx%d; correcting",
            actual_width, actual_height, expected_width, expected_height);
    th2_web_force_canvas_buffer(expected_width, expected_height);
#endif
}

void Game::iterate()
{
    if (trace_mode_) {
        // A trace run starts in the scenario rather than at the title: the
        // two title screens are not the same program and never will be, so
        // aligning them would be aligning the wrong thing.
        if (trace_tick_ == 0 && ui_mode_ != UiMode::game) {
            movie_.reset();
            start_new_game();
        }
        {
            // TH2_RAND_LOG: engine_rand() calls made during each tick, the
            // port's side of the reference's TH2REF_RAND_LOG.
            static std::FILE* rand_log = [] {
                const char* path = std::getenv("TH2_RAND_LOG");
                return path && *path ? std::fopen(path, "w") : nullptr;
            }();
            static std::uint64_t rand_seen = th2::engine_rand_calls;
            if (rand_log && th2::engine_rand_calls != rand_seen) {
                std::fprintf(rand_log, "%llu %llu\n",
                             static_cast<unsigned long long>(trace_tick_),
                             static_cast<unsigned long long>(
                                 th2::engine_rand_calls - rand_seen));
                std::fflush(rand_log);
                rand_seen = th2::engine_rand_calls;
            }
        }
        ++trace_tick_;
        if (trace_last_tick_ && trace_tick_ > trace_last_tick_) {
            running_ = false;
            return;
        }
    }
    // An ESC_WAIT opcode parks the virtual machine for the rest of the frame
    // it ran in.  That frame is over, so the park is retired before the
    // player's input is looked at rather than after - a click arriving on
    // the frame after a message would otherwise be swallowed by advance()'s
    // wake_time_ guard.  Actually running the script waits for
    // pump_script(), in EXEC_ControlLang's slot below.
    sync_web_viewport();
    // Reads two ints the page publishes and compares them; it does not go
    // near the browser unless they actually disagree.
    sync_web_canvas_buffer();
    // Before any event is looked at, not after: process_event() scales touch
    // positions by this, and entering fullscreen changes it, so leaving it
    // until new_frame() maps a frame's worth of input with the old scale -
    // and on a phone that is every tap until a rotation forces a refresh.
    if (imgui_) {
        imgui_->set_display_scale(imgui_display_scale());
    }
    // The lookahead scan runs here rather than at the end of advance(), so
    // its cost lands on an idle frame instead of the one the player's click
    // is already busy with.  It is throttled because skipping advances the
    // script far faster than the network can answer anyway.
    // One allowance for every subsystem that works ahead, spent in the order
    // they ask.  Reset here so it covers the whole frame.
    background_budget_.begin_frame();
    audio_opens_this_frame_ = 0;
    report_prefetch_trace();
    update_image_decode();
    // Once each time the title comes up: nothing advances the script there,
    // and the new game's opening is worth having ready before the click.
    if (ui_mode_ == UiMode::title) {
        if (!title_scanned_) {
            title_scanned_ = true;
            prefetch_scan_pending_ = true;
        }
    } else {
        title_scanned_ = false;
    }
    if (prefetch_scan_pending_ || prefetch_follow_pending_) {
        const auto now = std::chrono::steady_clock::now();
        // A trace throttles by ticks: 50 ms of the host's time is a
        // different number of ticks on every run.
        const bool due = trace_mode_
            ? trace_tick_ % 3 == 0
            : now - last_prefetch_scan_ >= std::chrono::milliseconds(50);
        if (due && !background_budget_.exhausted()) {
            prefetch_scan_pending_ = false;
            last_prefetch_scan_ = now;
            background_budget_.spend([&] { prefetch_upcoming_assets(); });
        }
    }
    int window_width = 800;
    int window_height = 600;
    SDL_GetWindowSize(window_, &window_width, &window_height);
    // TH2_RECORD_FEED: device events for a recording, from a file instead
    // of a person - "<tick> move X Y", "<tick> down NAME", "<tick> up NAME"
    // in game coordinates and the recorder's names.  Only for checking that
    // a recording replays as it was played; it goes in where SDL's input
    // would, past the coordinate conversion.
    if (recorder_.recording()) {
        static std::vector<std::pair<std::uint64_t, std::string>> feed = [] {
            std::vector<std::pair<std::uint64_t, std::string>> lines;
            const char* path = std::getenv("TH2_RECORD_FEED");
            std::ifstream file(path ? path : "");
            std::string line;
            while (std::getline(file, line)) {
                std::istringstream parts(line);
                std::uint64_t tick = 0;
                std::string rest;
                if (parts >> tick && std::getline(parts, rest)) {
                    lines.emplace_back(tick, rest);
                }
            }
            return lines;
        }();
        static std::size_t fed = 0;
        while (fed < feed.size() && feed[fed].first <= trace_tick_) {
            std::istringstream parts(feed[fed].second);
            std::string what;
            parts >> what;
            if (what == "move") {
                int x = 0;
                int y = 0;
                parts >> x >> y;
                recorder_.pointer(x, y);
            } else {
                std::string name;
                parts >> name;
                recorder_.key(name, what == "down");
            }
            ++fed;
        }
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            running_ = false;
            if (recorder_.recording()) {
                recorder_.finish(trace_tick_);
                SDL_Log("record: finished at tick %llu",
                        static_cast<unsigned long long>(trace_tick_));
            }
            continue;
        }
        if (touch_clear_event_ != 0 && event.type == touch_clear_event_) {
            clear_pointer_highlights();
            continue;
        }
        if (!gamepad_input_.process_event(event)) {
            continue;
        }
        if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
            app_active_ = false;
            SDL_Log("App entered background");
            // Persist the system-save flags immediately. On Android the
            // process may be killed while in the background before the
            // destructor runs.
            sync_game_flags();
            continue;
        }
        if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            app_active_ = true;
            SDL_Log("App entered foreground");
            reset_render_state();
            continue;
        }
        if (event.type == SDL_EVENT_RENDER_TARGETS_RESET) {
            SDL_Log("Render targets reset");
            reset_render_state();
            continue;
        }
        if (event.type == SDL_EVENT_RENDER_DEVICE_RESET) {
            SDL_Log("Render device reset");
            reset_render_state();
            continue;
        }
        if (event.type == SDL_EVENT_RENDER_DEVICE_LOST) {
            SDL_Log("Render device lost");
            continue;
        }
#ifndef __ANDROID__
        if (event.type == SDL_EVENT_WINDOW_MOVED
            || event.type == SDL_EVENT_WINDOW_RESIZED
            || event.type == SDL_EVENT_WINDOW_RESTORED
            || event.type == SDL_EVENT_WINDOW_MAXIMIZED) {
            sync_window_config();
        }
        // Fullscreen can also be left behind our back (Escape in a browser,
        // the window manager on the desktop); keep the option in step.
        if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN
            || event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
            config_.fullscreen =
                event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
        }
#endif
        // A fullscreen change or a lost focus eats the touches that were
        // down, and the finger that triggered it never reports a lift.
        if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN
            || event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN
            || event.type == SDL_EVENT_WINDOW_FOCUS_LOST
            || event.type == SDL_EVENT_WINDOW_HIDDEN) {
            touch_input_.reset();
            imgui_->on_touch_cancel();
        }
        imgui_->process_event(event);
        touch_input_.process_event(event);
        if (event.type == SDL_EVENT_FINGER_DOWN) {
            imgui_->on_touch_down(
                event.tfinger.x, event.tfinger.y);
        } else if (event.type == SDL_EVENT_FINGER_MOTION) {
            imgui_->on_touch_motion(
                event.tfinger.x, event.tfinger.y,
                event.tfinger.dx, event.tfinger.dy);
        } else if (event.type == SDL_EVENT_FINGER_UP
                   || event.type == SDL_EVENT_FINGER_CANCELED) {
            imgui_->on_touch_up(
                event.tfinger.x, event.tfinger.y);
        }
        if (event.type == SDL_EVENT_FINGER_DOWN
            || event.type == SDL_EVENT_FINGER_MOTION
            || event.type == SDL_EVENT_FINGER_UP
            || event.type == SDL_EVENT_FINGER_CANCELED) {
            push_touch_mouse_event(event, window_width, window_height);
        }
        convert_event_to_logical_coordinates(
            event, window_width, window_height);
        // The engine is fed once a tick, from the sampler, and from nowhere
        // else - the same path a recording takes, so what a recording
        // verifies against the reference is what is played.  Releases and
        // the pointer always reach it, so a key pressed in the scene and let
        // go over one of the port's screens does not stay held; presses only
        // while nothing of the port's is up over the scene.
        const bool press = event.type == SDL_EVENT_KEY_DOWN
            || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
            || event.type == SDL_EVENT_MOUSE_WHEEL;
        if (!press) {
            record_input_event(event);
        }
        if (config_.show_script_position
            && imgui_->wants_mouse()
            && (event.type == SDL_EVENT_MOUSE_MOTION
                || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                || event.type == SDL_EVENT_MOUSE_BUTTON_UP
                || event.type == SDL_EVENT_MOUSE_WHEEL)) {
            continue;
        }
        if (event.type == SDL_EVENT_KEY_DOWN
            && (is_alt_enter(event.key) || event.key.key == SDLK_F11)) {
            toggle_fullscreen();
            continue;
        }
        if (movie_) {
            const bool skip_key = event.type == SDL_EVENT_KEY_DOWN
                && (event.key.key == SDLK_ESCAPE
                    || is_confirm_key(event.key.key));
            const bool skip_mouse = event.type
                == SDL_EVENT_MOUSE_BUTTON_DOWN;
            const bool locked = (movie_mode_ == 0
                    && runtime_.game_flag(98) == 0)
                || (movie_mode_ == 1
                    && runtime_.game_flag(80) == 0)
                || (movie_mode_ == 2
                    && runtime_.game_flag(99) == 0);
            if ((skip_key || skip_mouse) && !locked) {
                const int skipped_mode = movie_mode_;
                movie_.reset();
                movie_bytes_.clear();
                movie_mode_ = -1;
                if (skipped_mode != 3) {
                    bgm_.stop();
                    bgm_track_ = -1;
                }
                if (movie_resume_script_) {
                    movie_resume_script_ = false;
                    advance();
                } else if (skipped_mode == 3
                           && runtime_.game_flag(98) != 0) {
                    start_movie(0, 0, false);
                } else {
                    title_started_ = std::chrono::steady_clock::now();
                }
            }
            continue;
        }
        if (config_open_ || name_input_open_) {
            if (config_open_ && event.type == SDL_EVENT_KEY_DOWN
                && gamepad_input_.last_event_was_gamepad()) {
                config_gamepad_focus_requested_ = true;
            }
            if (event.type == SDL_EVENT_KEY_DOWN
                && event.key.key == SDLK_ESCAPE && config_open_) {
                play_se(-1, 9107, false, 255);
                close_config();
            }
            continue;
        }
        if (engine_input_open()) {
            if (ui_mode_ == UiMode::save || ui_mode_ == UiMode::load) {
                handle_save_load_input(event);
            }
            if (!handle_host_key(event) && !handle_text_scroll_drag(event)
                && press) {
                record_input_event(event);
            }
            continue;
        }

        // The port's own screens, which have no engine under them.
        if (ui_mode_ == UiMode::title) {
            handle_title_input(event);
        } else if (ui_mode_ == UiMode::cg_gallery) {
            handle_cg_gallery_input(event);
        } else if (ui_mode_ == UiMode::music_room) {
            handle_music_room_input(event);
        } else if (ui_mode_ == UiMode::replay_gallery) {
            handle_replay_gallery_input(event);
        }
    }
    handle_touch_actions();
    process_save_bundle_dialog();

    if (!app_active_) {
        // Pause the loop while the app is in the background so we
        // don't keep rendering to a surface that may be destroyed.
#ifndef __EMSCRIPTEN__
        SDL_Delay(50);
#endif
        return;
    }
    // The title has no engine under it, so the skip key there is the
    // port's own: it fast-forwards the title's animation.  In the scene it
    // is only KeyCond.btn.ctrl, which the per-tick sample hands over.
    const bool control_held =
        !config_open_ && !name_input_open_
        && ((SDL_GetModState() & SDL_KMOD_CTRL) != 0
            || touch_input_.skip_held()
            || gamepad_input_.ctrl_skip_held());
    if (!movie_ && control_held && ui_mode_ == UiMode::title) {
        title_started_ -= std::chrono::milliseconds(50);
        if (title_exit_started_) {
            *title_exit_started_ -= std::chrono::milliseconds(50);
        }
    }
    // How many ticks are due.  control_ticks_due() spends the
    // accumulator, so it is asked once a frame; a trace always gets one.
    const int ticks = control_ticks_due();
    if (soak_) {
        // The soak driver plays headless and steps the frame its own way.
        control_steps_ = ticks;
        global_count_ += ticks;
        refresh_audio_wait(true);
        update_clock_calendar();
        pump_script();
        soak_->step();
        update_audio();
        update_movie();
        trace_peek_map_pointer();
        update_map();
        update_playback_modes();
        update_title();
        if (control_steps_ > 0) {
            get_game_key();
        } else {
            game_key_ = {};
        }
        control_calendar_key();
        control_system2();
        for (int i = 0; i < control_steps_; ++i) {
            msg().control_novel_message(game_key_);
            msg().control_half_tone();
        }
        update_avg_back(control_steps_);
        for (int i = 0; i < control_steps_; ++i) {
            avgback().control_fade();
        }
        update_audio_decode();
        update_half_tone();
        update_screen_flash();
        update_character_animations(control_steps_);
        update_sakura(control_steps_);
        retire_soak_gpu_work();
        next_frame_ = std::chrono::steady_clock::now();
        return;
    }
    ensure_upscaler();
    if (!gl_transition_) {
        warm_renderer_programs();
        // Built on the first drawn frame, once the renderer's context is
        // current.  If the shader will not build, available() stays false
        // and every wipe keeps the CPU blend.
        gl_transition_ = std::make_unique<th2::GlPatternTransition>(renderer_);
        // Reports what will actually run, which is not the same question as
        // whether the shader built: --cpu-transitions leaves it built and
        // unused, and a line saying "GPU shader" in that case sent a
        // comparison run off to measure the GPU path against itself.
        SDL_Log(
            "pattern wipes: %s",
            th2app::force_cpu_transitions
                ? "CPU blend (forced)"
                : (gl_transition_->available() ? "GPU shader" : "CPU blend"));
    }
    int output_width = 800;
    int output_height = 600;
    SDL_GetRenderOutputSize(
        renderer_, &output_width, &output_height);
    const float scale_x = output_width / 800.0f;
    const float scale_y = output_height / 600.0f;
    const float framebuffer_scale = std::min(scale_x, scale_y);
#ifdef __EMSCRIPTEN__
    // Only while the readout is actually on screen; it is the one thing here
    // that reaches into the browser, and it has no reason to run otherwise.
    if (th2_web_debug_panel()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_metrics_publish_ >= std::chrono::milliseconds(250)) {
            last_metrics_publish_ = now;
            int window_width = 0;
            int window_height = 0;
            int pixel_width = 0;
            int pixel_height = 0;
            SDL_GetWindowSize(window_, &window_width, &window_height);
            SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
            // Straight out of the slot; asking JS for it would be a crossing
            // to fetch a number already sitting in our own memory.
            th2_web_publish_metrics(
                window_width, window_height, pixel_width, pixel_height,
                output_width, output_height,
                web_viewport_slot.width, web_viewport_slot.height,
                SDL_GetWindowPixelDensity(window_),
                SDL_GetWindowDisplayScale(window_));
        }
    }
#endif
    const float display_scale = imgui_display_scale();
    imgui_->new_frame(window_, display_scale);
    font_.configure(
        config_.authentic_font, config_.font_family,
        config_.font_size, framebuffer_scale);
    draw_config();
    draw_name_input();
    // MAIN_Loop's tick body, once for every tick that is due: the script,
    // then the control chain, each tick with its own sample of the player's
    // input.  A frame with no tick in it - most of them on a fast display -
    // draws the same picture again; a frame after a stall runs
    // several, as the engine would have.
    tick_this_frame_ = ticks > 0;
    for (int i = 0; i < ticks; ++i) {
        run_tick();
    }
    if (ticks == 0) {
        // Nothing counted, but the decoders still want feeding.
        control_steps_ = 0;
        game_key_ = {};
        update_audio();
        update_movie();
        update_audio_decode();
        update_title();
    }
    // main.cpp runs EXEC_ControlLang, then MAIN_GameControl - the AVG_
    // Control* chain - and only then MAIN_DrawGraph.  Drawing before the
    // control pass meant anything the script had just set up was put on
    // screen once with whatever DSP_SetGraph left on it: a character
    // arriving showed at full opacity for a frame before AVG_ControlChar
    // gave it DRW_BLD(0) and started the fade.
    //
    // MAIN_Loop's MainWindow.draw_flag, in a trace:
    //     if( draw_flag == 2 ) draw_flag = 0;
    //     if( draw_flag == 1 ){ draw_flag = 2; ... }
    //     if( draw_flag < 0 ){ draw_flag++; if( draw_flag==0 ) MAIN_DrawControl(); }
    //     else MAIN_DrawControl();
    // so a skipped one-frame wait (AVG_WaitFrame's -10) leaves the screen,
    // and the reference's frame dump, standing still until something sets
    // the flag again.  Not in normal play: nothing there is compared, and a
    // window that stops presenting for a sixth of a second is only a stutter.
    frame_undrawn_ = false;
    if (trace_mode_) {
        int& flag = th2::engine_draw_flag;
        if (flag == 2) {
            flag = 0;
        }
        if (flag == 1) {
            flag = 2;
        }
        if (flag < 0) {
            ++flag;
            frame_undrawn_ = flag != 0;
        }
    }
    if (!frame_undrawn_) {
        draw();
        if (snapshot_after_draw_) {
            capture_autosave_thumbnail();
        }
    }
    // After the whole tick, not before it - which is where the reference
    // writes its line, at the end of MAIN_Loop's tick body.  Taken at the
    // top instead, our line for tick N described the state after tick N-1
    // while the reference's described the state after tick N, and the half
    // frame of skew between them turned up as a phantom one-tick lag in
    // whichever field happened to change that frame.  A comparison cannot
    // tell an off-by-one in the engine from an off-by-one in its own
    // instrument, so the instrument has to be pinned first.
    if (trace_mode_) {
        // Resume before the line, save after it: the first line a resumed
        // run writes has to describe the loaded state, and the checkpoint
        // has to describe the same instant as the last line before it.
        trace_checkpoint_resume();
        trace_dump_state();
        trace_checkpoint_save();
        if (trace_hold_tick_ && trace_tick_ == trace_hold_tick_) {
            SDL_Log("trace: holding at tick %llu for %d s",
                    static_cast<unsigned long long>(trace_tick_),
                    trace_hold_seconds_);
            std::this_thread::sleep_for(
                std::chrono::seconds(trace_hold_seconds_));
        }
    }
#ifdef __EMSCRIPTEN__
    // run_loop() paces the browser build with requestAnimationFrame; there is
    // no thread to sleep on.  All animation is wall-clock driven, so a display
    // refresh above 60 Hz simply renders more often.
    next_frame_ = std::chrono::steady_clock::now();
#else
    // A trace tick is a unit of engine progress, not of time: nothing in the
    // run reads a clock, so there is nothing for it to be in step with.
    // Sleeping here would pace the replay at the engine's rate, which is
    // the whole cost of getting to a divergence - the engine itself computes
    // a tick in well under a millisecond.
    // A recording is played by a person, so from the hand-over it runs at
    // the engine's own rate, a tick every engine_tick_ms.  Only the pacing
    // reads the clock; the tick is still the only thing the engine sees.
    if (!trace_mode_ && vsync_paced_) {
        // SDL_RenderPresent waited for the display; the ticks are counted
        // off the clock in control_ticks_due.
        next_frame_ = std::chrono::steady_clock::now();
    } else if (!trace_mode_ || record_live()) {
        if (record_live() && trace_tick_ == record_from_) {
            next_frame_ = std::chrono::steady_clock::now();
        }
        if (record_live() && trace_tick_ % 30 == 0) {
            const auto title = std::format(
                "ToHeart2 - recording, tick {}", trace_tick_);
            SDL_SetWindowTitle(window_, title.c_str());
        }
        constexpr auto frame_duration =
            std::chrono::milliseconds(engine_tick_ms);
        next_frame_ += frame_duration;
        const auto now = std::chrono::steady_clock::now();
        if (next_frame_ > now) {
            std::this_thread::sleep_until(next_frame_);
        } else if (now - next_frame_ > frame_duration * 4) {
            next_frame_ = now;
        }
    }
#endif
}

void Game::run_tick()
{
    // One tick of the engine: EXEC_ControlLang, then MAIN_GameControl -
    // the AVG_Control* chain - with every counter in it stepped once.
    // Drawing is the frame's, not the tick's; see iterate().
    control_steps_ = 1;
    ++global_count_;
    load_work_tick_ = false;
    if (trace_mode_ && !trace_script_.rules().empty()) {
        trace_rule_script_ = runtime_.script_name();
        for (auto& c : trace_rule_script_) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (const auto dot = trace_rule_script_.find('.');
            dot != std::string::npos) {
            trace_rule_script_.erase(dot);
        }
        trace_rule_pc_ = script_ended_ ? 0 : runtime_.vm_pc();
    }
    // EXEC_ControlLang.  main.cpp runs it, then MAIN_GameControl, and only
    // then MAIN_DrawGraph.  Every resumption of the script goes through here
    // so that order holds; resuming from inside the control pass put a
    // character on screen before AVG_ControlChar had given it its first
    // DRW_BLD, and raised the half tone after AVG_ControlHalfTone had
    // already run for the frame, which is text over an undarkened plate.
    //
    // Before the script runs, not after: an ESC_WAIT re-asks its predicate
    // as part of the script pass.  See Game::refresh_audio_wait.
    refresh_audio_wait(true);
    // Before the script pass: the clock and the calendar are waits the script
    // asks about, so stepping them afterwards left it reading last tick's
    // frame and leaving one tick late.
    update_clock_calendar();
    pump_script();
    update_audio();
    update_movie();
    trace_peek_map_pointer();
    // AVG_Main runs AVG_ControlMapEvent only in the AVG_MAP step: the map
    // stands still while the menu is up over it.
    if (!engine_config_step_) {
        update_map();
    }
    update_playback_modes();
    update_title();
    // AVG_GetGameKey, the top of MAIN_SystemControl: one sample of the
    // player's hands (or the script's) per tick, so every edge it folds is
    // seen by exactly one control pass.
    get_game_key();
    // AVG_Main's AVG_CALENDER arm: AVG_SetCalender reads it.
    control_calendar_key();
    // AVG_System's chain, in its order:
    //     AVG_ControlSystem2();
    //     AVG_ControlNovelMessage();
    //     AVG_ControlHalfTone();
    //     AVG_ControlText();
    //     AVG_ControlBack();
    //     AVG_ControlChar();
    // Before the control chain, which is after everything is drawn: this is
    // the frame as the screen saw it.  See Game::trace_glyph_drawn_.
    if (trace_mode_) {
        trace_glyph_drawn_ = trace_glyph_alpha();
    }
    if (engine_config_step_) {
        // AVG_Main's AVG_CONFIG step: the system menu and the half tone, and
        // nothing else of the AVG_GAME chain - the characters, the weather
        // and the message all stand still underneath it.  AVG_SAVE, AVG_LOAD
        // and AVG_SETTING run the same AVG_ControlSystem.
        control_system();
        msg().control_half_tone();
        update_audio_decode();
        update_half_tone();
    } else {
        control_system2();
        msg().control_novel_message(game_key_);
        msg().control_half_tone();
        update_avg_back(1);
        update_audio_decode();
        update_half_tone();
        update_character_animations(1);
        // AVG_ControlWeather sets every petal's graph up again.
        weather_disp_ = true;
        update_sakura(1);
        // AVG_System runs AVG_ControlSelectWindow late, after
        // AVG_ControlChar and the weather and warp passes rather than with
        // the message control.
        control_select_window();
        // AVG_ControlSystem, the last of the chain: the system menu opened
        // this frame gets its first step on the frame it opened.
        control_system();
    }
    // AVG_Main's tail, after the step's own control and for every step:
    //     AVG_ControlLoad(); AVG_ControlGotoTitle(); AVG_ColtrolFade();
    // The load looks at FadeStruct before this tick's count, so it lets go
    // the tick after the fade lands, not the tick it lands on.
    control_load();
    avgback().control_fade();
    update_screen_flash();
    // AVG_RenewSetp: a step change made during the tick takes effect for
    // the next one.
    if (engine_config_step_next_) {
        engine_config_step_ = *engine_config_step_next_;
        engine_config_step_next_.reset();
    }
    if (load_step_next_) {
        load_step_ = *load_step_next_;
        load_step_next_.reset();
    }
}

// SDL input, in the 800x600 game space, as the script's vocabulary: the
// keys th2ref_input.cpp knows, and the two mouse buttons.  The wheel and the
// middle button have no script name on the reference's side, so they reach
// the engine (live_wheel_, live_middle_) without being recorded - a
// recording that used them could not be replayed there.
void Game::record_input_event(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        pointer_from_touch_ = event.motion.which == SDL_TOUCH_MOUSEID;
        recorder_.pointer(static_cast<int>(event.motion.x),
                          static_cast<int>(event.motion.y));
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        pointer_from_touch_ = event.button.which == SDL_TOUCH_MOUSEID;
        recorder_.pointer(static_cast<int>(event.button.x),
                          static_cast<int>(event.button.y));
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (event.button.button == SDL_BUTTON_LEFT) {
            recorder_.key("lclick", down);
        } else if (event.button.button == SDL_BUTTON_RIGHT) {
            recorder_.key("rclick", down);
        } else if (event.button.button == SDL_BUTTON_MIDDLE && down) {
            live_middle_ = true;
        }
        return;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        // Over a page that does not fit, the wheel scrolls the page and
        // nothing else.
        if (event.wheel.y != 0 && recorder_.x() < sidebar_left_x
            && scroll_overflowing_text(event.wheel.y > 0 ? -1 : 1)) {
            return;
        }
        //     wheel = MUS_GetMouseWheel();  ... if( wheel>0 ) GameKey.pup
        if (event.wheel.y > 0) {
            live_wheel_ = 1;
        } else if (event.wheel.y < 0) {
            live_wheel_ = -1;
        }
        return;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (event.key.repeat) {
            return;
        }
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const char* name = nullptr;
        switch (event.key.key) {
        case SDLK_RETURN: case SDLK_KP_ENTER: name = "enter"; break;
        case SDLK_SPACE: name = "space"; break;
        case SDLK_ESCAPE: name = "esc"; break;
        case SDLK_BACKSPACE: name = "bs"; break;
        case SDLK_LCTRL: case SDLK_RCTRL: name = "ctrl"; break;
        case SDLK_LSHIFT: case SDLK_RSHIFT: name = "shift"; break;
        case SDLK_LALT: case SDLK_RALT: name = "alt"; break;
        case SDLK_HOME: name = "home"; break;
        case SDLK_END: name = "end"; break;
        case SDLK_PAGEUP: name = "pup"; break;
        case SDLK_PAGEDOWN: name = "pdown"; break;
        case SDLK_UP: name = "up"; break;
        case SDLK_DOWN: name = "down"; break;
        case SDLK_LEFT: name = "left"; break;
        case SDLK_RIGHT: name = "right"; break;
        default: break;
        }
        // The gamepad's three extra buttons arrive as F8, F9 and F10 (see
        // GamepadInput) and stand for keys the engine already has: the skip
        // toggle (shift, GameKey.mes_cut_mode), the bar's auto button, and
        // hiding the window (space, GameKey.diswin).
        if (gamepad_input_.last_event_was_gamepad()) {
            if (event.key.key == SDLK_F8) {
                name = "shift";
            } else if (event.key.key == SDLK_F10) {
                name = "space";
            } else if (event.key.key == SDLK_F9 && down) {
                live_bar_button_ = 5;
            }
        }
        static constexpr const char* digits[10] = {
            "num0", "num1", "num2", "num3", "num4",
            "num5", "num6", "num7", "num8", "num9"};
        if (event.key.key >= SDLK_0 && event.key.key <= SDLK_9) {
            name = digits[event.key.key - SDLK_0];
        } else if (event.key.key >= SDLK_KP_1 && event.key.key <= SDLK_KP_9) {
            name = digits[event.key.key - SDLK_KP_1 + 1];
        } else if (event.key.key == SDLK_KP_0) {
            name = digits[0];
        }
        if (name) {
            recorder_.key(name, down);
        }
        return;
    }
    default:
        return;
    }
}

bool Game::handle_text_scroll_drag(const SDL_Event& event)
{
    // The overflow scrollbar at the left edge of the page, the port's: a
    // press on it is the bar's, not a click in the scene.
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
        && event.button.button == SDL_BUTTON_LEFT) {
        return handle_message_scroll_press(event.button.x, event.button.y);
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && message_scroll_dragging_) {
        set_message_scroll_from_y(event.motion.y);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP
               && event.button.button == SDL_BUTTON_LEFT) {
        message_scroll_dragging_ = false;
    }
    return false;
}

bool Game::engine_input_open() const
{
    if (config_open_ || name_input_open_ || movie_) {
        return false;
    }
    // The save and load window is the engine's too (GWIN_ControlSaveLoad-
    // Window reads GameKey), over the scene or over the title.
    return ui_mode_ == UiMode::game || ui_mode_ == UiMode::map
        || ui_mode_ == UiMode::save || ui_mode_ == UiMode::load;
}

bool Game::handle_host_key(const SDL_Event& event)
{
    // The port's keys that are not the engine's: quick save and the load
    // screen.  Everything else in the scene is the sampler's.
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat
        || ui_mode_ != UiMode::game || replay_mode_) {
        return false;
    }
    if (event.key.key == SDLK_F5) {
        // Only where the engine's own menu could have been opened.
        if (!steady_for_save() || !engine_config_check()) {
            return true;
        }
        capture_save_snapshot();
        save(0);
        return true;
    }
    if (event.key.key == SDLK_F7) {
        // AVG_GoConfig(2), as the bar's load button.
        if (engine_config_check()) {
            engine_go_config(2);
        }
        return true;
    }
    return false;
}

void Game::draw()
{
    draw_frame();
}

}  // namespace th2app

using th2app::Game;
using th2app::SdlSubsystem;
using th2app::discover_game_data_path;
using th2app::writable_directory;

int main(int argc, char** argv)
{
    try {
        SdlSubsystem sdl_subsystem;
#ifdef __ANDROID__
        std::filesystem::path data =
            std::filesystem::path(SDL_GetAndroidInternalStoragePath()) /
            "game-data";
#else
        std::filesystem::path data = "game-data";
#endif
        std::optional<std::filesystem::path> scenario;
        std::optional<std::filesystem::path> soak_directory;
        std::size_t soak_runs = 1;
        std::optional<std::filesystem::path> trace_directory;
        std::optional<std::filesystem::path> trace_input;
        std::uint64_t trace_ticks = 0;
        std::uint64_t trace_first = 0;
        std::uint64_t trace_lead = 0;
        std::uint64_t trace_hold = 0;
        int trace_hold_seconds = 20;
        std::filesystem::path trace_save_file;
        std::uint64_t trace_save_at = 0;
        std::filesystem::path trace_resume_file;
        std::uint64_t trace_resume_trigger = 0;
        std::string trace_game_flags;
        std::optional<std::filesystem::path> record_path;
        std::uint64_t record_from = 0;
        bool trace_first_set = false;
        bool trace_lead_set = false;
        bool data_set = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--scenario") {
                if (++index >= argc) {
                    throw std::runtime_error("--scenario requires an SDT path");
                }
                scenario = argv[index];
            } else if (argument == "--trace") {
                if (++index >= argc) {
                    throw std::runtime_error("--trace requires a directory");
                }
                trace_directory = argv[index];
            } else if (argument == "--trace-input") {
                if (++index >= argc) {
                    throw std::runtime_error("--trace-input requires a path");
                }
                trace_input = argv[index];
            } else if (argument == "--trace-ticks") {
                if (++index >= argc) {
                    throw std::runtime_error("--trace-ticks requires a count");
                }
                trace_ticks = std::stoull(argv[index]);
            } else if (argument == "--trace-hold") {
                if (++index >= argc) {
                    throw std::runtime_error("--trace-hold requires a tick");
                }
                trace_hold = std::stoull(argv[index]);
            } else if (argument == "--trace-hold-seconds") {
                if (++index >= argc) {
                    throw std::runtime_error("--trace-hold-seconds needs a count");
                }
                trace_hold_seconds = std::stoi(argv[index]);
            } else if (argument == "--trace-lead") {
                if (++index >= argc) {
                    throw std::runtime_error("--trace-lead requires a tick");
                }
                trace_lead = std::stoull(argv[index]);
                trace_lead_set = true;
            } else if (argument == "--trace-save-file") {
                if (++index >= argc) {
                    throw std::runtime_error(
                        "--trace-save-file requires a path");
                }
                trace_save_file = argv[index];
            } else if (argument == "--trace-save-at") {
                if (++index >= argc) {
                    throw std::runtime_error("--trace-save-at requires a tick");
                }
                trace_save_at = std::stoull(argv[index]);
            } else if (argument == "--trace-resume-file") {
                if (++index >= argc) {
                    throw std::runtime_error(
                        "--trace-resume-file requires a path");
                }
                trace_resume_file = argv[index];
            } else if (argument == "--trace-game-flags") {
                if (++index >= argc) {
                    throw std::runtime_error(
                        "--trace-game-flags requires N=V[,N=V...]");
                }
                trace_game_flags = argv[index];
            } else if (argument == "--trace-resume-trigger") {
                if (++index >= argc) {
                    throw std::runtime_error(
                        "--trace-resume-trigger requires a tick");
                }
                trace_resume_trigger = std::stoull(argv[index]);
            } else if (argument == "--trace-from") {
                if (++index >= argc) {
                    throw std::runtime_error("--trace-from requires a tick");
                }
                trace_first = std::stoull(argv[index]);
                trace_first_set = true;
            } else if (argument == "--record") {
                if (++index >= argc) {
                    throw std::runtime_error("--record requires a path");
                }
                record_path = argv[index];
            } else if (argument == "--record-from") {
                if (++index >= argc) {
                    throw std::runtime_error("--record-from requires a tick");
                }
                record_from = std::stoull(argv[index]);
            } else if (argument == "--cpu-transitions") {
                th2app::force_cpu_transitions = true;
            } else if (argument == "--frame-step") {
                th2app::web_frame_pacing = 1;
            } else if (argument == "--frame-free") {
                th2app::web_frame_pacing = 2;
            } else if (argument == "--trace-prefetch") {
                th2app::trace_prefetch = true;
            } else if (argument == "--soak") {
                soak_directory = writable_directory() / "logs" / "soak";
            } else if (argument == "--soak-state") {
                if (++index >= argc) {
                    throw std::runtime_error(
                        "--soak-state requires a directory");
                }
                soak_directory = argv[index];
            } else if (argument == "--soak-runs") {
                if (++index >= argc) {
                    throw std::runtime_error(
                        "--soak-runs requires a positive number");
                }
                soak_runs = std::stoull(argv[index]);
                if (soak_runs == 0) {
                    throw std::runtime_error(
                        "--soak-runs requires a positive number");
                }
            } else if (!data_set) {
                data = argv[index];
                data_set = true;
            } else {
                throw std::runtime_error(
                    "usage: toheart2 [GAME_DATA_DIRECTORY] "
                    "[--scenario FILE.SDT] [--cpu-transitions] [--soak] "
                    "[--soak-state DIRECTORY] [--soak-runs COUNT]");
            }
        }
        // --record is a trace run with live input.  It has to be the same
        // run the harness will replay, so it takes the harness's defaults:
        // the reference's lead-in for GlobalCount (make_pair.py's
        // REFERENCE_LEAD_IN) and no frame dumps unless asked for.
        if (record_path) {
            if (!trace_directory) {
                trace_directory = record_path->parent_path()
                    / (record_path->stem().string() + ".trace");
            }
            if (!trace_first_set) {
                trace_first = std::numeric_limits<std::uint64_t>::max();
            }
            if (!trace_lead_set) {
                trace_lead = 235;
            }
        } else if (record_from) {
            throw std::runtime_error("--record-from needs --record");
        }
        if (scenario && soak_directory) {
            throw std::runtime_error(
                "--scenario and --soak cannot be used together");
        }

#ifdef __ANDROID__
        // Pause SDL event processing while the app is backgrounded.  The
        // main loop still checks app_active_, but this keeps SDL from
        // presenting frames to a surface that may be destroyed on sleep.
        SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
        // Keep the Android back button as an SDL key event instead of letting
        // the OS finish the activity.
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
        // We handle touch gestures ourselves; don't let SDL synthesize mouse
        // events from finger input, which otherwise causes a "tap" on release
        // to leave the backlog/Advance text.  In the browser it also made
        // every tap count twice: once for SDL's synthetic click and once for
        // the click handle_touch_actions() re-injects for the tap gesture.
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
        av_log_set_level(AV_LOG_ERROR);   // suppress warnings, keep errors
        auto discovered_data = discover_game_data_path(
            data, data_set, !trace_directory && !soak_directory);
        if (!discovered_data) {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "Game data directory not found or invalid: %s",
                data.string().c_str());
            return 1;
        }
        data = *discovered_data;
        SDL_Log("Game data path: %s", data.string().c_str());
        SDL_Log("Game files found, starting engine");

        // Opening an archive reads its header and then its directory, and in
        // the browser each of those is a round trip - a dozen of them in a
        // row before the title screen can appear.  One chunk per archive
        // covers both, and asking for them all here lets them travel at the
        // same time.
        // SDT.PAK and FNT.PAK are small enough that the first read pulls
        // them in whole, so they are not listed here.
        for (const auto* archive : {
                 "GRP.PAK", "bak.pak", "bgm.PAK", "SE.PAK", "voice.pak",
                 "mov.pak"}) {
            th2::data_prefetch_chunk(data / archive, 0, 1 << 20);
        }

        Game game(data, scenario, soak_directory, soak_runs);
        if (trace_directory) {
            game.enable_trace(
                *trace_directory, trace_input, trace_ticks, trace_first,
                trace_lead);
            game.set_trace_hold(trace_hold, trace_hold_seconds);
            game.set_trace_checkpoint(trace_save_file, trace_save_at,
                                      trace_resume_file, trace_resume_trigger);
            if (!trace_game_flags.empty()) {
                game.set_trace_game_flags(trace_game_flags);
            }
            if (record_path) {
                game.enable_recording(*record_path, record_from);
            }
        }
        return game.run();
    } catch (const std::exception& error) {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION, "Fatal error: %s", error.what());
        return 1;
    }
}
