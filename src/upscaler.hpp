#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>

namespace th2 {

// Abstraction that owns the fixed 800x600 core art render target and a
// monitor-resolution overlay target, and composites both to the window.
// Different implementations can upscale the art layer in different ways
// (e.g. bilinear, Anime4K, Lanczos, etc.) while the core game rendering
// logic stays locked to 800x600.
class Upscaler {
public:
    virtual ~Upscaler() = default;

    // Fixed 800x600 target for all core game art (backgrounds, characters,
    // transitions, title screen, map, etc.).
    virtual SDL_Texture* art_target() const = 0;

    // Monitor-resolution target for text, ImGui, and other overlays that
    // must remain crisp at the native display resolution.
    virtual SDL_Texture* overlay_target() = 0;

    // Fixed-resolution target for authentic bitmap text. This is composited
    // with linear filtering between the art and native overlay layers.
    virtual SDL_Texture* authentic_text_target() const = 0;

    // Monitor-resolution target for the sidebar, composited above every
    // other game layer so its configured fade is background-independent.
    virtual SDL_Texture* sidebar_target() = 0;

    // Composite the art and overlay layers to the window backbuffer.
    virtual void present() = 0;

    // Which of the optional layers hold anything this frame.  A layer that
    // does not is neither composited nor cleared - each of those is a pass
    // over every pixel of the screen, which a phone pays for in fill rate,
    // and the authentic-text layer is empty whenever the outline font is
    // in use.
    void set_layer_content(bool authentic_text, bool sidebar)
    {
        authentic_text_content_ = authentic_text;
        sidebar_content_ = sidebar;
    }

    // The art to composite this frame instead of art_target(): a frame
    // drawn between two ticks is drawn into a copy, so the art target keeps
    // exactly what the last tick drew (see Game::draw_frame).  Null for the
    // art target itself.
    void set_art_source(SDL_Texture* art) { art_source_ = art; }

protected:
    bool authentic_text_content_ = true;
    bool sidebar_content_ = true;
    SDL_Texture* art_source_ = nullptr;

public:

    // Recreate all owned render targets (e.g. after a GPU device reset).
    virtual void reset() = 0;

    // Returns true if this upscaler is the Anime4K implementation.
    virtual bool is_anime4k() const { return false; }
};

// Creates the best available upscaler. If use_anime4k is true and the
// Anime4K GPU path is available, returns that; otherwise returns a linear
// fallback. If anime4k_available is non-null, it is set to true when the
// Anime4K path could be initialized.
std::unique_ptr<Upscaler> create_upscaler(
    SDL_Renderer* renderer,
    const std::filesystem::path& shader_dir,
    bool use_anime4k,
    bool* anime4k_available = nullptr);

}  // namespace th2
