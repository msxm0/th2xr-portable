#include "upscaler.hpp"

#include "gl_anime4k.hpp"
#include "gl_cunny.hpp"
#include "image.hpp"

#include <string_view>

#include <cstdlib>

#include "anime4k.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace th2 {
namespace {

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;

SDL_FRect letterbox_rect(int output_width, int output_height)
{
    const float scale = std::min(
        output_width / 800.0f, output_height / 600.0f);
    const float width = 800.0f * scale;
    const float height = 600.0f * scale;
    return {
        (output_width - width) / 2.0f,
        (output_height - height) / 2.0f,
        width,
        height};
}

class LinearUpscaler : public Upscaler {
public:
    LinearUpscaler(SDL_Renderer* renderer, ArtUpscaling mode)
        : renderer_(renderer), mode_(mode)
    {
        create_targets();
    }

    // Whether the art layer is actually going through Anime4K.  Answered
    // only once present() has had a chance to build the shader, because the
    // GL context is not current before the first drawn frame.
    bool anime4k_active() const
    {
        return mode_ == ArtUpscaling::anime4k && anime4k_
            && anime4k_->available();
    }

    void reset() override
    {
        overlay_.reset();
        sidebar_.reset();
        // A device reset takes the GL context, and the network's textures
        // and programs with it.
        cunny_.reset();
        art_changed_ = true;
        overlay_width_ = 0;
        overlay_height_ = 0;
        create_targets();
    }

    void create_targets()
    {
        art_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            800, 600));
        if (!art_) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(art_.get(), SDL_BLENDMODE_BLEND);
        authentic_text_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            800, 600));
        if (!authentic_text_) {
            throw std::runtime_error(SDL_GetError());
        }
        th2::make_premultiplied_layer(authentic_text_.get());
        SDL_SetTextureScaleMode(authentic_text_.get(), SDL_SCALEMODE_LINEAR);
    }

    SDL_Texture* art_target() const override { return art_.get(); }

    SDL_Texture* overlay_target() override
    {
        ensure_overlay();
        return overlay_.get();
    }

    SDL_Texture* authentic_text_target() const override
    {
        return authentic_text_.get();
    }

    SDL_Texture* sidebar_target() override
    {
        ensure_overlay();
        return sidebar_.get();
    }

    void present() override
    {
        ensure_overlay();
        int output_width = 0;
        int output_height = 0;
        if (!SDL_GetRenderOutputSize(
                renderer_, &output_width, &output_height)) {
            throw std::runtime_error(SDL_GetError());
        }
        const auto destination = letterbox_rect(output_width, output_height);
        SDL_SetRenderTarget(renderer_, nullptr);
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_Texture* const art = art_source_ ? art_source_ : art_.get();
        SDL_SetTextureScaleMode(art, SDL_SCALEMODE_LINEAR);
        // Built on the first drawn frame, once the renderer's context is
        // current - the same reason GlPatternTransition is.
        if (mode_ == ArtUpscaling::anime4k && !anime4k_) {
            anime4k_ = std::make_unique<GlAnime4K>(renderer_);
            SDL_Log("upscaler: %s", anime4k_->available()
                        ? "Anime4K (GLES shader)" : "linear magnification");
        }
        if (cunny_model() && !cunny_) {
            cunny_ = std::make_unique<GlCunny>(renderer_, *cunny_model());
            SDL_Log("upscaler: %s",
                    cunny_->available() ? "CuNNy (GLES shaders)"
                    : cunny_->building()
                        ? "CuNNy, shaders building - linear until then"
                        : "linear magnification");
        }
        // The art layer only, and only this draw: the shaders magnify, so
        // they stand in for the blit rather than wrapping it.  A failure
        // here is not fatal - the plain blit below runs instead.
        bool drawn = false;
        ArtPath path{"linear", static_cast<int>(std::round(destination.w)),
                     static_cast<int>(std::round(destination.h))};
        float art_w = 0.0f;
        float art_h = 0.0f;
        SDL_GetTextureSize(art, &art_w, &art_h);
        if (cunny_ && destination.w > art_w) {
            // The network's 2x picture, Lanczos-resampled to the letterbox
            // (gl_lanczos.hpp) and drawn 1:1, at whole pixels so nothing
            // filters it again.  Both run only when the art or the size
            // changed.  At 1x or below there is nothing to upscale, and the
            // art is blitted as it is.
            const SDL_FRect whole{
                std::round(destination.x), std::round(destination.y),
                std::round(destination.w), std::round(destination.h)};
            if (SDL_Texture* const upscaled = cunny_->upscale(
                    renderer_, art, art_changed_,
                    static_cast<int>(whole.w), static_cast<int>(whole.h))) {
                drawn = SDL_RenderTexture(renderer_, upscaled, nullptr, &whole);
                art_changed_ = false;
            }
            path.method = drawn ? cunny_method()
                : cunny_->building() ? "linear, CuNNy building" : "linear";
        } else if (anime4k_) {
            drawn = anime4k_->draw(renderer_, art, destination);
            art_changed_ = false;
            path.method = drawn ? "Anime4K" : "linear";
        } else if (!cunny_) {
            art_changed_ = false;
        }
        // (With CuNNy chosen but not drawn - skipped at 1x, or its shaders
        // still building - art_changed_ stays set, so the network runs on
        // the first frame it can.)
        if (!drawn) {
            SDL_RenderTexture(renderer_, art, nullptr, &destination);
        }
        if (cunny_ && !(destination.w > art_w)) {
            path.method = "linear, 1x or below";
        }
        if (path != art_path_) {
            art_path_ = path;
            SDL_Log("upscaler: art %s to %dx%d output pixels",
                    path.method, path.width, path.height);
        }
        if (authentic_text_content_) {
            SDL_RenderTexture(
                renderer_, authentic_text_.get(), nullptr, &destination);
        }
        SDL_RenderTexture(renderer_, overlay_.get(), nullptr, &destination);
        if (sidebar_content_) {
            SDL_RenderTexture(renderer_, sidebar_.get(), nullptr, &destination);
        }
    }

private:
    void ensure_overlay()
    {
        int output_width = 0;
        int output_height = 0;
        if (!SDL_GetRenderOutputSize(
                renderer_, &output_width, &output_height)) {
            throw std::runtime_error(SDL_GetError());
        }
        const auto destination = letterbox_rect(output_width, output_height);
        const int width = static_cast<int>(destination.w);
        const int height = static_cast<int>(destination.h);
        if (overlay_ && sidebar_
            && width == overlay_width_ && height == overlay_height_) {
            return;
        }
        overlay_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            width, height));
        if (!overlay_) {
            throw std::runtime_error(SDL_GetError());
        }
        th2::make_premultiplied_layer(overlay_.get());
        SDL_SetTextureScaleMode(overlay_.get(), SDL_SCALEMODE_LINEAR);
        sidebar_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            width, height));
        if (!sidebar_) {
            throw std::runtime_error(SDL_GetError());
        }
        th2::make_premultiplied_layer(sidebar_.get());
        SDL_SetTextureScaleMode(sidebar_.get(), SDL_SCALEMODE_LINEAR);
        overlay_width_ = width;
        overlay_height_ = height;
    }

    const char* cunny_method() const
    {
        switch (mode_) {
        case ArtUpscaling::cunny_fast: return "CuNNy fast 2x + Lanczos";
        case ArtUpscaling::cunny_4x16: return "CuNNy 4x16 2x + Lanczos";
        case ArtUpscaling::cunny_4x32: return "CuNNy 4x32 2x + Lanczos";
        default: return "CuNNy";
        }
    }

    std::optional<CunnyModel> cunny_model() const
    {
        switch (mode_) {
        case ArtUpscaling::cunny_fast: return CunnyModel::fast;
        case ArtUpscaling::cunny_4x16: return CunnyModel::c4x16;
        case ArtUpscaling::cunny_4x32: return CunnyModel::c4x32;
        default: return std::nullopt;
        }
    }

    SDL_Renderer* renderer_;
    ArtUpscaling mode_ = ArtUpscaling::linear;
    std::unique_ptr<GlAnime4K> anime4k_;
    std::unique_ptr<GlCunny> cunny_;
    Texture art_;
    Texture authentic_text_;
    Texture overlay_;
    Texture sidebar_;
    int overlay_width_ = 0;
    int overlay_height_ = 0;
};

}  // namespace

namespace {

// The GL form needs SDL to be drawing through GLES; the shader itself is not
// built until the first frame, so this is the question that can be answered
// at construction time.
bool renderer_can_run_gl_shaders(SDL_Renderer* renderer)
{
    const char* name = SDL_GetRendererName(renderer);
    if (!name) {
        return false;
    }
    const std::string_view driver(name);
    return driver == "opengl" || driver == "opengles2"
        || driver == "opengles";
}

}  // namespace

std::unique_ptr<Upscaler> create_upscaler(
    SDL_Renderer* renderer,
    const std::filesystem::path& shader_dir,
    ArtUpscaling mode,
    bool* shaders_available)
{
    const bool use_anime4k = mode == ArtUpscaling::anime4k;
    // The SDL_GPU implementation first, for a build still running that
    // renderer.  On GLES - which is every build now - it cannot initialise,
    // and the GL form inside LinearUpscaler takes over.
    try {
        auto anime4k = std::make_unique<Anime4K>(renderer, shader_dir);
        if (anime4k->available()) {
            if (shaders_available) {
                *shaders_available = true;
            }
            if (use_anime4k) {
                return anime4k;
            }
        }
    } catch (const std::exception& error) {
        SDL_Log("Anime4K (SDL_GPU) unavailable: %s", error.what());
    }
    auto linear = std::make_unique<LinearUpscaler>(renderer, mode);
    if (shaders_available) {
        // Reported as available whenever the renderer can carry the GL form;
        // whether the shader built is only known after the first frame, and
        // the answer here drives a settings toggle rather than a draw.
        *shaders_available = renderer_can_run_gl_shaders(renderer);
    }
    return linear;
}

}  // namespace th2
