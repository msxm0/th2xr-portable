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

namespace th2app {

Game::CharacterTexture& Game::character_texture(int number)
{
    if (number < 0
        || static_cast<std::size_t>(number) >= character_textures_.size()) {
        throw std::out_of_range("unsupported character number");
    }
    return character_textures_[number];
}

Surface Game::capture_frame_pixels(bool art_only)
{
    (void)art_only;
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    SDL_SetRenderTarget(renderer_, upscaler_->art_target());
    SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
    SDL_SetRenderTarget(renderer_, previous_target);
    if (!surface) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    if (!converted) {
        throw std::runtime_error(SDL_GetError());
    }
    return Surface(converted);
}

Surface Game::capture_frame_thumbnail(int width, int height)
{
    Texture small(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
        width, height));
    if (!small) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_Texture* art = upscaler_->art_target();
    SDL_ScaleMode art_scale = SDL_SCALEMODE_LINEAR;
    SDL_GetTextureScaleMode(art, &art_scale);
    // Linear, because this is a big reduction; nearest would alias badly.
    SDL_SetTextureScaleMode(art, SDL_SCALEMODE_LINEAR);
    // A straight copy, not a blend.  The art target carries no useful alpha,
    // so blending it over an empty target yields a black thumbnail - which
    // reading the pixels back directly never did.
    SDL_BlendMode art_blend = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(art, &art_blend);
    SDL_SetTextureBlendMode(art, SDL_BLENDMODE_NONE);

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, small.get());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    const bool drawn = SDL_RenderTexture(renderer_, art, nullptr, nullptr);
    SDL_Surface* raw = drawn
        ? SDL_RenderReadPixels(renderer_, nullptr) : nullptr;
    SDL_SetRenderTarget(renderer_, previous_target);
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    SDL_SetTextureScaleMode(art, art_scale);
    SDL_SetTextureBlendMode(art, art_blend);
    if (!raw) {
        throw std::runtime_error(SDL_GetError());
    }
    Surface captured(raw);
    if (raw->format == SDL_PIXELFORMAT_RGBA32) {
        return captured;
    }
    Surface converted(SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32));
    if (!converted) {
        throw std::runtime_error(SDL_GetError());
    }
    return converted;
}

Texture Game::capture_frame_texture()
{
    SDL_Texture* source = upscaler_->art_target();
    float width = 0.0f;
    float height = 0.0f;
    if (!SDL_GetTextureSize(source, &width, &height)) {
        throw std::runtime_error(SDL_GetError());
    }
    // A finished transition's own target if one is spare: creating a target
    // texture makes SDL's GLES2 renderer check the framebuffer's status, a
    // round trip to the GPU process in WebGL, once or twice a wipe.
    Texture copy;
    for (auto at = spare_frame_targets_.begin();
         at != spare_frame_targets_.end(); ++at) {
        float spare_width = 0.0f;
        float spare_height = 0.0f;
        SDL_GetTextureSize(at->get(), &spare_width, &spare_height);
        if (spare_width == width && spare_height == height) {
            copy = std::move(*at);
            spare_frame_targets_.erase(at);
            // As a new one would be: whatever the wipe set is gone.
            SDL_SetTextureColorMod(copy.get(), 255, 255, 255);
            SDL_SetTextureAlphaMod(copy.get(), 255);
            SDL_ScaleMode scale = SDL_SCALEMODE_LINEAR;
            SDL_GetDefaultTextureScaleMode(renderer_, &scale);
            SDL_SetTextureScaleMode(copy.get(), scale);
            break;
        }
    }
    if (!copy) {
        copy.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            static_cast<int>(width), static_cast<int>(height)));
    }
    if (!copy) {
        throw std::runtime_error(SDL_GetError());
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, copy.get());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    // A straight copy, not a blend: the art target already holds the frame
    // exactly as it should be remembered.
    const SDL_BlendMode source_blend = [&] {
        SDL_BlendMode mode = SDL_BLENDMODE_BLEND;
        SDL_GetTextureBlendMode(source, &mode);
        return mode;
    }();
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
    const bool drawn = SDL_RenderTexture(renderer_, source, nullptr, nullptr);
    SDL_SetTextureBlendMode(source, source_blend);
    SDL_SetRenderTarget(renderer_, previous_target);
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    if (!drawn) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(copy.get(), SDL_BLENDMODE_BLEND);
    return copy;
}

void Game::end_transition()
{
    if (!transition_) {
        return;
    }
    // Its frame copies go back for the next wipe; see capture_frame_texture.
    // Only render targets - a CPU wipe's composite is a streaming texture.
    for (Texture* held : {&transition_->previous, &transition_->composite}) {
        if (!*held || spare_frame_targets_.size() >= 4) {
            continue;
        }
        const auto access = SDL_GetNumberProperty(
            SDL_GetTextureProperties(held->get()),
            SDL_PROP_TEXTURE_ACCESS_NUMBER, -1);
        if (access == SDL_TEXTUREACCESS_TARGET) {
            spare_frame_targets_.push_back(std::move(*held));
        }
    }
    transition_.reset();
}

Texture Game::texture_from_surface(SDL_Surface* surface)
{
    SDL_Texture* raw = SDL_CreateTextureFromSurface(renderer_, surface);
    if (!raw) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(raw, SDL_BLENDMODE_BLEND);
    return Texture(raw);
}

void Game::retire_soak_gpu_work(bool force)
{
    constexpr std::size_t flush_interval = 64;
    if (!force && ++soak_renderer_ticks_ < flush_interval) {
        return;
    }
    soak_renderer_ticks_ = 0;
    // SDL_GPU retires transient uploads at present boundaries. Without
    // these, accelerated soak runs retain every scene's staging buffers.
    if (!SDL_SetRenderTarget(renderer_, nullptr)) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    if (!SDL_RenderClear(renderer_) || !SDL_RenderPresent(renderer_)) {
        throw std::runtime_error(SDL_GetError());
    }
}

// A pattern wipe with no snapshot under it - a B that landed on the frame a
// held skip key came up, see begin_transition - is GRP_BACK drawn through
// the mask straight over the framebuffer:
//     DSP_SetGraphBSet( GRP_BACK, BMP_BACK+2, rate, BackStruct.fd_vague );
// where the mask has not reached, the frame keeps whatever the last one
// left, and that builds up tick by tick.  So the wipe's "outgoing" picture
// is not the one taken when it began but the target as this pass finds it.
// Measured on ERRATIC2 at ticks 8074..8080 (pattern 22, pc 2409 of
// 070000100.sdt): the new picture was on screen whole from the first frame
// while the reference's grew out of the old street.
void Game::renew_wipe_backdrop(SDL_Texture* target)
{
    if (!transition_ || !transition_->no_snapshot || transition_->type < 0x80
        || !back().fd_flag || !transition_->previous) {
        return;
    }
    if (transition_->previous_pixels && target == upscaler_->art_target()) {
        transition_->previous_pixels = capture_frame_pixels(true);
        transition_->previous = texture_from_surface(
            transition_->previous_pixels.get());
        return;
    }
    const auto access = SDL_GetNumberProperty(
        SDL_GetTextureProperties(transition_->previous.get()),
        SDL_PROP_TEXTURE_ACCESS_NUMBER, -1);
    if (access != SDL_TEXTUREACCESS_TARGET) {
        return;
    }
    SDL_SetRenderTarget(renderer_, transition_->previous.get());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(target, &blend);
    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(renderer_, target, nullptr, nullptr);
    SDL_SetTextureBlendMode(target, blend);
}

bool Game::gl_transition_usable() const
{
    return !force_cpu_transitions && gl_transition_
        && gl_transition_->available();
}

// The mask only becomes a texture if the shader path is going to sample it;
// on the CPU path the bytes are all that is needed.
SDL_Texture* Game::transition_mask_texture(int type)
{
    auto& mask = const_cast<TransitionMask&>(transition_mask(type));
    if (mask.texture) {
        return mask.texture.get();
    }
    if (mask.width <= 0 || mask.height <= 0) {
        return nullptr;
    }
    Texture texture(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
        mask.width, mask.height));
    if (!texture) {
        return nullptr;
    }
    // The shader reads the red channel; the mask is a single value per pixel.
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(mask.width) * mask.height * 4);
    for (std::size_t i = 0; i < mask.pixels.size(); ++i) {
        const auto value = mask.pixels[i];
        rgba[i * 4 + 0] = value;
        rgba[i * 4 + 1] = value;
        rgba[i * 4 + 2] = value;
        rgba[i * 4 + 3] = 255;
    }
    if (!SDL_UpdateTexture(
            texture.get(), nullptr, rgba.data(), mask.width * 4)) {
        return nullptr;
    }
    // The CPU blend indexes the mask with integer division, so nearest keeps
    // the two paths producing the same edge.
    SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST);
    mask.texture = std::move(texture);
    return mask.texture.get();
}

const Game::TransitionMask& Game::transition_mask(int type)
{
    if (const auto found = transition_masks_.find(type);
        found != transition_masks_.end()) {
        return found->second;
    }
    TransitionMask mask;
    mask.pixels = load_transition_mask(type, mask.width, mask.height);
    return transition_masks_.emplace(type, std::move(mask)).first->second;
}

// Runs on an idle frame only, and only once the mask's own bytes have
// arrived, so preparing it never turns into a blocking read.
void Game::prepare_pending_transition_mask()
{
    while (!pending_transition_masks_.empty()) {
        const int type = *pending_transition_masks_.begin();
        if (transition_masks_.contains(type)) {
            pending_transition_masks_.erase(pending_transition_masks_.begin());
            continue;
        }
        const auto name = std::format("f0{:03d}.bmp", type & 0x7f);
        const auto* entry = graphics_.find(name);
        if (!entry) {
            pending_transition_masks_.erase(pending_transition_masks_.begin());
            continue;  // Not in this release; nothing to prepare.
        }
        // The pre-decoder unpacks the bitmap in budgeted slices (the scan
        // names it); building the mask from that is one quick pass.  Until
        // it is there, wait - decoding it here was the 14-24 ms frame.
        if (!decoded_images_.contains("grp:" + name)) {
            return;
        }
        pending_transition_masks_.erase(pending_transition_masks_.begin());
        try {
            transition_mask(type);
        } catch (const std::exception&) {
            // A mask that will not load is not worth retrying every scan.
        }
        return;  // One per idle frame; the walk is not cheap.
    }
}

std::vector<std::uint8_t> Game::load_transition_mask(
    int type, int& width, int& height)
{
    const auto name = std::format("f0{:03d}.bmp", type & 0x7f);
    // The pre-decoder's copy if it has one: it unpacks the bitmap a slice a
    // frame, which is what keeps preparing a wipe off any single frame.  It
    // is read, not taken - the same bitmap serves every flip and curve of
    // the pattern.
    if (Surface* held = decoded_images_.find("grp:" + name);
        held && *held) {
        return transition_mask_pixels(held->get(), type, width, height);
    }
    const auto* entry = graphics_.find(name);
    if (!entry) {
        throw std::runtime_error("transition mask not found: " + name);
    }
    Surface surface(th2::load_image(graphics_.read(*entry), entry->name));
    return transition_mask_pixels(surface.get(), type, width, height);
}

std::vector<std::uint8_t> Game::transition_mask_pixels(
    SDL_Surface* surface, int type, int& width, int& height)
{
    width = surface->w;
    height = surface->h;
    std::array<std::uint8_t, 256> curve{};
    for (int i = 0; i < 256; ++i) {
        curve[i] = static_cast<std::uint8_t>(i);
    }
    std::string_view curve_name;
    if (type & 0x100) {
        curve_name = type & 0x200 ? "rev_accel1.AMP"
            : type & 0x400 ? "rev_accel2.AMP" : "rev.AMP";
    } else if (type & 0x200) {
        curve_name = "accel1.AMP";
    } else if (type & 0x400) {
        curve_name = "accel2.AMP";
    }
    if (!curve_name.empty()) {
        const auto* curve_entry = graphics_.find(curve_name);
        if (!curve_entry) {
            throw std::runtime_error(
                "transition curve not found: " + std::string(curve_name));
        }
        const auto bytes = graphics_.read(*curve_entry);
        if (bytes.size() != curve.size()) {
            throw std::runtime_error(
                "invalid transition curve: " + std::string(curve_name));
        }
        std::copy(bytes.begin(), bytes.end(), curve.begin());
    }

    // One pass over the pixels as stored.  SDL_ReadSurfacePixel per pixel
    // was the whole cost: 480,000 calls, each resolving the format again,
    // 14-24 ms on whichever frame prepared the wipe.  The value is the same
    // blue channel it returned - the palette entry for an 8 bit bitmap, the
    // converted pixel otherwise.
    const SDL_Palette* palette = SDL_GetSurfacePalette(surface);
    const bool indexed =
        SDL_BITSPERPIXEL(surface->format) == 8 && palette != nullptr;
    Surface converted;
    SDL_Surface* source = surface;
    if (!indexed) {
        converted.reset(SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32));
        if (!converted) {
            throw std::runtime_error(SDL_GetError());
        }
        source = converted.get();
    }
    if (!SDL_LockSurface(source)) {
        throw std::runtime_error(SDL_GetError());
    }
    std::vector<std::uint8_t> mask(
        static_cast<std::size_t>(width) * height);
    const bool flip_x = type & 0x800;
    const bool flip_y = type & 0x1000;
    for (int y = 0; y < height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(source->pixels)
            + static_cast<std::size_t>(flip_y ? height - y - 1 : y)
                * source->pitch;
        auto* out = mask.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const int sx = flip_x ? width - x - 1 : x;
            std::uint8_t blue = 0;
            if (indexed) {
                const int index = row[sx];
                blue = index < palette->ncolors ? palette->colors[index].b : 0;
            } else {
                blue = row[static_cast<std::size_t>(sx) * 4 + 2];
            }
            out[x] = curve[blue];
        }
    }
    SDL_UnlockSurface(source);
    return mask;
}

// The per-pixel sweeps always blend in C++.  Pattern wipes do too, unless the
// shader path is up, in which case nothing needs to come off the GPU at all.
bool Game::transition_needs_pixels(int type) const
{
    if (type >= 0x80) {
        return !gl_transition_usable();
    }
    return (type >= 2 && type <= 10) || type == 21;
}

void Game::begin_transition(
    int type, int frames, int vague, bool resume_script,
    EffectTiming timing)
{
    if (type < 0) {
        return;
    }
    // AVG_EffCnt4() is the same count in 30fps units with no Avg.wait in it,
    // so a menu fade lasts the same half second whatever the effect speed.
    const int effective_frames = timing == EffectTiming::menu
        ? frames * 2 : effect_frames(frames);
    // AVG_SetBack's other half: the outgoing screen goes into GRP_BACK+1 a
    // layer below and GRP_BACK moves on top of it, so the fade has a fixed
    // backdrop instead of compositing over a framebuffer that already holds
    // the picture it is fading to.  A menu cross-fade is of the whole
    // composited screen rather than of GRP_BACK, so it keeps out of this.
    Surface previous_pixels;
    Texture previous;
    if (transition_needs_pixels(type)) {
        previous_pixels = capture_frame_pixels(true);
        previous = texture_from_surface(previous_pixels.get());
    } else {
        previous = capture_frame_texture();
    }
    Transition transition{
        std::move(previous),
        std::move(previous_pixels),
        {},
        {},
        {},
        0,
        0,
        vague >= 0 ? vague : 128,
        effective_frames,
        type,
        resume_script,
        std::chrono::steady_clock::now(),
        next_transition_debug_id_++,
        -1,
        false,
    };
    if (type >= 0x80) {
        const auto& mask = transition_mask(type);
        transition.mask = mask.pixels;
        transition.mask_width = mask.width;
        transition.mask_height = mask.height;
    }
    // AVG_SetBack:
    //     int back_max = AVG_EffCnt(fd_max);
    //     ...
    //     if( chg_type!=BAK_DIRECT ){ if(back_max){ ...DSP_GetDispBmp,
    //         GRP_BACK+1, GRP_BACK up a layer... } fd_flag = 1; ... }
    // back_max is taken once, here, while AVG_ControlBackChange re-asks it
    // every frame.  A B that lands on the frame a held skip key comes up
    // sees Avg.msg_cut still set from the frame before - no snapshot, GRP_BACK
    // left at LAY_BACK - and then a fade of the full length once the key is
    // up, compositing over whatever the last frame left.  Measured on
    // ERRATIC2 at pc 5480 of 010302000.sdt: the reference drew GRP_BACK alone
    // at DRW_BLD(2, 4, 6, 8...) and never called GetGraph.
    transition.no_snapshot = timing != EffectTiming::menu
        && effective_frames == 0;
    end_transition();   // one still running hands its targets back first
    transition_ = std::move(transition);
    // AVG_SetBack's tail: fd_flag on, fd_cnt zeroed, fd_max left as the raw
    // number the script wrote so AVG_EffCnt can be re-asked every frame.
    // The menu fades keep their own count, which is AVG_EffCnt4's.
    back().fd_flag = 1;
    back().fd_type = type;
    back().fd_cnt = 0;
    back().fd_max = timing == EffectTiming::menu ? frames : frames;
    back().fd_vague = vague >= 0 ? vague : 128;
    back().redraw = 1;
}

void Game::update_transition()
{
    // A menu cross-fade keeps its own clock.
    if (menu_transition_frames_ > 0) {
        if (transition_progress() >= 1.0f) {
            menu_transition_frames_ = 0;
            end_transition();
        }
        return;
    }
    // AVG_ControlBackChange owns the counter now; all that is left here is
    // letting go of the pixels the wipe borrowed once fd_flag has cleared.
    // Nothing is resumed: the B instruction that started this is still the
    // current instruction and re-asks AVG_WaitBack at the top of the frame.
    if (transition_ && !back().fd_flag) {
        end_transition();
    }
}

// The shader form of draw_pattern_transition(): same blend, same mask, but
// both frames stay on the GPU, so the wipe costs no readback.  Returns false
// if anything is missing, and the caller falls back to the CPU blend.
bool Game::draw_pattern_transition_gpu(int rate)
{
    if (!gl_transition_usable()) {
        return false;
    }
    auto& transition = *transition_;
    if (!transition.previous) {
        return false;
    }
    if (!transition.composite) {
        // The new scene is on the art target; take a GPU-side copy of it the
        // same way the old one was taken.
        transition.composite = capture_frame_texture();
    }
    SDL_Texture* mask = transition_mask_texture(transition.type);
    if (!mask) {
        return false;
    }
    // blnd2, exactly as draw_pattern_transition_rate() builds it: the
    // integer rate through a truncating divide, not a float progress
    // rescaled.  The two disagree by a step at the wipe front.
    const int vague = std::clamp(transition.vague, 1, 256);
    const int offset = rate * (256 + vague) / 256;
    if (th2::debug_draws) {
        SDL_Log("wipe: mask %dx%d  vague=%d offset=%d rate=%d",
                transition.mask_width, transition.mask_height, vague, offset,
                rate);
    }
    return gl_transition_->draw(
        renderer_, transition.previous.get(), transition.composite.get(),
        mask, offset, vague);
}

void Game::draw_pattern_transition(float progress)
{
    draw_pattern_transition_rate(
        std::clamp(static_cast<int>(progress * 256.0f), 0, 256));
}

void Game::draw_pattern_transition_rate(int rate)
{
    auto& transition = *transition_;
    if (!transition.next_pixels) {
        transition.next_pixels = capture_frame_pixels();
        if (transition.next_pixels->w != transition.previous_pixels->w
            || transition.next_pixels->h != transition.previous_pixels->h) {
            throw std::runtime_error("transition frame size changed");
        }
        SDL_Texture* raw = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING,
            transition.next_pixels->w, transition.next_pixels->h);
        if (!raw) {
            throw std::runtime_error(SDL_GetError());
        }
        transition.composite.reset(raw);
    }

    const int width = transition.next_pixels->w;
    const int height = transition.next_pixels->h;
    // Reused across the frames of a wipe rather than reallocated and zeroed
    // for each one: at 800x600 that was ~1.9MB of allocation and a memset
    // every frame, for a buffer every byte of which is overwritten below.
    auto& pixels = transition_pixels_;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    const auto* previous =
        static_cast<const std::uint8_t*>(transition.previous_pixels->pixels);
    const auto* next =
        static_cast<const std::uint8_t*>(transition.next_pixels->pixels);
    //     blnd3 = LIM(dobj->dnum3, 1, 256);
    //     blnd2 = dobj->dnum2*(256+blnd3)/256;
    //     PtnTable[i] = LIM( (i-256)*256/blnd3, 0, 255 );
    // with dnum2 the rate and dnum3 the vagueness, both integers.
    const int vague = std::clamp(transition.vague, 1, 256);
    const int blend_offset = rate * (256 + vague) / 256;

    for (int y = 0; y < height; ++y) {
        const int mask_y = y * transition.mask_height / height;
        for (int x = 0; x < width; ++x) {
            const int mask_x = x * transition.mask_width / width;
            const int mask = transition.mask[
                static_cast<std::size_t>(mask_y) * transition.mask_width
                + mask_x];
            const int alpha = std::clamp(
                (mask + blend_offset - 256) * 256 / vague, 0, 255);
            const auto source_offset =
                static_cast<std::size_t>(y) * transition.next_pixels->pitch
                + static_cast<std::size_t>(x) * 4;
            const auto previous_offset =
                static_cast<std::size_t>(y) * transition.previous_pixels->pitch
                + static_cast<std::size_t>(x) * 4;
            const auto output_offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            // Draw24.cpp's pattern blit, term by term:
            //     blnd_tbl = BlendTable[     PtnTable[mask+blnd2] ];
            //     brev_tbl = BlendTable[ 255-PtnTable[mask+blnd2] ];
            //     dest = brev_tbl[dest] + blnd_tbl[src];
            // BlendTable is (i*j)>>8, so this is two independent truncations
            // at /256 - not one combined divide by 255, which is what this
            // used to do and which came out a level bright on every pixel of
            // the screen for the length of a wipe.
            for (int channel = 0; channel < 3; ++channel) {
                const int kept =
                    ((255 - alpha) * previous[previous_offset + channel]) >> 8;
                const int added = (alpha * next[source_offset + channel]) >> 8;
                pixels[output_offset + channel] =
                    static_cast<std::uint8_t>(std::clamp(kept + added, 0, 255));
            }
            pixels[output_offset + 3] = 255;
        }
    }
    if (!SDL_UpdateTexture(
            transition.composite.get(), nullptr, pixels.data(), width * 4)) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_RenderTexture(
        renderer_, transition.composite.get(), nullptr, nullptr);
}

void Game::ensure_transition_target()
{
    auto& transition = *transition_;
    if (transition.composite) {
        return;
    }
    if (!transition_needs_pixels(transition.type)) {
        // The new scene is already on the art target; copying it there and
        // back through system memory would buy nothing.
        transition.composite = capture_frame_texture();
        return;
    }
    transition.next_pixels = capture_frame_pixels();
    if (transition.next_pixels->w != transition.previous_pixels->w
        || transition.next_pixels->h != transition.previous_pixels->h) {
        throw std::runtime_error("transition frame size changed");
    }
    transition.composite.reset(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
        transition.next_pixels->w, transition.next_pixels->h));
    if (!transition.composite) {
        throw std::runtime_error(SDL_GetError());
    }
}

void Game::draw_pixel_transition(float progress)
{
    ensure_transition_target();
    auto& transition = *transition_;
    const int width = transition.next_pixels->w;
    const int height = transition.next_pixels->h;
    const int rate = std::clamp(static_cast<int>(progress * 256.0f), 0, 256);
    auto& pixels = transition_pixels_;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    const auto* previous =
        static_cast<const std::uint8_t*>(transition.previous_pixels->pixels);
    const auto* next =
        static_cast<const std::uint8_t*>(transition.next_pixels->pixels);

    auto source_alpha = [&](int x, int y) {
        switch (transition.type) {
        case 2: {
            const int edge = (height + 255) * (256 - rate) / 256;
            return std::clamp((y - edge + 255) * 256 / 255, 0, 256);
        }
        case 3: {
            const int edge = (height + 255) * rate / 256;
            return std::clamp((edge - y) * 256 / 255, 0, 256);
        }
        case 4: {
            const int edge = (width + 255) * (256 - rate) / 256;
            return std::clamp((x - edge + 255) * 256 / 255, 0, 256);
        }
        case 5: {
            const int edge = (width + 255) * rate / 256;
            return std::clamp((edge - x) * 256 / 255, 0, 256);
        }
        case 6: {
            const int edge = (width / 2 + 127) * (256 - rate) / 256;
            return std::clamp(
                (width / 2 - std::abs(x - width / 2) - edge + 127)
                    * 256 / 127,
                0, 256);
        }
        case 7: {
            const int edge = (width / 2 + 127) * rate / 256;
            return std::clamp(
                (std::abs(x - width / 2) - width / 2 + edge)
                    * 256 / 127,
                0, 256);
        }
        case 8:
        case 9:
        case 10: {
            const int shift = transition.type - 8;
            const int mask = 0x3f >> shift;
            const int half = 32 >> shift;
            const int extent = (96 >> shift) * rate / 256 - half;
            const bool inside = std::abs((x & mask) - half)
                < std::abs((y & mask) - half) + extent;
            return inside ? 16 + rate * rate / 512 : 0;
        }
        case 21: {
            std::uint32_t value = static_cast<std::uint32_t>(
                x + y * width + transition.debug_id * 0x9e3779b9U);
            value ^= value >> 16;
            value *= 0x7feb352dU;
            value ^= value >> 15;
            return static_cast<int>(value & 0xff) < rate ? 256 : 0;
        }
        default:
            return rate;
        }
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int alpha = source_alpha(x, y);
            const auto old_offset =
                static_cast<std::size_t>(y) * transition.previous_pixels->pitch
                + static_cast<std::size_t>(x) * 4;
            const auto new_offset =
                static_cast<std::size_t>(y) * transition.next_pixels->pitch
                + static_cast<std::size_t>(x) * 4;
            const auto output_offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                pixels[output_offset + channel] =
                    static_cast<std::uint8_t>(
                        (previous[old_offset + channel] * (256 - alpha)
                         + next[new_offset + channel] * alpha)
                        / 256);
            }
            pixels[output_offset + 3] = 255;
        }
    }
    if (!SDL_UpdateTexture(
            transition.composite.get(), nullptr, pixels.data(), width * 4)) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_RenderTexture(
        renderer_, transition.composite.get(), nullptr, nullptr);
}

void Game::draw_geometric_transition(float progress)
{
    ensure_transition_target();
    auto& transition = *transition_;
    if (transition.next_pixels
        && !SDL_UpdateTexture(
            transition.composite.get(), nullptr,
            transition.next_pixels->pixels,
            transition.next_pixels->pitch)) {
        throw std::runtime_error(SDL_GetError());
    }
    const int rate = std::clamp(static_cast<int>(progress * 256.0f), 0, 256);
    auto draw_old = [&](SDL_FRect destination, float alpha = 1.0f) {
        SDL_SetTextureAlphaModFloat(transition.previous.get(), alpha);
        SDL_RenderTexture(
            renderer_, transition.previous.get(), nullptr, &destination);
    };
    const SDL_FRect full{0.0f, 0.0f, 800.0f, 600.0f};

    switch (transition.type) {
    // BAK_CFZOOM1..4 and BAK_SLIDE_UP..LE are graph parameters now; see
    // control_back_change().  What is left here are the types no script
    // uses, kept because they cost nothing and the numbering would be
    // confusing without them.
    case 15: {  // BAK_KAMI - simplified; the engine uses four graphs
        const float x = 800.0f * rate / 256.0f;
        SDL_FRect left{-x, 0.0f, 800.0f, 600.0f};
        SDL_FRect right{x, 0.0f, 800.0f, 600.0f};
        draw_old(left, 0.5f);
        draw_old(right, 0.5f);
        break;
    }
    case 20: {
        const int amplitude = std::min(255, rate);
        for (int y = 0; y < 600; ++y) {
            const float offset = amplitude
                * std::sin((y * 480.0f / 600.0f + rate * 2.0f)
                           * std::numbers::pi_v<float> / 128.0f)
                / 10.0f;
            const SDL_FRect source{
                std::max(0.0f, offset), static_cast<float>(y),
                800.0f - std::abs(offset), 1.0f};
            SDL_FRect destination{
                std::max(0.0f, -offset), static_cast<float>(y),
                source.w, 1.0f};
            SDL_RenderTexture(
                renderer_, transition.previous.get(), &source, &destination);
        }
        break;
    }
    case 22: {
        for (int y = 0; y < 600; ++y) {
            const float offset = rate
                * std::sin((y + rate * 2.0f)
                           * std::numbers::pi_v<float> / 128.0f)
                / 10.0f;
            const SDL_FRect source{
                0.0f, static_cast<float>(
                    std::clamp(y + static_cast<int>(offset), 0, 599)),
                800.0f, 1.0f};
            const SDL_FRect destination{
                0.0f, static_cast<float>(y), 800.0f, 1.0f};
            SDL_SetTextureAlphaModFloat(
                transition.previous.get(), 32.0f / 256.0f);
            SDL_RenderTexture(
                renderer_, transition.previous.get(), &source, &destination);
        }
        break;
    }
    case 23: {
        int tv = 150 - 150 * rate / 256;
        tv = 150 - tv * tv * tv / (150 * 150);
        const float h = std::max(0.0f, 600.0f - 600.0f * tv / 120.0f);
        SDL_FRect rectangle{0.0f, (600.0f - h) / 2.0f, 800.0f, h};
        draw_old(rectangle);
        break;
    }
    case 24: {
        const float roll = 128.0f * rate / 256.0f;
        const float angle = roll * 360.0f / 256.0f;
        const float scale = (roll + 256.0f) / 256.0f;
        SDL_FRect rectangle{
            400.0f - 400.0f * scale, 300.0f - 300.0f * scale,
            800.0f * scale, 600.0f * scale};
        SDL_SetTextureAlphaModFloat(
            transition.previous.get(),
            std::clamp((128.0f - roll) / 128.0f, 0.0f, 1.0f));
        SDL_RenderTextureRotated(
            renderer_, transition.previous.get(), nullptr, &rectangle,
            angle, nullptr, SDL_FLIP_NONE);
        break;
    }
    default:
        break;
    }
}

void Game::dump_transition_frame(float progress)
{
    if (!config_.dump_transition_frames || !transition_) {
        return;
    }
    const int frame_count = transition_->frames;
    const int frame = std::clamp(
        static_cast<int>(progress * frame_count), 0, frame_count);
    if (frame == transition_->last_dumped_frame) {
        return;
    }
    transition_->last_dumped_frame = frame;

    const auto directory = std::filesystem::path("debug/transitions")
        / std::format("{:06}_{}_{}", transition_->debug_id,
                      runtime_.script_name(), runtime_.vm_pc());
    std::filesystem::create_directories(directory);
    auto surface = capture_frame_pixels();
    const auto path = directory / std::format("frame_{:04}.bmp", frame);
    if (!SDL_SaveBMP(surface.get(), path.string().c_str())) {
        std::cerr << "transition dump: " << SDL_GetError() << '\n';
    }
    if (!transition_->debug_metadata_written) {
        transition_->debug_metadata_written = true;
        std::ofstream metadata(directory / "transition.txt");
        metadata << "script=" << runtime_.script_name() << '\n'
                 << "vm_pc=" << runtime_.vm_pc() << '\n'
                 << "type=" << transition_->type << '\n'
                 << "vague=" << transition_->vague << '\n'
                 << "frames=" << transition_->frames << '\n'
                 << "mask_width=" << transition_->mask_width << '\n'
                 << "mask_height=" << transition_->mask_height << '\n';
    }
}

void Game::draw_active_transition()
{
    if (!transition_) {
        return;
    }
    if (transition_->frames <= 0 && !transition_->no_snapshot) {
        // Instant: there is no frame of the old screen left to show.  Not a
        // wipe that began with no snapshot, though: that one had no length
        // only because a held skip key was still latched when it began, and
        // AVG_ControlBackChange re-asks AVG_EffCnt every frame - once the key
        // is up the wipe runs its full length (fd_cnt and fd_max below).
        return;
    }
    // AVG_ControlBackChange measures the wipe with BackStruct's own counter:
    //
    //     int back_max = AVG_EffCnt(BackStruct.fd_max);
    //     ...
    //     rate = 256*BackStruct.fd_cnt/back_max;
    //
    // - a tick count, re-asking AVG_EffCnt every frame so a held skip key
    // collapses it.  This read a wall clock instead, which is the last one
    // left in the drawing path: the wipe ran at whatever rate the machine
    // managed rather than at the engine's, and no two runs agreed.  fd_cnt
    // and fd_max are already in step with the reference tick for tick.
    const int back_max = effect_frames(back().fd_max);
    int rate = back_max > 0
        ? std::clamp(256 * back().fd_cnt / back_max, 0, 256)
        : 256;
    // Between two ticks, the count the next one is heading for - still a
    // whole rate, so the mask shader and its CPU twin take it unchanged.
    if (presentation_phase_ > 0.0 && back_max > 0) {
        const double fd = avgback().present_fd_cnt(presentation_phase_);
        if (fd >= 0.0) {
            rate = std::clamp(
                static_cast<int>(std::floor(256.0 * fd / back_max)), 0, 256);
        }
    }
    const float progress = std::clamp(
        static_cast<float>(rate) / 256.0f, 0.0f, 1.0f);
    if (transition_->type >= 0x80) {
        // The integer rate, not the float: blnd2 is built from it with two
        // truncating divisions and the wipe front sits a step off if either
        // is done in floating point.
        if (!draw_pattern_transition_gpu(rate)) {
            draw_pattern_transition_rate(rate);
        }
    } else if (transition_->type == 0) {
        if (progress < 0.5f) {
            SDL_SetTextureAlphaModFloat(
                transition_->previous.get(), 1.0f);
            SDL_RenderTexture(
                renderer_, transition_->previous.get(), nullptr, nullptr);
        }
        const float midpoint = progress < 0.5f
            ? progress * 2.0f : (1.0f - progress) * 2.0f;
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(
            renderer_, 0, 0, 0,
            static_cast<Uint8>(
                std::clamp(midpoint, 0.0f, 1.0f) * 255.0f));
        SDL_RenderFillRect(renderer_, nullptr);
    } else if ((transition_->type >= 2 && transition_->type <= 10)
               || transition_->type == 21) {
        draw_pixel_transition(progress);
    } else if (transition_->type >= 11
               && transition_->type <= 24) {
        draw_geometric_transition(progress);
    } else {
        SDL_SetTextureAlphaModFloat(
            transition_->previous.get(), 1.0f - progress);
        SDL_RenderTexture(
            renderer_, transition_->previous.get(), nullptr, nullptr);
    }
    dump_transition_frame(progress);
}

void Game::draw_script_position()
{
    if (!config_.show_script_position || ui_mode_ != UiMode::game) {
        return;
    }
    const auto position = std::format(
        "{}:{}  line={}", runtime_.script_name(), runtime_.vm_pc(),
        current_line_key_.empty() ? "-" : current_line_key_);
    ImGui::SetNextWindowPos(
        ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin(
        "Script position", nullptr,
        ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(position.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        SDL_SetClipboardText(position.c_str());
    }
    ImGui::End();
}

void Game::begin_background_fade(int red, int green, int blue, int frames)
{
    // AVG_SetBackFade opens with
    //     AVG_ResetHalfTone();
    //     AVG_SetNovelMessageDisp(OFF);
    // which is what keeps the darkened copy from being on screen while the
    // background's brightness is being driven out from under it - the copy
    // is of a plate at the old brightness and nothing would refresh it.
    avgback().set_back_fade(
        std::clamp(red, 0, 256), std::clamp(green, 0, 256),
        std::clamp(blue, 0, 256), frames);
}

void Game::update_background_fade()
{
    // AVG_ControlBackFade owns br_cnt and the rr/gg/bb interpolation, and
    // sets the brightness on GRP_BACK and on every script overlay itself.
    // What is left here is handing the result to the parts of the draw that
    // are not graphs yet.
    background_brightness_ = {
        static_cast<float>(back().rr),
        static_cast<float>(back().gg),
        static_cast<float>(back().bb),
    };
}
void Game::update_screen_flash()
{
    // AVG_ColtrolFade owns FadeStruct.cnt and starts the return leg itself,
    // so all that is left is letting go of the overlay once flag clears.
    if (screen_flash_ && !avgback().wait_fade()) {
        screen_flash_.reset();
    }
}

float Game::screen_flash_alpha() const
{
    // FadeStruct.r/g/b, which AVG_ColtrolFade walks from where the last ramp
    // left off to (er,eg,eb) and, for a flash, back to neutral afterwards.
    // The engine puts that on GRP_DISP's brightness over a frozen snapshot;
    // ours tints the composited frame, so the distance from neutral is the
    // alpha.
    if (!avg_back_) {
        return 0.0f;
    }
    const auto& fade = avgback().fade();
    int r = fade.r;
    int g = fade.g;
    int b = fade.b;
    if (presentation_phase_ > 0.0) {
        avgback().present_fade(presentation_phase_, r, g, b);
    }
    const int distance = std::max({
        std::abs(r - th2::bright_neutral),
        std::abs(g - th2::bright_neutral),
        std::abs(b - th2::bright_neutral)});
    return std::clamp(
        static_cast<float>(distance) / static_cast<float>(th2::bright_neutral),
        0.0f, 1.0f);
}

void Game::update_avg_back(int steps)
{
    // AVG_ControlBack, from AVG_System's chain:
    //     AVG_ControlBackFade(); AVG_ControlBackChange();
    //     AVG_ControlBackScroll(); AVG_ControlShake();
    // Every one of them increments its own counter and re-asks AVG_EffCnt,
    // so a step is a whole frame of the original and nothing measures
    // itself against a clock.
    for (int i = 0; i < steps; ++i) {
        avgback().control_back();
    }
    update_transition();
    update_background_fade();
    update_background_scroll();
}

void Game::update_shake()
{
    // AVG_ControlShake owns sk_cnt and calls AVG_StopShake itself, which is
    // where SetCharPosShake(0,0,OFF) puts the characters back in the plate.
}

Game::ShakeSample Game::shake_sample()
{
    ShakeSample result;
    if (!back().sk_flag) {
        return result;
    }
    // AVG_ControlShake has already run this frame and worked the numbers out
    // in its own integer arithmetic - see AvgBack::ShakeOut.  The draw only
    // applies them.  This used to derive them a second time in floating
    // point, which truncates once at the end where the engine truncates
    // after every step, and on SHAKE_ROLL_SIN (010301100.sdt, Q 13 20 40 0)
    // that put the whole frame a 256th of a turn away from the reference.
    const auto& out = avgback().shake_out();
    const int type = back().sk_type;
    result.x = static_cast<float>(out.x);
    result.y = static_cast<float>(out.y);
    if (out.roll >= 0) {
        result.roll_rate = out.roll;
        result.angle = static_cast<double>(out.roll) * 360.0 / 256.0;
    }
    if (type == th2::shake_zoom && out.zoom >= 0) {
        // SHAKE_ZOOM alone sets DRW_BLD(128) on the background while it runs.
        result.zoom_256 = out.zoom;
        result.scale = 1.0f + static_cast<float>(out.zoom) / 256.0f;
        result.half_blend = true;
    }
    result.text_only = type == th2::shake_txt_sin
        || type == th2::shake_txt_2ti || type == th2::shake_txt_rand;
    result.includes_text = type == th2::shake_all_sin
        || type == th2::shake_all_2ti || type == th2::shake_all_rand
        || type == th2::shake_all_sin_set;
    return result;
}

Game::BackgroundView Game::current_background_view(double extra) const
{
    if (!background_scroll_) {
        return background_view_;
    }
    // AVG_ControlBackScroll's cnt/max, with AVG_EffCnt4 re-asked here the
    // same way it is re-asked there.
    const int back_max = std::max(1, effect_frames4(back().sc_max));
    const float raw = std::clamp(
        static_cast<float>(back().sc_cnt + extra)
            / static_cast<float>(back_max),
        0.0f, 1.0f);
    const float progress = background_scroll_->easing == 1
        ? raw * raw
        : background_scroll_->easing == 2
            ? 1.0f - (1.0f - raw) * (1.0f - raw)
            : raw;
    if (background_scroll_->zoom) {
        const auto zoom_axis = [progress](
            float from_position, float from_size,
            float to_position, float to_size) {
            const float inverse_size =
                (1.0f - progress) / from_size + progress / to_size;
            const float size = 1.0f / inverse_size;
            const float position =
                ((1.0f - progress) * from_position / from_size
                 + progress * to_position / to_size)
                * size;
            return std::pair{position, size};
        };
        const auto [x, width] = zoom_axis(
            background_scroll_->from.x,
            background_scroll_->from.width,
            background_scroll_->to.x,
            background_scroll_->to.width);
        const auto [y, height] = zoom_axis(
            background_scroll_->from.y,
            background_scroll_->from.height,
            background_scroll_->to.y,
            background_scroll_->to.height);
        return {x, y, width, height};
    }
    const auto mix = [progress](float from, float to) {
        return from + (to - from) * progress;
    };
    return {
        mix(background_scroll_->from.x, background_scroll_->to.x),
        mix(background_scroll_->from.y, background_scroll_->to.y),
        mix(background_scroll_->from.width, background_scroll_->to.width),
        mix(background_scroll_->from.height, background_scroll_->to.height),
    };
}

void Game::update_background_scroll()
{
    if (!background_scroll_) {
        return;
    }
    // AVG_ControlBackScroll owns sc_cnt and runs AVG_CopyBack(ON) and
    // AVG_SetBackChar at the end itself; this lets go of the view the draw
    // interpolates once sc_flag has cleared.
    if (!back().sc_flag) {
        background_view_ = background_scroll_->to;
        background_scroll_.reset();
        // AVG_ControlBackScroll ends with
        //     AVG_CopyBack(ON);
        //     AVG_SetBackChar( BackStruct.x, BackStruct.y, ON );
        // because the characters were composited into the plate at the
        // window's old offset.  AVG_ControlChar's invalidation is suppressed
        // for the whole scroll (BackStruct.sc_flag), so this is the only
        // thing that puts them back where they belong.
        background_baked_dirty_ = true;
        // Nothing to resume: the parked instruction re-asks its own wait.
    }
}


}  // namespace th2app
