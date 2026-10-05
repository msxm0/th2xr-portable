#include "upscaler.hpp"

#include "gl_anime4k.hpp"
#include "image.hpp"

#include <string_view>

#include <cstdlib>

#include "anime4k.hpp"

#include <algorithm>
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
    LinearUpscaler(SDL_Renderer* renderer, bool want_anime4k)
        : renderer_(renderer), want_anime4k_(want_anime4k)
    {
        create_targets();
    }

    // Whether the art layer is actually going through Anime4K.  Answered
    // only once present() has had a chance to build the shader, because the
    // GL context is not current before the first drawn frame.
    bool anime4k_active() const
    {
        return want_anime4k_ && anime4k_ && anime4k_->available();
    }

    void reset() override
    {
        overlay_.reset();
        sidebar_.reset();
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
        if (want_anime4k_ && !anime4k_) {
            anime4k_ = std::make_unique<GlAnime4K>(renderer_);
            SDL_Log("upscaler: %s", anime4k_->available()
                        ? "Anime4K (GLES shader)" : "linear magnification");
        }
        // The art layer only, and only this draw: the shader magnifies, so
        // it stands in for the blit rather than wrapping it.  A failure here
        // is not fatal - the plain blit below runs instead.
        if (!(anime4k_ && anime4k_->draw(renderer_, art, destination))) {
            SDL_RenderTexture(renderer_, art, nullptr, &destination);
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

    SDL_Renderer* renderer_;
    bool want_anime4k_ = false;
    std::unique_ptr<GlAnime4K> anime4k_;
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
bool renderer_can_run_gl_anime4k(SDL_Renderer* renderer)
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
    bool use_anime4k,
    bool* anime4k_available)
{
    // The SDL_GPU implementation first, for a build still running that
    // renderer.  On GLES - which is every build now - it cannot initialise,
    // and the GL form inside LinearUpscaler takes over.
    try {
        auto anime4k = std::make_unique<Anime4K>(renderer, shader_dir);
        if (anime4k->available()) {
            if (anime4k_available) {
                *anime4k_available = true;
            }
            if (use_anime4k) {
                return anime4k;
            }
        }
    } catch (const std::exception& error) {
        SDL_Log("Anime4K (SDL_GPU) unavailable: %s", error.what());
    }
    auto linear = std::make_unique<LinearUpscaler>(renderer, use_anime4k);
    if (anime4k_available) {
        // Reported as available whenever the renderer can carry the GL form;
        // whether the shader built is only known after the first frame, and
        // the answer here drives a settings toggle rather than a draw.
        *anime4k_available = renderer_can_run_gl_anime4k(renderer);
    }
    return linear;
}

}  // namespace th2
