#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Platforms where touch is a first-class input: SDL's synthetic touch-mouse
// events are turned off there (see the SDL_HINT_TOUCH_MOUSE_EVENTS hint in
// main()) and ImGui is fed from finger events instead.
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#define TH2_IMGUI_TOUCH 1
#endif

namespace th2 {

class ImGuiLayer {
public:
    ImGuiLayer(SDL_Window* window, SDL_Renderer* renderer);
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    void process_event(const SDL_Event& event);
    // Keeps the scale used to map input in step with the one the next frame
    // will draw at.  process_event() mixes a live pixel density with this
    // cached scale, so a stale value skews every touch until something
    // refreshes it.
    void set_display_scale(float display_scale);
    void new_frame(SDL_Window* window, float display_scale = 1.0f);
    void render();
    bool wants_input() const;
    bool wants_mouse() const;
    void rebuild_font_atlas(float display_scale);

    // Enable vertical drag-to-scroll for the current ImGui window/child on
    // touch screens. Call once per frame inside the scrollable region.
    void touch_drag_scroll();

    // Direct touch feed for ImGui, used on the platforms where SDL's
    // touch-to-mouse synthesis is disabled (Android, browser).  A finger
    // down is a held left button, so press-drag-release widgets - the
    // scrollbar above all - behave the way they do under a mouse.
    void on_touch_down(float normalized_x, float normalized_y);
    void on_touch_motion(
        float normalized_x, float normalized_y,
        float normalized_dx, float normalized_dy);
    void on_touch_up(float normalized_x, float normalized_y);

    // Forget a touch that never lifted (focus loss, fullscreen change).
    void on_touch_cancel();

private:
    void apply_mobile_style(float scale);

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    bool touch_scroll_active_ = false;
    bool touch_down_ = false;
    float touch_drag_distance_ = 0.0f;
    std::uint64_t last_frame_ticks_ = 0;
    std::string imgui_font_path_;
    float display_scale_ = 1.0f;
    // Kept between frames so the per-frame conversion below reuses their
    // capacity instead of allocating for every draw command.
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
    float last_font_scale_ = 0.0f;
    float last_style_scale_ = 0.0f;
#ifdef __ANDROID__
    // ImGui's AddFontFromMemoryTTF needs the TTF data to stay alive until
    // the atlas is built; we free it when the atlas is rebuilt or destroyed.
    std::unique_ptr<void, decltype(&SDL_free)> imgui_font_data_{nullptr, SDL_free};
#endif
};

}  // namespace th2
