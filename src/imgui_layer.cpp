#include "imgui_layer.hpp"

#include <imgui.h>
#ifdef TH2_IMGUI_TOUCH
#include <imgui_internal.h>  // ClearActiveID, for drag-to-scroll on touch
#endif

#ifdef TH2_USE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>


namespace th2 {
namespace {

std::string find_imgui_font_path()
{
#ifdef TH2_BUNDLED_IMGUI_FONT_PATH
    return TH2_BUNDLED_IMGUI_FONT_PATH;
#else
    if (!FcInit()) {
        return {};
    }
    FcPattern* pattern = FcNameParse(
        reinterpret_cast<const FcChar8*>("sans-serif"));
    if (!pattern) {
        return {};
    }
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    std::string path;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            path = reinterpret_cast<const char*>(file);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
    return path;
#endif
}

ImTextureID texture_id(SDL_Texture* texture)
{
    return static_cast<ImTextureID>(
        reinterpret_cast<std::uintptr_t>(texture));
}

SDL_Texture* sdl_texture(ImTextureID id)
{
    return reinterpret_cast<SDL_Texture*>(
        static_cast<std::uintptr_t>(id));
}

void update_texture(SDL_Renderer* renderer, ImTextureData* texture)
{
    if (texture->Status == ImTextureStatus_WantDestroy) {
        SDL_DestroyTexture(sdl_texture(texture->GetTexID()));
        texture->SetTexID(ImTextureID_Invalid);
        texture->BackendUserData = nullptr;
        texture->SetStatus(ImTextureStatus_Destroyed);
        return;
    }

    std::vector<unsigned char> rgba;
    const void* pixels = texture->GetPixels();
    int pitch = texture->GetPitch();
    if (texture->Format == ImTextureFormat_Alpha8) {
        rgba.resize(texture->Width * texture->Height * 4);
        for (int i = 0; i < texture->Width * texture->Height; ++i) {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = texture->Pixels[i];
        }
        pixels = rgba.data();
        pitch = texture->Width * 4;
    }

    SDL_Texture* sdl = sdl_texture(texture->GetTexID());
    if (texture->Status == ImTextureStatus_WantCreate) {
        sdl = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
            texture->Width, texture->Height);
        if (!sdl) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(sdl, SDL_BLENDMODE_BLEND);
        texture->SetTexID(texture_id(sdl));
        texture->BackendUserData = sdl;
    }
    if (!SDL_UpdateTexture(sdl, nullptr, pixels, pitch)) {
        throw std::runtime_error(SDL_GetError());
    }
    texture->SetStatus(ImTextureStatus_OK);
}

ImGuiKey imgui_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_A: return ImGuiKey_A;
    case SDLK_C: return ImGuiKey_C;
    case SDLK_V: return ImGuiKey_V;
    case SDLK_X: return ImGuiKey_X;
    case SDLK_Y: return ImGuiKey_Y;
    case SDLK_Z: return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

}  // namespace

ImGuiLayer::ImGuiLayer(SDL_Window* window, SDL_Renderer* renderer)
    : window_(window), renderer_(renderer)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    imgui_font_path_ = find_imgui_font_path();
}

ImGuiLayer::~ImGuiLayer()
{
    if (SDL_TextInputActive(window_)) {
        SDL_StopTextInput(window_);
    }
    for (auto* texture : ImGui::GetPlatformIO().Textures) {
        SDL_DestroyTexture(sdl_texture(texture->GetTexID()));
        texture->SetTexID(ImTextureID_Invalid);
        texture->BackendUserData = nullptr;
    }
    ImGui::DestroyContext();
}

void ImGuiLayer::process_event(const SDL_Event& event)
{
    auto& io = ImGui::GetIO();
    // SDL reports mouse coordinates in window points, while ImGui's
    // coordinate system is the window's pixel size divided by
    // display_scale_ (see new_frame()). Converting points -> pixels ->
    // ImGui units keeps the two aligned everywhere: on Android, where a
    // point is a pixel, this is the old 1/display_scale_; on desktop, where
    // display_scale_ is the pixel density, it is the identity; in the
    // browser on a phone whose device pixel ratio exceeds the scale cap it
    // is the ratio between the two.
    const float density = window_ ? SDL_GetWindowPixelDensity(window_) : 1.0f;
    const float point_scale =
        (density > 0.0f ? density : 1.0f) / display_scale_;
    const auto mouse_x = [&](float x) { return x * point_scale; };
    const auto mouse_y = [&](float y) { return y * point_scale; };
#ifdef TH2_IMGUI_TOUCH
    // Touches reach ImGui through on_touch_*; the mouse events the game
    // synthesizes from the same fingers would be the second copy.
    const bool from_touch =
        (event.type == SDL_EVENT_MOUSE_MOTION
         && event.motion.which == SDL_TOUCH_MOUSEID)
        || ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
             || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            && event.button.which == SDL_TOUCH_MOUSEID);
    if (from_touch) {
        return;
    }
#endif
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        io.AddMouseSourceEvent(
            event.motion.which == SDL_TOUCH_MOUSEID
                ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(
            mouse_x(event.motion.x), mouse_y(event.motion.y));
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
               || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        io.AddMouseSourceEvent(
            event.button.which == SDL_TOUCH_MOUSEID
                ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(
            mouse_x(event.button.x), mouse_y(event.button.y));
        int button = -1;
        if (event.button.button == SDL_BUTTON_LEFT) button = 0;
        if (event.button.button == SDL_BUTTON_RIGHT) button = 1;
        if (event.button.button == SDL_BUTTON_MIDDLE) button = 2;
        if (button >= 0) {
            io.AddMouseButtonEvent(
                button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        io.AddMouseSourceEvent(
            event.wheel.which == SDL_TOUCH_MOUSEID
                ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(
            mouse_x(event.wheel.mouse_x), mouse_y(event.wheel.mouse_y));
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
    } else if (event.type == SDL_EVENT_TEXT_INPUT) {
        io.AddInputCharactersUTF8(event.text.text);
    } else if (event.type == SDL_EVENT_KEY_DOWN
               || event.type == SDL_EVENT_KEY_UP) {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const auto key = imgui_key(event.key.key);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        const auto modifiers = SDL_GetModState();
        io.AddKeyEvent(ImGuiMod_Ctrl, (modifiers & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (modifiers & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (modifiers & SDL_KMOD_ALT) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (modifiers & SDL_KMOD_GUI) != 0);
    }
}

void ImGuiLayer::rebuild_font_atlas(float display_scale)
{
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
#ifdef __ANDROID__
    imgui_font_data_.reset();
#endif
    if (!imgui_font_path_.empty()) {
        // Scale the reference 13px default size to the monitor DPI.
#ifdef __ANDROID__
        std::size_t size = 0;
        void* data = SDL_LoadFile(imgui_font_path_.c_str(), &size);
        if (data) {
            imgui_font_data_.reset(data);
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(
                data, static_cast<int>(size), 13.0f * display_scale, &cfg);
        }
#else
        io.Fonts->AddFontFromFileTTF(
            imgui_font_path_.c_str(), 13.0f * display_scale);
#endif
    }
    if (io.Fonts->Fonts.empty()) {
        io.Fonts->AddFontDefault();
    }
    last_font_scale_ = display_scale;
}

void ImGuiLayer::apply_mobile_style(float scale)
{
#ifdef __ANDROID__
    (void)scale;
    auto& style = ImGui::GetStyle();

    // Reset to a clean base style so repeated calls do not accumulate.
    ImGui::StyleColorsDark();
    style = ImGuiStyle();

    constexpr float k = 1.5f;

    // More breathing room and fatter hit targets without ballooning the UI.
    style.WindowPadding     = ImVec2(style.WindowPadding.x * k,
                                     style.WindowPadding.y * k);
    style.FramePadding      = ImVec2(style.FramePadding.x * k,
                                     style.FramePadding.y * k);
    // Extra vertical gap between controls; this is what makes scrolling and
    // tapping a long settings list feel less cramped.
    style.ItemSpacing       = ImVec2(style.ItemSpacing.x * k,
                                     style.ItemSpacing.y * 2.0f);
    style.ItemInnerSpacing  = ImVec2(style.ItemInnerSpacing.x * k,
                                     style.ItemInnerSpacing.y * k);
    style.CellPadding       = ImVec2(style.CellPadding.x * k,
                                     style.CellPadding.y * k);

    style.ScrollbarSize     = style.ScrollbarSize * k;
    style.GrabMinSize       = style.GrabMinSize * k;

    // Padding outside a widget's visible rectangle that still accepts taps.
    style.TouchExtraPadding = ImVec2(8.0f, 8.0f);

    style.WindowRounding    = 8.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;

    last_style_scale_ = scale;
#endif
}

void ImGuiLayer::set_display_scale(float display_scale)
{
    display_scale_ = std::max(1.0f, display_scale);
}

void ImGuiLayer::new_frame(SDL_Window* window, float display_scale)
{
    auto& io = ImGui::GetIO();
    set_display_scale(display_scale);

    // Use the pixel size so the math works on platforms (e.g. Android) where
    // SDL_GetWindowSize() returns pixels rather than logical points.
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height);
    if (pixel_width == 0 || pixel_height == 0) {
        SDL_GetWindowSize(window, &pixel_width, &pixel_height);
    }

    // present_frame() renders ImGui with SDL_SetRenderScale(display_scale_).
    // ImGui's internal coordinate system must be the matching logical
    // coordinate system, so we divide the pixel size by the same scale that
    // the renderer will multiply by. This keeps mouse/touch input (which is
    // divided by display_scale_ in process_event()) aligned with the UI.
    io.DisplaySize = ImVec2(
        static_cast<float>(pixel_width) / display_scale_,
        static_cast<float>(pixel_height) / display_scale_);
    io.DisplayFramebufferScale = ImVec2(display_scale_, display_scale_);

    // Rebuild the font atlas when the scale changes so text stays crisp
    // at the monitor's native resolution.
    if (std::abs(display_scale_ - last_font_scale_) > 0.05f) {
        rebuild_font_atlas(display_scale_);
    }

#ifdef __ANDROID__
    if (std::abs(display_scale_ - last_style_scale_) > 0.05f) {
        apply_mobile_style(display_scale_);
        SDL_Log(
            "ImGui mobile style applied: scale=%.2f "
            "WindowPadding=%.1f,%.1f FramePadding=%.1f,%.1f "
            "ItemSpacing=%.1f,%.1f TouchExtraPadding=%.1f,%.1f",
            display_scale_,
            ImGui::GetStyle().WindowPadding.x,
            ImGui::GetStyle().WindowPadding.y,
            ImGui::GetStyle().FramePadding.x,
            ImGui::GetStyle().FramePadding.y,
            ImGui::GetStyle().ItemSpacing.x,
            ImGui::GetStyle().ItemSpacing.y,
            ImGui::GetStyle().TouchExtraPadding.x,
            ImGui::GetStyle().TouchExtraPadding.y);
    }
#endif

    const auto ticks = SDL_GetTicks();
    io.DeltaTime = last_frame_ticks_ == 0
        ? 1.0f / 60.0f
        : std::max((ticks - last_frame_ticks_) / 1000.0f, 0.001f);
    last_frame_ticks_ = ticks;
    ImGui::NewFrame();
}

void ImGuiLayer::render()
{
    ImGui::Render();
    const bool wants_text = ImGui::GetIO().WantTextInput;
    if (wants_text && !SDL_TextInputActive(window_)) {
        SDL_StartTextInput(window_);
    } else if (!wants_text && SDL_TextInputActive(window_)) {
        SDL_StopTextInput(window_);
    }
    const auto* data = ImGui::GetDrawData();
    if (!data) {
        return;
    }
    if (data->Textures) {
        for (auto* texture : *data->Textures) {
            if (texture->Status != ImTextureStatus_OK) {
                update_texture(renderer_, texture);
            }
        }
    }
    for (int list_index = 0; list_index < data->CmdListsCount; ++list_index) {
        const auto* list = data->CmdLists[list_index];
        // Cleared, not rebuilt: capacity survives the frame, so a steady UI
        // stops allocating entirely after the first one.
        vertices_.clear();
        vertices_.reserve(list->VtxBuffer.Size);
        for (const auto& vertex : list->VtxBuffer) {
            const auto color = vertex.col;
            vertices_.push_back(SDL_Vertex{
                {vertex.pos.x, vertex.pos.y},
                {
                    ((color >> IM_COL32_R_SHIFT) & 0xff) / 255.0f,
                    ((color >> IM_COL32_G_SHIFT) & 0xff) / 255.0f,
                    ((color >> IM_COL32_B_SHIFT) & 0xff) / 255.0f,
                    ((color >> IM_COL32_A_SHIFT) & 0xff) / 255.0f,
                },
                {vertex.uv.x, vertex.uv.y},
            });
        }
        for (const auto& command : list->CmdBuffer) {
            if (command.UserCallback) {
                command.UserCallback(list, &command);
                continue;
            }
            const SDL_Rect clip{
                static_cast<int>(command.ClipRect.x),
                static_cast<int>(command.ClipRect.y),
                static_cast<int>(command.ClipRect.z - command.ClipRect.x),
                static_cast<int>(command.ClipRect.w - command.ClipRect.y),
            };
            SDL_SetRenderClipRect(renderer_, &clip);
            auto* texture = sdl_texture(command.GetTexID());
            indices_.clear();
            indices_.reserve(command.ElemCount);
            for (unsigned int i = 0; i < command.ElemCount; ++i) {
                indices_.push_back(
                    list->IdxBuffer[command.IdxOffset + i]
                    + static_cast<int>(command.VtxOffset));
            }
            SDL_RenderGeometry(
                renderer_, texture, vertices_.data(),
                static_cast<int>(vertices_.size()),
                indices_.data(), static_cast<int>(indices_.size()));
        }
    }
    SDL_SetRenderClipRect(renderer_, nullptr);
}

void ImGuiLayer::touch_drag_scroll()
{
#ifdef TH2_IMGUI_TOUCH
    auto& io = ImGui::GetIO();
    if (ImGui::GetScrollMaxY() <= 0.0f
        || !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        touch_scroll_active_ = false;
        return;
    }

    if (!touch_down_) {
        touch_scroll_active_ = false;
        touch_drag_distance_ = 0.0f;
        return;
    }

    if (!touch_scroll_active_) {
        if (!ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive()) {
            // Started on empty space: scroll straight away.
            touch_scroll_active_ = true;
        } else {
            // Started on a widget.  Nearly the whole panel is widgets on a
            // phone, so a plain "not on an item" rule leaves nowhere to drag
            // from.  Let the touch keep the widget until it has clearly
            // travelled vertically, then take it over for scrolling - the
            // sliders here are horizontal, so this costs them nothing.
            touch_drag_distance_ += io.MouseDelta.y;
            if (std::abs(touch_drag_distance_) > 10.0f) {
                ImGui::ClearActiveID();
                touch_scroll_active_ = true;
            }
        }
    }

    if (touch_scroll_active_ && io.MouseDelta.y != 0.0f) {
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    }
#endif
}

void ImGuiLayer::on_touch_down(float normalized_x, float normalized_y)
{
#ifdef TH2_IMGUI_TOUCH
    auto& io = ImGui::GetIO();
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(
        normalized_x * pixel_width / display_scale_,
        normalized_y * pixel_height / display_scale_);
    if (!touch_down_) {
        // A finger is a held button as far as ImGui is concerned.  Without
        // it nothing that needs a press-drag-release works on a phone -
        // scrollbars, sliders, drag handles - because the tap gesture only
        // ever produced a click after the finger was already gone.
        io.AddMouseButtonEvent(0, true);
    }
    touch_down_ = true;
    touch_drag_distance_ = 0.0f;
#endif
}

void ImGuiLayer::on_touch_motion(
    float normalized_x, float normalized_y,
    float normalized_dx, float normalized_dy)
{
#ifdef TH2_IMGUI_TOUCH
    auto& io = ImGui::GetIO();
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(
        normalized_x * pixel_width / display_scale_,
        normalized_y * pixel_height / display_scale_);
    (void)normalized_dx;
    (void)normalized_dy;
#endif
}

void ImGuiLayer::on_touch_up(float normalized_x, float normalized_y)
{
#ifdef TH2_IMGUI_TOUCH
    auto& io = ImGui::GetIO();
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSizeInPixels(window_, &pixel_width, &pixel_height);
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(
        normalized_x * pixel_width / display_scale_,
        normalized_y * pixel_height / display_scale_);
    if (touch_down_) {
        io.AddMouseButtonEvent(0, false);
    }
    touch_down_ = false;
#endif
}

void ImGuiLayer::on_touch_cancel()
{
#ifdef TH2_IMGUI_TOUCH
    if (touch_down_) {
        ImGui::GetIO().AddMouseButtonEvent(0, false);
    }
    touch_down_ = false;
    touch_scroll_active_ = false;
    touch_drag_distance_ = 0.0f;
#endif
}

bool ImGuiLayer::wants_input() const
{
    const auto& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantCaptureMouse;
}

bool ImGuiLayer::wants_mouse() const
{
    return ImGui::GetIO().WantCaptureMouse;
}

}  // namespace th2
