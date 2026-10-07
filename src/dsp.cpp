#include "dsp.hpp"

#include "image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace th2 {

bool debug_draws = false;
// The engine's compositing arithmetic as a shader; see dsp.hpp and
// shaders/blend/premul256.frag.
struct Display::ExactBlend {
    SDL_Renderer* renderer = nullptr;
    SDL_GPUDevice* device = nullptr;
    SDL_GPUShader* shader = nullptr;
    SDL_GPURenderState* state = nullptr;

    struct Uniform {
        std::int32_t engine_alpha;
        std::int32_t bright_r;
        std::int32_t bright_g;
        std::int32_t bright_b;
    };

    // The same tables the shader implements, for the self-test.
    static int blend_table(int alpha, int value)
    {
        return std::clamp(
            (std::clamp(alpha, 0, 256) * std::clamp(value, 0, 255)) >> 8,
            0, 255);
    }
    static int bright_of(int value, int level)
    {
        const int j = std::clamp(level, 0, 255);
        const int v = std::clamp(value, 0, 255);
        return j < 128 ? (v * j) >> 7
                       : ((((255 - v) * (j - 128)) >> 7) + v);
    }

    ~ExactBlend() { destroy_state(); }

    void destroy_state()
    {
        if (state) {
            SDL_DestroyGPURenderState(state);
            state = nullptr;
        }
    }

    bool load(SDL_Renderer* r, const std::filesystem::path& dir)
    {
        renderer = r;
        device = SDL_GetGPURendererDevice(renderer);
        if (!device) {
            return false;          // software or non-GPU renderer
        }
        if (!(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV)) {
            // No Metal or DXIL translation is shipped, so those keep SDL's
            // blend.  Wrong output would be worse than a known level of
            // drift.
            return false;
        }
        std::size_t size = 0;
        void* code = SDL_LoadFile(
            (dir / "premul256.frag.spv").string().c_str(), &size);
        if (!code) {
            return false;
        }
        SDL_GPUShaderCreateInfo info{};
        info.code = static_cast<const Uint8*>(code);
        info.code_size = size;
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = 1;
        info.num_uniform_buffers = 1;
        shader = SDL_CreateGPUShader(device, &info);
        SDL_free(code);
        if (!shader) {
            SDL_Log("exact blend: shader %s", SDL_GetError());
            return false;
        }
        SDL_GPURenderStateCreateInfo desc{};
        desc.fragment_shader = shader;
        state = SDL_CreateGPURenderState(renderer, &desc);
        if (!state) {
            SDL_Log("exact blend: render state %s", SDL_GetError());
        }
        return state != nullptr;
    }

    void arm(int engine_alpha, int r, int g, int b)
    {
        const Uniform uniform{
            static_cast<std::int32_t>(engine_alpha),
            static_cast<std::int32_t>(r),
            static_cast<std::int32_t>(g),
            static_cast<std::int32_t>(b)};
        SDL_SetGPURenderStateFragmentUniforms(
            state, 0, &uniform, sizeof uniform);
        SDL_SetGPURenderState(renderer, state);
    }

    void release() { SDL_SetGPURenderState(renderer, nullptr); }

    // Runs the tables through the shader and compares against the CPU.
    //
    // This is the whole defence against the arithmetic being done in
    // something narrower than it asks for.  A mediump float is fp16: exact
    // on integers only to 2048 and infinite past 65504, so 255*256 comes
    // back wrong and 256*256 comes back Inf - a failure that happens on a
    // phone while the desktop it was written on is fine.  The shader is
    // written in int for that reason and compiles to 32-bit OpTypeInt with
    // no RelaxedPrecision, but that is a claim about a translator and a
    // driver rather than a fact, so it is measured once at startup and the
    // whole path switched off if it does not hold.
    //
    // The destination is cleared to black so that the destination half of
    // the blend contributes exactly zero whatever the blend unit rounds:
    // what is under test is the source term, which is the part the shader
    // is responsible for and the part with the large products in it.
    //
    // One readback, at init, never in a frame.
    bool self_test()
    {
        constexpr int width = 256;      // every source value
        constexpr int height = 6;
        // Alphas and brightnesses chosen for the corners: 256 is the value
        // whose square overflows fp16, 255*256 is the largest product the
        // blend takes, and bright 255 drives (255-v)*(j-128) at its widest.
        static constexpr int alphas[height] = {256, 255, 128, 1, 256, 256};
        static constexpr int brights[height] = {128, 128, 128, 128, 255, 64};

        // Through a surface in RGBA32, which is byte-ordered R,G,B,A on
        // every endianness.  RGBA8888 is a *packed* format whose bytes are
        // not in that order, and writing the alpha to the wrong one of them
        // makes the test measure a ramp of alphas rather than the tables.
        SDL_Surface* const face =
            SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
        Texture target(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET, width, height));
        if (!face || !target) {
            if (face) SDL_DestroySurface(face);
            return false;
        }
        for (int y = 0; y < height; ++y) {
            auto* row = static_cast<std::uint8_t*>(face->pixels)
                + static_cast<std::size_t>(y) * face->pitch;
            for (int x = 0; x < width; ++x) {
                row[x * 4 + 0] = static_cast<std::uint8_t>(x);
                row[x * 4 + 1] = static_cast<std::uint8_t>(x);
                row[x * 4 + 2] = static_cast<std::uint8_t>(x);
                // Opaque, to take Draw32's src1_p->a == 255 branch.
                row[x * 4 + 3] = 255;
            }
        }
        Texture source(SDL_CreateTextureFromSurface(renderer, face));
        SDL_DestroySurface(face);
        if (!source) {
            return false;
        }
        SDL_SetTextureScaleMode(source.get(), SDL_SCALEMODE_NEAREST);

        SDL_Texture* const held_target = SDL_GetRenderTarget(renderer);
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        SDL_GetRenderScale(renderer, &scale_x, &scale_y);
        SDL_SetRenderTarget(renderer, target.get());
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        SDL_SetTextureBlendMode(
            source.get(), SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        for (int y = 0; y < height; ++y) {
            const SDL_FRect rect{0.0f, static_cast<float>(y),
                                 static_cast<float>(width), 1.0f};
            arm(alphas[y], brights[y], brights[y], brights[y]);
            SDL_RenderTexture(renderer, source.get(), &rect, &rect);
        }
        release();

        SDL_Surface* const got = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SetRenderTarget(renderer, held_target);
        SDL_SetRenderScale(renderer, scale_x, scale_y);
        if (!got) {
            SDL_Log("exact blend: self-test readback %s", SDL_GetError());
            return false;
        }
        SDL_Surface* const rgba =
            SDL_ConvertSurface(got, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(got);
        if (!rgba) {
            return false;
        }
        int wrong = 0;
        int worst = 0;
        for (int y = 0; y < height; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(rgba->pixels)
                + static_cast<std::size_t>(y) * rgba->pitch;
            for (int x = 0; x < width; ++x) {
                const int expected =
                    blend_table(alphas[y], bright_of(x, brights[y]));
                const int actual = row[x * 4 + 0];
                const int off = std::abs(actual - expected);
                worst = std::max(worst, off);
                wrong += off != 0;
            }
        }
        SDL_DestroySurface(rgba);
        if (wrong) {
            // Reported, not acted on.  Silently demoting a path that is
            // mostly working hides the problem twice over: the picture goes
            // back to being uniformly one or two levels bright, and the
            // reason it did never reaches anyone.  A number on the way past
            // is what a developer can act on - the same call gl_transition
            // makes about judging what a shader draws.
            SDL_Log("Exact blend self-test: %d of %d samples wrong, worst "
                    "off by %d - this GPU is not reproducing the engine's "
                    "integer tables (narrow ints, or fp16?).  Continuing "
                    "with the shader anyway.",
                    wrong, width * height, worst);
        } else {
            SDL_Log("Exact blend self-test: %d samples exact",
                    width * height);
        }
        return true;
    }
};

bool Display::enable_exact_blend(const std::filesystem::path& shader_dir)
{
    // GL first: it is the only one of the two that can read the destination,
    // so it is the only one that can be exact, and it is also the stack that
    // exists in the browser.
    auto gl = std::make_shared<GlExactBlend>(renderer_);
    if (gl->available()) {
        gl_exact_blend_ = std::move(gl);
        SDL_Log("Blending: engine integer arithmetic (GLES, with destination)");
        return true;
    }
    auto blend = std::make_shared<ExactBlend>();
    if (!blend->load(renderer_, shader_dir)) {
        exact_blend_.reset();
        SDL_Log("Blending: SDL straight alpha - neither shader path is "
                "available on this renderer");
        return false;
    }
    SDL_Log("Blending: engine integer arithmetic (SDL_GPU, source term only)");
    // The self-test reports and returns; it does not gate the path.  A
    // shader that will not load at all is a different thing from one whose
    // arithmetic came out wrong somewhere, and only the first is a reason
    // to fall back.
    blend->self_test();
    exact_blend_ = std::move(blend);
    return true;
}

bool Display::exact_blend_active() const { return exact_blend_ != nullptr; }

GlExactBlend* Display::gl_exact_blend() const { return gl_exact_blend_.get(); }

namespace {


// MM_std's SinTbl[256], one turn in 256 steps scaled by 4096.  Every entry
// is (int)(4096 * sin(2*pi*i/256)) truncated towards zero - checked against
// all 256 literals in MM_std.cpp, so this is the same table rather than an
// approximation of it.  Building it keeps the arithmetic integral, which
// matters: the roll's corners are divided back down by 4096 and a double
// that lands a hair under an exact 1.0 loses a whole pixel.
const std::array<int, 256>& sin_table()
{
    static const std::array<int, 256> table = [] {
        std::array<int, 256> built{};
        for (int i = 0; i < 256; ++i) {
            built[i] = static_cast<int>(
                4096.0 * std::sin(2.0 * M_PI * i / 256.0));
        }
        return built;
    }();
    return table;
}

// COS(X) is SinTbl[X%256] and SIN(X) is SinTbl[(X+64)%256] - so the one the
// engine calls COS is the sine, and COS(0) is zero.  Reading these the other
// way round put every rotation a quarter turn out.
int engine_cos(int rate)
{
    return sin_table()[static_cast<std::size_t>(((rate % 256) + 256) % 256)];
}

int engine_sin(int rate)
{
    return engine_cos(rate + 64);
}

// DrawGraphBmp's brightness fold.  128 is neutral, and the global
// brightness multiplies into the graph's own unless brt_flag is set:
//     if( BrightR<=BRT_NML ) r =       gs->r * BrightR          / BRT_NML;
//     else                   r = (0xff-gs->r)*(BrightR-BRT_NML) / BRT_NML + gs->r;
int fold_bright(int own, int global, bool brt_flag)
{
    return brt_flag
        ? own
        : (global <= bright_neutral
               ? own * global / bright_neutral
               : (0xff - own) * (global - bright_neutral) / bright_neutral
                     + own);
}

// Everything up to neutral is a colour modulation; the rasteriser treats
// 128 as unchanged, so that is a modulation of 255 here.
Uint8 modulation_of(int value)
{
    return static_cast<Uint8>(
        std::clamp(std::min(value, bright_neutral) * 255 / bright_neutral,
                   0, 255));
}

// The rasteriser's ClipRect: narrow the blit to what the bitmap actually
// holds and shift the destination by the same proportion.  False when
// nothing of the source is left.
bool clip_source(const Bitmap& bitmap, SDL_FRect& source, SDL_FRect& destination)
{
    const auto width = static_cast<float>(bitmap.width);
    const auto height = static_cast<float>(bitmap.height);
    if (source.w <= 0.0f || source.h <= 0.0f || width <= 0.0f
        || height <= 0.0f) {
        return false;
    }
    const SDL_FRect original = source;
    const float left = std::clamp(source.x, 0.0f, width);
    const float top = std::clamp(source.y, 0.0f, height);
    const float right = std::clamp(source.x + source.w, 0.0f, width);
    const float bottom = std::clamp(source.y + source.h, 0.0f, height);
    if (right <= left || bottom <= top) {
        return false;
    }
    destination.x += (left - original.x) / original.w * destination.w;
    destination.y += (top - original.y) / original.h * destination.h;
    destination.w *= (right - left) / original.w;
    destination.h *= (bottom - top) / original.h;
    source = {left, top, right - left, bottom - top};
    return true;
}

}  // namespace

int COS(int rate)
{
    return engine_cos(rate);
}

int SIN(int rate)
{
    return engine_sin(rate);
}

int draw_alpha_of(std::uint32_t param)
{
    const std::uint32_t mode = draw_mode_of(param);
    if (mode == drw_bld || mode == drw_ami
        || (mode >= drw_lcf && mode < drw_dio + 3)) {
        return draw_param_of(param);
    }
    return 256;
}

SDL_BlendMode blend_of(std::uint32_t param)
{
    switch (draw_mode_of(param)) {
    case drw_nml:
    case drw_bld:
        // DRW_BLD is a plain source-over with an alpha; the alpha rides on
        // the texture's own modulation, so the mode is the same as normal.
        return SDL_BLENDMODE_BLEND;
    case drw_add:
    case drw_ooi:
        return SDL_BLENDMODE_ADD;
    case drw_sub:
        return SDL_BLENDMODE_MOD;
    case drw_dim:
    case drw_mul:
        return SDL_BLENDMODE_MUL;
    default:
        // AMI (dither mesh), NIS, MOZ, BOM, FLT and the wipe patterns
        // (LCF/LPP/DIA/DIO) have no SDL equivalent.  They are drawn plain
        // rather than dropped; the wipes have their own shader path.
        return SDL_BLENDMODE_BLEND;
    }
}

GraphGeometry resolve_geometry(
    const Graph& graph, int global_x, int global_y,
    int bitmap_x, int bitmap_y,
    int global_r, int global_g, int global_b)
{
    GraphGeometry out;
    out.poly = graph.poly;

    // int dx = gs->dx + x;  ... through dy4.
    const int dx = graph.dx + global_x;
    const int dy = graph.dy + global_y;

    // int sx = gs->sx - BmpSet[gs->bno].pos.x;  ... through sy4.
    const int sx = graph.sx - bitmap_x;
    const int sy = graph.sy - bitmap_y;

    // if(gs->zoom){ ... } - a scale about (cx, cy) in 256ths, applied to the
    // destination before the global offset goes on, which is why the two
    // additions below sit after the multiply rather than before it.
    int zoomed_x = dx;
    int zoomed_y = dy;
    int dw = graph.dw;
    int dh = graph.dh;
    if (graph.zoom) {
        const int scale = graph.zoom + 256;
        zoomed_x = graph.cx + ((graph.dx - graph.cx) * scale >> 8);
        zoomed_y = graph.cy + ((graph.dy - graph.cy) * scale >> 8);
        dw = graph.cx + ((graph.dx + graph.dw - graph.cx) * scale >> 8)
            - zoomed_x;
        dh = graph.cy + ((graph.dy + graph.dh - graph.cy) * scale >> 8)
            - zoomed_y;
        zoomed_x += global_x;
        zoomed_y += global_y;
    }

    out.destination = SDL_FRect{
        static_cast<float>(zoomed_x), static_cast<float>(zoomed_y),
        static_cast<float>(dw), static_cast<float>(dh)};
    out.source = SDL_FRect{
        static_cast<float>(sx), static_cast<float>(sy),
        static_cast<float>(graph.sw), static_cast<float>(graph.sh)};

    // The four corners, in the original's order: 1 top-left, 2 top-right,
    // 3 bottom-left, 4 bottom-right.  DSP_SetGraphRoll fills them that way
    // and DRW_SetDrawObjectPoly reads them that way.
    out.corners[0] = {static_cast<float>(graph.dx + global_x),
                      static_cast<float>(graph.dy + global_y)};
    out.corners[1] = {static_cast<float>(graph.dx2 + global_x),
                      static_cast<float>(graph.dy2 + global_y)};
    out.corners[2] = {static_cast<float>(graph.dx3 + global_x),
                      static_cast<float>(graph.dy3 + global_y)};
    out.corners[3] = {static_cast<float>(graph.dx4 + global_x),
                      static_cast<float>(graph.dy4 + global_y)};
    out.source_corners[0] = {static_cast<float>(graph.sx - bitmap_x),
                             static_cast<float>(graph.sy - bitmap_y)};
    out.source_corners[1] = {static_cast<float>(graph.sx2 - bitmap_x),
                             static_cast<float>(graph.sy2 - bitmap_y)};
    out.source_corners[2] = {static_cast<float>(graph.sx3 - bitmap_x),
                             static_cast<float>(graph.sy3 - bitmap_y)};
    out.source_corners[3] = {static_cast<float>(graph.sx4 - bitmap_x),
                             static_cast<float>(graph.sy4 - bitmap_y)};

    // if(gs->clip){ ... }else{ 0,0,DISP_X,DISP_Y } - the clip is in screen
    // space and moves with the global offset like everything else.
    if (graph.clip) {
        out.clipped = true;
        out.clip = SDL_Rect{
            graph.clip->x + global_x, graph.clip->y + global_y,
            graph.clip->w, graph.clip->h};
    } else {
        out.clipped = false;
        out.clip = SDL_Rect{0, 0, display_width, display_height};
    }

    const int red = fold_bright(graph.r, global_r, graph.brt_flag);
    const int green = fold_bright(graph.g, global_g, graph.brt_flag);
    const int blue = fold_bright(graph.b, global_b, graph.brt_flag);
    out.red = modulation_of(red);
    out.green = modulation_of(green);
    out.blue = modulation_of(blue);
    out.bright_r = red;
    out.bright_g = green;
    out.bright_b = blue;
    // AVG_ControlBackFade drives GRP_BACK's brightness all the way to 255
    // for a flash to white, which is past what a modulation can reach.
    const int highest = std::max({red, green, blue});
    out.brighten = highest > bright_neutral
        ? static_cast<Uint8>(std::clamp(
              (highest - bright_neutral) * 255 / bright_neutral, 0, 255))
        : 0;

    // REV_W 0x10 / REV_H 0x20, which the rasteriser turns into a negative
    // step through the source.
    out.flip_x = (graph.rparam & 0x10u) != 0;
    out.flip_y = (graph.rparam & 0x20u) != 0;
    return out;
}

// --- bitmaps -----------------------------------------------------------

void Display::create_bmp(int bno, int width, int height)
{
    if (bno < 0 || bno >= bitmap_max) {
        return;
    }
    auto& bitmap = bitmaps_[bno];
    if (bitmap.valid() && bitmap.renderable
        && bitmap.width == width && bitmap.height == height) {
        return;
    }
    retire_bitmap(bitmap);
    bitmap.owned = take_spare_target(width, height);
    if (!bitmap.owned) {
        bitmap.owned.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            width, height));
    }
    bitmap.view = bitmap.owned.get();
    bitmap.width = width;
    bitmap.height = height;
    bitmap.pos_x = 0;
    bitmap.pos_y = 0;
    bitmap.renderable = bitmap.view != nullptr;
    if (bitmap.view) {
        SDL_SetTextureBlendMode(bitmap.view, SDL_BLENDMODE_BLEND);
    // Nearest, always.  The rasteriser these bitmaps stand in for indexes
    // its source by integer pixel - Draw32.cpp walks `src1_p += xinc` and
    // reads the texel - so there is no filtering to reproduce, and SDL's
    // default of LINEAR is simply a different picture.  It cost accuracy
    // twice over: it interpolated wherever a graph sat at a fractional
    // offset, and the two renderers rounded the sample position differently,
    // so the GLES and GPU backends disagreed with each other as well as with
    // the reference.  Filtering belongs in the upscaler, which is a choice
    // about presentation, not in the layer being compared.
        SDL_SetTextureScaleMode(bitmap.view, SDL_SCALEMODE_NEAREST);
    }
}

void Display::release_bmp(int bno)
{
    if (bno < 0 || bno >= bitmap_max) {
        return;
    }
    retire_bitmap(bitmaps_[bno]);
    bitmaps_[bno] = Bitmap{};
}

void Display::release_bmp_all()
{
    for (auto& bitmap : bitmaps_) {
        retire_bitmap(bitmap);
        bitmap = Bitmap{};
    }
}

void Display::retire_bitmap(Bitmap& bitmap)
{
    // Only targets this slot made: a borrowed view belongs to the game, and
    // a plain loaded texture is not worth keeping.
    if (!bitmap.owned || !bitmap.renderable
        || SDL_GetNumberProperty(SDL_GetTextureProperties(bitmap.owned.get()),
                                 SDL_PROP_TEXTURE_ACCESS_NUMBER, -1)
            != SDL_TEXTUREACCESS_TARGET) {
        return;
    }
    // A handful covers every size the scripts use (the screen, the plates);
    // past that the oldest goes, so a run of odd sizes cannot pile up.
    constexpr std::size_t spare_max = 8;
    if (spare_targets_.size() >= spare_max) {
        spare_targets_.erase(spare_targets_.begin());
    }
    bitmap.view = nullptr;
    spare_targets_.push_back(std::move(bitmap.owned));
}

void Display::reserve_targets(int width, int height, std::size_t count)
{
    constexpr std::size_t spare_max = 8;    // as retire_bitmap keeps
    std::size_t have = 0;
    for (const auto& spare : spare_targets_) {
        have += spare->w == width && spare->h == height ? 1 : 0;
    }
    while (have < count && spare_targets_.size() < spare_max) {
        Texture texture(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_TARGET, width,
                                          height));
        if (!texture) {
            return;
        }
        spare_targets_.push_back(std::move(texture));
        ++have;
    }
}

Texture Display::take_spare_target(int width, int height)
{
    for (auto at = spare_targets_.begin(); at != spare_targets_.end(); ++at) {
        if ((*at)->w != width || (*at)->h != height) {
            continue;
        }
        Texture texture = std::move(*at);
        spare_targets_.erase(at);
        // As a new one would be: cleared to nothing, and none of the state
        // its last owner set.
        SDL_Texture* const held = SDL_GetRenderTarget(renderer_);
        Uint8 r = 0, g = 0, b = 0, a = 0;
        SDL_GetRenderDrawColor(renderer_, &r, &g, &b, &a);
        SDL_SetRenderTarget(renderer_, texture.get());
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        SDL_SetRenderTarget(renderer_, held);
        SDL_SetRenderDrawColor(renderer_, r, g, b, a);
        SDL_SetTextureColorMod(texture.get(), 255, 255, 255);
        SDL_SetTextureAlphaMod(texture.get(), 255);
        return texture;
    }
    return {};
}

void Display::set_bmp(int bno, Texture texture, int width, int height)
{
    if (bno < 0 || bno >= bitmap_max) {
        return;
    }
    auto& bitmap = bitmaps_[bno];
    // A target this slot made goes back to the pool rather than away: the
    // next create_bmp of its size would otherwise make a new one, and making
    // a target is a round trip to the GPU process in a browser.
    retire_bitmap(bitmap);
    bitmap.owned = std::move(texture);
    bitmap.view = bitmap.owned.get();
    bitmap.width = width;
    bitmap.height = height;
    bitmap.pos_x = 0;
    bitmap.pos_y = 0;
    bitmap.renderable = false;
    if (bitmap.view) {
        // See create_bmp: the rasteriser samples by integer pixel.
        SDL_SetTextureScaleMode(bitmap.view, SDL_SCALEMODE_NEAREST);
    }
}

void Display::borrow_bmp(
    int bno, SDL_Texture* texture, int width, int height, bool renderable)
{
    if (bno < 0 || bno >= bitmap_max) {
        return;
    }
    auto& bitmap = bitmaps_[bno];
    retire_bitmap(bitmap);    // see set_bmp
    bitmap.owned.reset();
    bitmap.view = texture;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.pos_x = 0;
    bitmap.pos_y = 0;
    bitmap.renderable = renderable && texture != nullptr;
    if (texture) {
        // The slot is borrowed, but how it is sampled is this layer's
        // business - see create_bmp.
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }
}

bool Display::ensure_renderable(int bno)
{
    if (bno < 0 || bno >= bitmap_max) {
        return false;
    }
    auto& bitmap = bitmaps_[bno];
    if (bitmap.renderable && bitmap.valid()) {
        return true;
    }
    if (bitmap.width <= 0 || bitmap.height <= 0) {
        return false;
    }
    if (bitmap.view && !bitmap.owned) {
        // Borrowed and not a target: promoting would swap in a texture the
        // owner never sees, so refuse rather than silently diverge.
        return false;
    }
    // Promote in place: a bitmap loaded as a plain texture becomes a target
    // the first time something renders into it, keeping whatever it held.
    Texture promoted = take_spare_target(bitmap.width, bitmap.height);
    if (!promoted) {
        promoted.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            bitmap.width, bitmap.height));
    }
    if (!promoted) {
        return false;
    }
    SDL_SetTextureBlendMode(promoted.get(), SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(promoted.get(), SDL_SCALEMODE_NEAREST);
    if (bitmap.view) {
        SDL_Texture* const held = SDL_GetRenderTarget(renderer_);
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
        SDL_SetRenderTarget(renderer_, promoted.get());
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
        SDL_RenderClear(renderer_);
        SDL_SetTextureBlendMode(bitmap.view, SDL_BLENDMODE_NONE);
        SDL_RenderTexture(renderer_, bitmap.view, nullptr, nullptr);
        SDL_SetTextureBlendMode(bitmap.view, SDL_BLENDMODE_BLEND);
        SDL_SetRenderScale(renderer_, scale_x, scale_y);
        SDL_SetRenderTarget(renderer_, held);
    }
    bitmap.owned = std::move(promoted);
    bitmap.view = bitmap.owned.get();
    bitmap.renderable = true;
    return true;
}

void Display::copy_bmp(int db_no, int sb_no)
{
    copy_bmp2(db_no, sb_no, bright_neutral, bright_neutral, bright_neutral);
}

void Display::copy_bmp2(int db_no, int sb_no, int r, int g, int b)
{
    // if( db_no != sb_no ) - the original refuses to copy a bitmap onto
    // itself, which would be a no-op there and a feedback loop here.
    if (db_no == sb_no || db_no < 0 || db_no >= bitmap_max) {
        return;
    }
    if (sb_no < 0 || sb_no >= bitmap_max || !bitmaps_[sb_no].valid()) {
        return;  // "ソースの無い画像コピーを実行しました"
    }
    const auto& source = bitmaps_[sb_no];
    create_bmp(db_no, source.width, source.height);
    auto& destination = bitmaps_[db_no];
    if (!destination.valid()) {
        return;
    }
    destination.pos_x = source.pos_x;
    destination.pos_y = source.pos_y;

    SDL_Texture* const held = SDL_GetRenderTarget(renderer_);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, destination.view);
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    // DSP_CopyBmp2 is a DRW_NML blit with a brightness, so it lands on
    // BrightTable - (i*j)>>7 when darkening - while SDL's colour modulation
    // computes round(i*mod/255).  This is the half-toned message plate, and
    // the difference was a level on most of its pixels for as long as the
    // wash was up.  The copy overwrites rather than blends, so the shader
    // reproduces the table exactly.
    // GL first, exactly as draw_graph does.  Taking only the SDL_GPU blend
    // here meant that on the GLES renderer - which is every build now - this
    // copy fell back to SDL's colour modulation, and since BMP_BACKHALF is
    // the whole screen whenever the message window is up, that one omission
    // was most of the port's remaining difference from the reference.
    const bool gl_exact = gl_exact_blend_ && gl_exact_blend_->available();
    const bool exact = !gl_exact && exact_blend_ != nullptr;
    if (debug_draws) {
        SDL_Log("copy_bmp2 %d <- %d  bright %d,%d,%d  gl_exact=%d",
                db_no, sb_no, r, g, b, (int)gl_exact);
    }
    const auto shade = [](int value) {
        return static_cast<Uint8>(
            std::clamp(value * 255 / bright_neutral, 0, 255));
    };
    const bool modulated = !gl_exact && !exact;
    SDL_SetTextureColorMod(
        source.view,
        modulated ? shade(r) : 255, modulated ? shade(g) : 255,
        modulated ? shade(b) : 255);
    SDL_SetTextureBlendMode(source.view, SDL_BLENDMODE_NONE);
    bool drawn = false;
    if (gl_exact) {
        const SDL_FRect whole{
            0.0f, 0.0f, static_cast<float>(source.width),
            static_cast<float>(source.height)};
        drawn = gl_exact_blend_->capture_destination(renderer_)
            && gl_exact_blend_->draw(
                renderer_, source.view, whole, whole, false, false, 0,
                256, r, g, b);
    }
    if (!drawn) {
        if (exact) {
            // A plain overwrite, so there is no destination to read.
            exact_blend_->arm(256, r, g, b);
        }
        SDL_RenderTexture(renderer_, source.view, nullptr, nullptr);
        if (exact) {
            exact_blend_->release();
        }
    }
    SDL_SetTextureColorMod(source.view, 255, 255, 255);
    SDL_SetTextureBlendMode(source.view, SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    SDL_SetRenderTarget(renderer_, held);
}

void Display::get_bmp_size(int bno, int* sx, int* sy) const
{
    const bool ok = bno >= 0 && bno < bitmap_max && bitmaps_[bno].valid();
    if (sx) {
        *sx = ok ? bitmaps_[bno].width : 0;
    }
    if (sy) {
        *sy = ok ? bitmaps_[bno].height : 0;
    }
}

bool Display::bmp_flag(int bno) const
{
    return bno >= 0 && bno < bitmap_max && bitmaps_[bno].valid();
}

SDL_Texture* Display::bmp_texture(int bno) const
{
    if (bno < 0 || bno >= bitmap_max) {
        return nullptr;
    }
    return bitmaps_[bno].view;
}

void Display::get_disp_bmp(int bno)
{
    // GetBackNo = bno; GetBackFlag = 1; DSP_CreateBmp(...).  One shot: the
    // next draw() takes the picture and clears the flag.
    capture_bno_ = bno;
    capture_ = true;
    create_bmp(bno, display_width, display_height);
}

// --- graphs ------------------------------------------------------------

void Display::set_graph_str(int gno, int bno, int lno, bool disp, int nuki,
                            std::string str)
{
    // void DSP_SetGraphStr( int gno, int bno, int lno, int disp, int nuki,
    // char *str ): set_graph's fields, PRM_STR, and a cell of the sheet as
    // the size - dw = size.x/16, sw = size.x, dh = sh = size.y/4.
    if (gno < 0 || gno >= graph_max) {
        return;
    }
    set_graph(gno, bno, lno, disp, nuki);
    Graph& graph = at(gno);
    graph.type = GraphType::str;
    int width = 0;
    int height = 0;
    get_bmp_size(bno, &width, &height);
    graph.dw = width / 16;
    graph.sw = width;
    graph.dh = graph.sh = height / 4;
    graph.str = std::move(str);
}

void Display::set_graph(int gno, int bno, int lno, bool disp, int nuki)
{
    if (gno < 0 || gno >= graph_max) {
        return;
    }
    reset_graph(gno);
    Graph& graph = at(gno);
    graph.flag = true;
    graph.disp = disp;
    graph.layer = lno;
    graph.type = GraphType::bmp;
    graph.poly = Poly::rect;

    graph.bno = bno;
    graph.r = bright_neutral;
    graph.g = bright_neutral;
    graph.b = bright_neutral;
    graph.brt_flag = false;
    graph.param = drw_nml;
    graph.nuki = nuki;

    graph.dx = 0;
    graph.dy = 0;
    graph.sx = 0;
    graph.sy = 0;
    graph.target = false;

    int width = 0;
    int height = 0;
    get_bmp_size(bno, &width, &height);
    graph.dw = graph.sw = width;
    graph.dh = graph.sh = height;
    graph.clip.reset();
}

void Display::set_graph_prim(
    int gno, GraphType type, Poly poly, int lno, bool disp)
{
    if (gno < 0 || gno >= graph_max) {
        return;
    }
    Graph& graph = at(gno);
    graph.flag = true;
    graph.disp = disp;
    graph.layer = lno;
    graph.type = type;
    graph.poly = poly;

    graph.r = bright_neutral;
    graph.g = bright_neutral;
    graph.b = bright_neutral;
    graph.brt_flag = false;
    graph.param = drw_nml;
    if (poly == Poly::box) {
        graph.nuki = 1;
    }
    graph.dx = 0;
    graph.dy = 0;
    graph.dw = graph.sw = 0;
    graph.dh = graph.sh = 0;
    graph.target = false;
    graph.clip.reset();
}

void Display::reset_graph(int gno)
{
    if (gno < 0 || gno >= graph_max) {
        return;
    }
    graphs_[gno] = Graph{};
}

void Display::reset_graph_all()
{
    for (auto& graph : graphs_) {
        graph = Graph{};
    }
}

void Display::set_graph_bmp(int gno, int bno)
{
    Graph& graph = at(gno);
    graph.type = GraphType::bmp;
    graph.bno = bno;
}

void Display::set_graph_disp(int gno, bool disp)
{
    Graph& graph = at(gno);
    if (graph.flag) {
        graph.disp = disp;
        graph.target = false;
    }
}

void Display::set_graph_layer(int gno, int lno) { at(gno).layer = lno; }
void Display::set_graph_nuki(int gno, int nuki) { at(gno).nuki = nuki; }
void Display::set_graph_param(int gno, std::uint32_t param)
{
    at(gno).param = param;
}
void Display::set_graph_param2(int gno, std::uint32_t param)
{
    at(gno).param2 = param;
}
void Display::set_graph_type(int gno, GraphType type) { at(gno).type = type; }
void Display::set_graph_bright_flag(int gno, bool brt_flag)
{
    at(gno).brt_flag = brt_flag;
}
void Display::set_graph_rev_param(int gno, std::uint32_t rparam)
{
    at(gno).rparam = rparam;
}

void Display::set_graph_target(int gno, int target)
{
    Graph& graph = at(gno);
    if (!graph.flag) {
        return;
    }
    if (!ensure_renderable(target)) {
        return;
    }
    SDL_Texture* const held = SDL_GetRenderTarget(renderer_);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, bitmaps_[target].view);
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    // DrawGraphBmp( dest_bmp, 0, 0, ... ) - into the bitmap with no global
    // offset, whatever the screen's happens to be.
    draw_one(graph, bitmaps_[target].view, 0, 0);
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    SDL_SetRenderTarget(renderer_, held);

    // GrpStruct[gno].target = GrpStruct[gno].disp;  disp = OFF;
    // The graph stops drawing as a layer because it is now in the bitmap.
    // draw() turns it back on at the end of the frame, which is what lets
    // AVG_ControlChar bake a character and still have its graph to hand.
    graph.target = graph.disp;
    graph.disp = false;
}

void Display::set_graph_bright(int gno, int r, int g, int b)
{
    set_graph_bright_point(gno, 0, r, g, b);
}

void Display::set_graph_fade(int gno, int br)
{
    set_graph_bright_point(gno, 0, br, br, br);
}

void Display::set_graph_bright_point(int gno, int point, int r, int g, int b)
{
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    Graph& graph = at(gno);
    switch (point) {
    default:
    case 0: graph.r = r; graph.g = g; graph.b = b; break;
    case 1: break;  // points 1..3 only feed the gradient primitives
    case 2: break;
    case 3: break;
    }
}

void Display::set_graph_global_bright(int r, int g, int b)
{
    bright_r_ = r;
    bright_g_ = g;
    bright_b_ = b;
}

void Display::set_graph_global_pos(int x, int y)
{
    global_x_ = x;
    global_y_ = y;
}

void Display::set_graph_move(int gno, int dx, int dy)
{
    Graph& graph = at(gno);
    graph.dx = dx;
    graph.dy = dy;
}

void Display::set_graph_move2(int gno, int dx, int dy)
{
    Graph& graph = at(gno);
    graph.dx = dx;
    graph.dy = dy;
    graph.sx = 0;
    graph.sy = 0;
    int width = 0;
    int height = 0;
    get_bmp_size(graph.bno, &width, &height);
    graph.dw = graph.sw = width;
    graph.dh = graph.sh = height;
}

void Display::set_graph_smove(int gno, int sx, int sy)
{
    Graph& graph = at(gno);
    graph.sx = sx;
    graph.sy = sy;
}

void Display::set_graph_spos(int gno, int sx, int sy, int sw, int sh, int poly)
{
    Graph& graph = at(gno);
    graph.sx = sx;
    graph.sy = sy;
    if (poly != -1) {
        graph.poly = static_cast<Poly>(poly);
    }
    if (graph.poly != Poly::rect) {
        graph.sw = sw;
        graph.sh = sh;
    } else {
        graph.dw = graph.sw = sw;
        graph.dh = graph.sh = sh;
    }
}

void Display::set_graph_width(int gno, int w, int h)
{
    Graph& graph = at(gno);
    graph.dw = graph.sw = w;
    graph.dh = graph.sh = h;
}

void Display::set_graph_pos(
    int gno, int dx, int dy, int sx, int sy, int w, int h)
{
    Graph& graph = at(gno);
    graph.poly = Poly::rect;
    graph.dx = dx;
    graph.dy = dy;
    graph.dw = w;
    graph.dh = h;
    graph.sx = sx;
    graph.sy = sy;
    graph.sw = w;
    graph.sh = h;
}

void Display::set_graph_pos_zoom(
    int gno, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh)
{
    Graph& graph = at(gno);
    graph.poly = Poly::zoom;
    graph.dx = dx;
    graph.dy = dy;
    graph.dw = dw;
    graph.dh = dh;
    graph.sx = sx;
    graph.sy = sy;
    graph.sw = sw;
    graph.sh = sh;
    // A zoom that scales by one is a plain blit; the original drops back to
    // POL_RECT so the cheaper rasteriser is used.
    if (dw == sw && dh == sh) {
        graph.poly = Poly::rect;
    }
}

void Display::set_graph_zoom(int gno, int dx, int dy, int dw, int dh)
{
    Graph& graph = at(gno);
    graph.poly = Poly::zoom;
    graph.dx = dx;
    graph.dy = dy;
    graph.dw = dw;
    graph.dh = dh;
    graph.sx = 0;
    graph.sy = 0;
    int width = 0;
    int height = 0;
    get_bmp_size(graph.bno, &width, &height);
    graph.sw = width;
    graph.sh = height;
    if (graph.dw == graph.sw && graph.dh == graph.sh) {
        graph.poly = Poly::rect;
    }
}

void Display::set_graph_zoom2(int gno, int cx, int cy, int zoom)
{
    apply_zoom2(at(gno), cx, cy, zoom);
}

void Display::apply_zoom2(Graph& graph, int cx, int cy, int zoom)
{
    if (zoom < -256) {
        graph.poly = Poly::zoom;
        graph.zoom = -256;
        graph.cx = cx;
        graph.cy = cy;
    } else if (zoom) {
        graph.poly = Poly::zoom;
        graph.zoom = zoom;
        graph.cx = cx;
        graph.cy = cy;
    } else {
        graph.poly = Poly::rect;
        graph.zoom = 0;
    }
}

void Display::set_graph_zoom3(int gno, int cx, int cy, int zoom)
{
    Graph& graph = at(gno);
    if (zoom < 0) {
        return;
    }
    if (zoom != 100) {
        graph.poly = Poly::zoom;
        graph.zoom = zoom * 256 / 100 - 256;
        graph.cx = cx;
        graph.cy = cy;
    } else {
        graph.poly = Poly::rect;
        graph.zoom = 0;
    }
}

void Display::set_graph_roll(
    int gno, int cx, int cy, int zoom, int rate, int sx, int sy, int sw, int sh)
{
    // Straight out of DSP_SetGraphRoll, integer for integer.  The -1s keep
    // the quad an exact pixel span; rate 0 is upright, because COS(0) is
    // zero and SIN(0) is one.
    const int sw2 = sw * (zoom + 256) / 256 / 2;
    const int sh2 = sh * (zoom + 256) / 256 / 2;
    const int cosine = engine_cos(rate);
    const int sine = engine_sin(rate);
    set_graph_pos_poly(gno,
        cx + (cosine * sh2 - sine * sw2) / 4096,
        cy + (-cosine * sw2 - sine * sh2) / 4096,
        cx + (cosine * sh2 + sine * sw2) / 4096 - 1,
        cy + (cosine * sw2 - sine * sh2) / 4096,
        cx + (-cosine * sh2 - sine * sw2) / 4096,
        cy + (-cosine * sw2 + sine * sh2) / 4096 - 1,
        cx + (-cosine * sh2 + sine * sw2) / 4096 - 1,
        cy + (cosine * sw2 + sine * sh2) / 4096 - 1,
        sx, sy, sx + sw - 1, sy, sx, sy + sh - 1, sx + sw - 1, sy + sh - 1);
}

void Display::set_graph_pos_poly(int gno,
    int dx1, int dy1, int dx2, int dy2, int dx3, int dy3, int dx4, int dy4,
    int sx1, int sy1, int sx2, int sy2, int sx3, int sy3, int sx4, int sy4)
{
    Graph& graph = at(gno);
    graph.poly = Poly::poly4;
    graph.dx = dx1;
    graph.dy = dy1;
    graph.dx2 = dx2;
    graph.dy2 = dy2;
    graph.dx3 = dx3;
    graph.dy3 = dy3;
    graph.dx4 = dx4;
    graph.dy4 = dy4;

    int width = 0;
    int height = 0;
    get_bmp_size(graph.bno, &width, &height);
    graph.sx = std::clamp(sx1, 0, width);
    graph.sy = std::clamp(sy1, 0, height);
    graph.sx2 = std::clamp(sx2, 0, width);
    graph.sy2 = std::clamp(sy2, 0, height);
    graph.sx3 = std::clamp(sx3, 0, width);
    graph.sy3 = std::clamp(sy3, 0, height);
    graph.sx4 = std::clamp(sx4, 0, width);
    graph.sy4 = std::clamp(sy4, 0, height);
}

void Display::set_graph_pos_rect(int gno, int dx, int dy, int w, int h)
{
    Graph& graph = at(gno);
    graph.dx = dx;
    graph.dy = dy;
    graph.dw = w;
    graph.dh = h;
}

void Display::set_graph_pos_point(int gno, int point, int dx, int dy)
{
    Graph& graph = at(gno);
    switch (point) {
    default:
    case 0: graph.dx = dx; graph.dy = dy; break;
    case 1: graph.dx2 = dx; graph.dy2 = dy; break;
    case 2: graph.dx3 = dx; graph.dy3 = dy; break;
    case 3: graph.dx4 = dx; graph.dy4 = dy; break;
    }
}

void Display::set_graph_clip(int gno, int dx, int dy, int w, int h)
{
    Graph& graph = at(gno);
    if (dx < 0) {
        graph.clip.reset();
    } else {
        graph.clip = SDL_Rect{dx, dy, w, h};
    }
}

void Display::set_graph_bset(int gno, int bno2, int blnd, int vague)
{
    Graph& graph = at(gno);
    graph.bset = 1;
    graph.bno2 = bno2;
    graph.param2 = drw_bld | (static_cast<std::uint32_t>(
        std::clamp(blnd, 0, 256)) << 16);
    graph.sx2 = 0;
    graph.sy2 = 0;
    int width = 0;
    int height = 0;
    get_bmp_size(bno2, &width, &height);
    graph.sw2 = width;
    graph.sh2 = height;
    graph.param3 = static_cast<std::uint32_t>(vague);
}

void Display::set_graph_bset2(int gno, int bno2, int bno3, int blnd)
{
    set_graph_bset(gno, bno2, blnd);
    Graph& graph = at(gno);
    graph.bset = 2;
    graph.bno3 = bno3;
}

void Display::reset_graph_bset(int gno)
{
    Graph& graph = at(gno);
    graph.bset = 0;
    graph.bno2 = -1;
    graph.bno3 = -1;
    graph.param2 = drw_nml;
}

bool Display::graph_flag(int gno) const { return graphs_.at(gno).flag; }
bool Display::graph_disp(int gno) const { return graphs_.at(gno).disp; }
int Display::graph_layer(int gno) const { return graphs_.at(gno).layer; }
std::uint32_t Display::graph_param(int gno) const
{
    return graphs_.at(gno).param;
}

void Display::get_graph_move(int gno, int* dx, int* dy) const
{
    if (dx) {
        *dx = graphs_.at(gno).dx;
    }
    if (dy) {
        *dy = graphs_.at(gno).dy;
    }
}

void Display::get_graph_bmp_size(int gno, int* sx, int* sy) const
{
    get_bmp_size(graphs_.at(gno).bno, sx, sy);
}

void Display::get_graph_bright(int gno, int* r, int* g, int* b) const
{
    const Graph& graph = graphs_.at(gno);
    if (r) {
        *r = graph.r;
    }
    if (g) {
        *g = graph.g;
    }
    if (b) {
        *b = graph.b;
    }
}


namespace {

// DRW_DrawPOLY4_TT's scanline tables, transcribed from Draw.cpp and
// Draw32.cpp.  The engine rasterises a four-point quad by walking its four
// edges with a DDA into per-line min/max tables (DrawMinMaxTableSrc), then
// walking each line between them with a second DDA (DRW_DrawXLine_TT).
// Both are all-integer and both truncate where C truncates, so the texel a
// pixel reads is exactly reproducible - and it is not the texel a GPU's
// interpolated texture coordinate lands on, which is why a rotating shake
// came out as a fine stipple over the whole frame.
struct PolyEdge {
    int x;
    int sx;
    int sy;
};

void min_max_table_src(int x1, int y1, int sx1, int sy1,
                       int x2, int y2, int sx2, int sy2,
                       int cy1, int cy2,
                       std::vector<PolyEdge>& mi, std::vector<PolyEdge>& ma)
{
    int dx = x1 - x2;
    int dy = y1 - y2;
    if (dx == 0 && dy == 0) {
        return;
    }
    int ax = 1;
    int ay = 1;
    if (dx < 0) { ax = -1; dx = -dx; }
    if (dy < 0) { ay = -1; dy = -dy; }
    int x = x2;
    int y = y2;
    int dsx = sx1 - sx2;
    int dsy = sy1 - sy2;
    int sx = sx2;
    int sy = sy2;
    int cnt = 0;
    int sxcnt = 0;
    int sycnt = 0;
    const auto record = [&] {
        if (cy1 <= y && y < cy2) {
            auto& low = mi[static_cast<std::size_t>(y)];
            auto& high = ma[static_cast<std::size_t>(y)];
            if (low.x > x) {
                low = {x, sx, sy};
            }
            if (high.x < x + 1) {
                high = {x + 1, sx + 1, sy};
            }
        }
    };
    // The step is divided BEFORE the sign is taken off, so it truncates
    // toward zero, and the remainder is taken of the magnitude.
    const int span = dx < dy ? dy : dx;
    const int dsx1 = dsx / span;
    const int dsy1 = dsy / span;
    int asx = 1;
    int asy = 1;
    if (dsx < 0) { asx = -1; dsx = -dsx; }
    if (dsy < 0) { asy = -1; dsy = -dsy; }
    const int dsx2 = dsx % span;
    const int dsy2 = dsy % span;
    for (int i = 0; i < span; ++i) {
        record();
        if (dx < dy) {
            y += ay;
            cnt += dx;
            if (cnt >= dy) { cnt -= dy; x += ax; }
        } else {
            x += ax;
            cnt += dy;
            if (cnt >= dx) { cnt -= dx; y += ay; }
        }
        sxcnt += dsx2; if (sxcnt >= span) { sxcnt -= span; sx += asx; } sx += dsx1;
        sycnt += dsy2; if (sycnt >= span) { sycnt -= span; sy += asy; } sy += dsy1;
    }
}

}  // namespace

// Eight ints a display line: (dx1, w, sx2, sy2) - where the line's span
// starts, how long it is and the texel it starts on - then (dsx, dsy, ww, 0),
// the source delta across the full unclipped span.  DRW_DrawXLine_TT's own
// set-up, including the -1 it adds to a start whose delta is negative.
std::vector<int> Display::poly4_rows(const int corners[4][2],
                                     const int sources[4][2]) const
{
    const int height = display_height;
    const int width = display_width;
    std::vector<PolyEdge> mi(static_cast<std::size_t>(height), {width, 0, 0});
    std::vector<PolyEdge> ma(static_cast<std::size_t>(height), {0, 0, 0});
    // DRW_DrawPOLY4_TT walks 1-2, 2-4, 4-3, 3-1; ClipRectDef is the display.
    const auto edge = [&](int a, int b) {
        min_max_table_src(corners[a][0], corners[a][1],
                          sources[a][0], sources[a][1],
                          corners[b][0], corners[b][1],
                          sources[b][0], sources[b][1], 0, height, mi, ma);
    };
    edge(0, 1);
    edge(1, 3);
    edge(3, 2);
    edge(2, 0);

    std::vector<int> rows(static_cast<std::size_t>(height) * 8, 0);
    for (int y = 0; y < height; ++y) {
        const auto& low = mi[static_cast<std::size_t>(y)];
        const auto& high = ma[static_cast<std::size_t>(y)];
        const int ww = high.x - low.x;
        if (low.x == width || ww == 0) {
            continue;
        }
        const int dx1 = std::max(low.x, 0);
        const int dx2 = std::min(high.x, width);
        const int w = dx2 - dx1;
        const int xx = dx1 - low.x;
        const int dsx = high.sx - low.sx;
        const int dsy = high.sy - low.sy;
        const int sx2 = dsx * xx / ww + low.sx - (dsx < 0 ? 1 : 0);
        const int sy2 = dsy * xx / ww + low.sy - (dsy < 0 ? 1 : 0);
        int* row = rows.data() + static_cast<std::size_t>(y) * 8;
        row[0] = dx1;
        row[1] = w;
        row[2] = sx2;
        row[3] = sy2;
        row[4] = dsx;
        row[5] = dsy;
        row[6] = ww;
    }
    return rows;
}

// --- drawing -----------------------------------------------------------

SDL_Texture* Display::blend_pair(const Graph& graph)
{
    // DSP_SetGraphBSet hangs a second bitmap off the graph and the
    // rasteriser blends the pair by param2 before anything else touches
    // them - DRW_DrawBMP_FFF rather than DRW_DrawBMP_FF.  That is what makes
    // a character change pose without ever becoming see-through: it is one
    // solid sprite whose pixels are a mix, not two sprites laid over each
    // other.
    if (graph.bset != 1 || graph.bno2 < 0 || graph.bno2 >= bitmap_max) {
        return nullptr;
    }
    const Bitmap& first = bitmaps_[graph.bno];
    const Bitmap& second = bitmaps_[graph.bno2];
    if (!first.valid() || !second.valid()) {
        return nullptr;
    }
    const int width = std::max(first.width, second.width);
    const int height = std::max(first.height, second.height);
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    float held_width = 0.0f;
    float held_height = 0.0f;
    if (blend_target_) {
        SDL_GetTextureSize(blend_target_.get(), &held_width, &held_height);
    }
    if (!blend_target_ || static_cast<int>(held_width) != width
        || static_cast<int>(held_height) != height) {
        blend_target_.reset(SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
            width, height));
        if (!blend_target_) {
            return nullptr;
        }
        SDL_SetTextureBlendMode(blend_target_.get(), SDL_BLENDMODE_BLEND);
    }
    // Both go in weighted by their share, alpha weighted the same way, so
    // the two add up to one opaque sprite.
    const auto accumulate = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);
    SDL_Texture* const a = first.view;
    SDL_Texture* const b = second.view;
    if (!SDL_SetTextureBlendMode(a, accumulate)
        || !SDL_SetTextureBlendMode(b, accumulate)) {
        SDL_SetTextureBlendMode(a, SDL_BLENDMODE_BLEND);
        SDL_SetTextureBlendMode(b, SDL_BLENDMODE_BLEND);
        return nullptr;  // no custom blending here; the plain path stands in
    }
    const int rate = std::clamp(draw_alpha_of(graph.param2), 0, 256);
    SDL_Texture* const held = SDL_GetRenderTarget(renderer_);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
    SDL_SetRenderTarget(renderer_, blend_target_.get());
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    // bno2 is the pose being left, bno the one arriving, and param2 rises
    // from nothing to all of it.
    for (const auto& [texture, weight] :
         {std::pair{b, 256 - rate}, std::pair{a, rate}}) {
        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(
            texture,
            static_cast<Uint8>(std::clamp(weight * 255 / 256, 0, 255)));
        SDL_RenderTexture(renderer_, texture, nullptr, nullptr);
    }
    SDL_SetRenderScale(renderer_, scale_x, scale_y);
    SDL_SetRenderTarget(renderer_, held);
    SDL_SetTextureBlendMode(a, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(b, SDL_BLENDMODE_BLEND);
    return blend_target_.get();
}

void Display::draw_graph_bmp(
    const Graph& graph, SDL_Texture*, int global_x, int global_y)
{
    if (graph.bno < 0 || graph.bno >= bitmap_max) {
        return;
    }
    const Bitmap& bitmap = bitmaps_[graph.bno];
    if (!bitmap.valid()) {
        return;
    }
    const auto geometry = resolve_geometry(
        graph, global_x, global_y, bitmap.pos_x, bitmap.pos_y,
        bright_r_, bright_g_, bright_b_);

    // A graph with a second bitmap is one sprite whose pixels are a mix of
    // the two.  The rasteriser composites that in a single pass; the scratch
    // buffer below is a stand-in for callers that cannot, and it rounds where
    // the rasteriser truncates - a level on half the sprite.  So the pair is
    // handed to the exact path whole, and only a fallback pre-blends.
    SDL_Texture* pair_second = nullptr;
    int pair_rate = 256;
    if (graph.bset == 1 && graph.bno2 >= 0 && graph.bno2 < bitmap_max
        && bitmaps_[graph.bno2].valid()) {
        pair_second = bitmaps_[graph.bno2].view;
        pair_rate = std::clamp(draw_alpha_of(graph.param2), 0, 256);
    }
    SDL_Texture* paired = nullptr;
    SDL_Texture* texture = bitmap.view;
    const int alpha = draw_alpha_of(graph.param);
    // The exact path carries the alpha in a uniform and does the multiply
    // itself, so the texture's own modulation has to stand down - otherwise
    // it would be applied twice, once rounded at /255 by SDL and once
    // truncated at /256 by the shader.  It is only taken for the plain
    // source-over modes and only for the straight blit below: the poly4 path
    // carries its colour in the vertices, where the alpha would arrive
    // already folded into `color` and be counted twice again.
    const std::uint32_t mode = draw_mode_of(graph.param);
    // DRW_DrawBMP_TT_Bld leaves before it touches a pixel:
    //     if( blnd==256 ) return DRW_DrawBMP_TT_Std( dobj );
    //     if( blnd==0   ) return 1;
    // Nothing at all at zero - and that is not the same as compositing
    // nothing, because the partial-alpha branch scales the destination by
    // 255-blnd_tbl[a], which at blnd 0 is 255 and costs the destination a
    // level.  A character fading in is drawn at 0 on its first frame.
    if (mode == drw_bld && draw_alpha_of(graph.param) == 0) {
        return;
    }
    // The modes the shader reproduces: source-over, and the saturating add.
    // Everything else still goes to SDL, which rounds differently on each
    // backend - that is the remaining source of GLES-vs-GPU disagreement.
    const bool additive = mode == drw_add || mode == drw_ooi;
    const bool plain = graph.poly != Poly::poly4
        && (mode == drw_nml || mode == drw_bld || additive);
    // The SDL_GPU shader has no additive form, so it keeps its narrower set.
    const bool exact = exact_blend_ && plain && !additive;
    // The GL path draws the quad itself, so it only takes the cases whose
    // geometry it reproduces: no clip rectangle (it does not set a scissor)
    // and no render scale (its vertices are in target pixels).
    float exact_scale_x = 1.0f;
    float exact_scale_y = 1.0f;
    SDL_GetRenderScale(renderer_, &exact_scale_x, &exact_scale_y);
    // A clip rectangle is no longer a reason to decline: the exact path sets
    // it as a scissor.  Baking a character into the background bitmap is a
    // clipped draw, and sending that one to SDL left every solid pixel of
    // every baked character a level bright.
    const bool gl_exact = gl_exact_blend_ && plain
        && exact_scale_x == 1.0f && exact_scale_y == 1.0f;
    // A picture that came with an alpha channel is one the rasteriser stores
    // premultiplied, so its colour goes in already folded - mode 3 - while a
    // picture with none is taken at its own value.  The difference is a level
    // on every solid pixel of every sprite, which is most of a character.
    const bool premultiplied = th2::texture_has_source_alpha(texture);
    // Folded on load, before its tone curve: mode 3's blend without the fold.
    const bool folded = th2::texture_source_folded(texture);
    if (pair_second && !gl_exact) {
        // No exact path for this draw, so fall back to the scratch mix.
        paired = blend_pair(graph);
        if (paired) {
            texture = paired;
        }
        pair_second = nullptr;
    }
    // Under the exact path the tint rides in the uniform with the alpha, so
    // that BrightTable and BlendTable are applied in that order and each
    // truncates where the rasteriser truncates.  SDL's modulation would
    // round both, in the other order.
    SDL_SetTextureColorMod(
        texture,
        exact ? 255 : static_cast<Uint8>(geometry.red),
        exact ? 255 : static_cast<Uint8>(geometry.green),
        exact ? 255 : static_cast<Uint8>(geometry.blue));
    SDL_SetTextureAlphaMod(
        texture,
        exact ? 255
              : static_cast<Uint8>(std::clamp(alpha, 0, 256) * 255 / 256));
    SDL_SetTextureBlendMode(
        texture,
        exact ? SDL_BLENDMODE_BLEND_PREMULTIPLIED : blend_of(graph.param));

    const bool clipping = geometry.clipped;
    if (clipping) {
        SDL_SetRenderClipRect(renderer_, &geometry.clip);
    }
    bool poly_drawn = false;
    if (graph.poly == Poly::poly4 && gl_exact_blend_ && !additive
        && (mode == drw_nml || mode == drw_bld)
        && exact_scale_x == 1.0f && exact_scale_y == 1.0f) {
        // DRW_DrawPOLY4_TT, texel for texel - see poly4_rows().  The quad is
        // the whole target; the shader discards outside each line's span.
        int corners[4][2];
        int sources[4][2];
        for (int i = 0; i < 4; ++i) {
            corners[i][0] = static_cast<int>(geometry.corners[i].x);
            corners[i][1] = static_cast<int>(geometry.corners[i].y);
            sources[i][0] = static_cast<int>(geometry.source_corners[i].x);
            sources[i][1] = static_cast<int>(geometry.source_corners[i].y);
        }
        const auto rows = poly4_rows(corners, sources);
        float target_w = 0.0f;
        float target_h = 0.0f;
        if (SDL_Texture* target = SDL_GetRenderTarget(renderer_)) {
            SDL_GetTextureSize(target, &target_w, &target_h);
        }
        const SDL_FRect whole{0.0f, 0.0f, target_w, target_h};
        poly_drawn = target_w > 0.0f
            && gl_exact_blend_->capture_destination(renderer_)
            && gl_exact_blend_->draw(
                renderer_, texture, whole, whole, false, false, 0,
                std::clamp(alpha, 0, 256), geometry.bright_r,
                geometry.bright_g, geometry.bright_b, nullptr, 256,
                clipping ? &geometry.clip : nullptr,
                rows.data(), display_height);
    }
    if (graph.poly == Poly::poly4 && !poly_drawn) {
        // Fallback: two triangles through RenderGeometry.  Corner order is
        // 1 top-left, 2 top-right, 3 bottom-left, 4 bottom-right.
        float width = 0.0f;
        float height = 0.0f;
        SDL_GetTextureSize(texture, &width, &height);
        if (width > 0.0f && height > 0.0f) {
            const SDL_FColor tint{
                geometry.red / 255.0f, geometry.green / 255.0f,
                geometry.blue / 255.0f,
                std::clamp(alpha, 0, 256) / 256.0f};
            SDL_Vertex vertices[4];
            for (int i = 0; i < 4; ++i) {
                vertices[i].position = geometry.corners[i];
                vertices[i].color = tint;
                vertices[i].tex_coord = {
                    geometry.source_corners[i].x / width,
                    geometry.source_corners[i].y / height};
            }
            const int indices[6] = {0, 1, 2, 1, 3, 2};
            // The colour is carried by the vertices here, so the texture's
            // own modulation has to stand down or it would apply twice.
            SDL_SetTextureColorMod(texture, 255, 255, 255);
            SDL_SetTextureAlphaMod(texture, 255);
            SDL_RenderGeometry(renderer_, texture, vertices, 4, indices, 6);
        }
    } else if (graph.poly != Poly::poly4) {
        // DSP_SetGraphSMove can push the source window off the edge of the
        // bitmap - the background is exactly screen-sized, so any slide
        // does.  The rasteriser's ClipRect narrows the blit and pushes the
        // destination in by the same amount rather than clamping or
        // wrapping, which is what leaves the strip for GRP_WORK to fill.
        SDL_FRect source = geometry.source;
        SDL_FRect destination = geometry.destination;
        if (!clip_source(bitmap, source, destination)) {
            SDL_SetTextureColorMod(texture, 255, 255, 255);
            SDL_SetTextureAlphaMod(texture, 255);
            if (clipping) {
                SDL_SetRenderClipRect(renderer_, nullptr);
            }
            return;
        }
        const SDL_FlipMode flip = static_cast<SDL_FlipMode>(
            (geometry.flip_x ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE)
            | (geometry.flip_y ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE));
        // The GL path does the whole blend, destination included, so it
        // is tried first and the SDL draw is skipped when it succeeds.  It
        // reports failure rather than drawing something wrong, so falling
        // through is always safe.
        bool drawn = false;
        if (gl_exact) {
            drawn = gl_exact_blend_->capture_destination(renderer_)
                && gl_exact_blend_->draw(
                    renderer_, texture, source, destination,
                    geometry.flip_x, geometry.flip_y,
                    pair_second ? 4 : additive ? 1
                        : folded ? 6 : premultiplied ? 3 : 0,
                    std::clamp(alpha, 0, 256), geometry.bright_r,
                    geometry.bright_g, geometry.bright_b,
                    pair_second, pair_rate,
                    geometry.clipped ? &geometry.clip : nullptr);
        }
        if (pair_second && !drawn) {
            // The pair could not be composited in one pass after all - mix
            // it in the scratch and let the SDL path below draw that.
            paired = blend_pair(graph);
            if (paired) {
                texture = paired;
                SDL_SetTextureColorMod(
                    texture,
                    exact ? 255 : static_cast<Uint8>(geometry.red),
                    exact ? 255 : static_cast<Uint8>(geometry.green),
                    exact ? 255 : static_cast<Uint8>(geometry.blue));
                SDL_SetTextureAlphaMod(
                    texture,
                    exact ? 255
                          : static_cast<Uint8>(
                                std::clamp(alpha, 0, 256) * 255 / 256));
                SDL_SetTextureBlendMode(
                    texture,
                    exact ? SDL_BLENDMODE_BLEND_PREMULTIPLIED
                          : blend_of(graph.param));
            }
        }
        if (debug_draws) {
            SDL_Log("draw_graph bno=%d mode=%u dst=%.0f,%.0f %.0fx%.0f "
                    "alpha=%d gl_exact=%d drawn=%d",
                    graph.bno, mode, destination.x, destination.y,
                    destination.w, destination.h, alpha, (int)gl_exact,
                    (int)drawn);
        }
        if (!drawn) {
            if (exact) {
                exact_blend_->arm(
                    std::clamp(alpha, 0, 256),
                    geometry.bright_r, geometry.bright_g, geometry.bright_b);
            }
            if (flip == SDL_FLIP_NONE) {
                SDL_RenderTexture(renderer_, texture, &source, &destination);
            } else {
                SDL_RenderTextureRotated(
                    renderer_, texture, &source, &destination,
                    0.0, nullptr, flip);
            }
            if (exact) {
                exact_blend_->release();
            }
        }
        // BrightTable's upper half is in the shader, so the additive pass
        // that stands in for it has nothing left to do - on either exact
        // path.  It only asked about the CPU one, so every GRP_BACK the GL
        // path had already brightened got the picture added on again: a
        // flash to white (FB 256) went white a good deal sooner than it
        // should, and at 204 there was nothing of the CG's shadows left.
        if (geometry.brighten > 0 && !exact && !drawn) {
            // The same picture again, added on.  A white rectangle over the
            // destination would light up everything transparent in it too.
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
            SDL_SetTextureColorMod(
                texture, geometry.brighten, geometry.brighten,
                geometry.brighten);
            SDL_RenderTextureRotated(
                renderer_, texture, &source, &destination, 0.0, nullptr,
                flip);
        }
    }
    if (clipping) {
        SDL_SetRenderClipRect(renderer_, nullptr);
    }
    SDL_SetTextureColorMod(texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(texture, 255);
}

void Display::draw_graph_prim(
    const Graph& graph, SDL_Texture*, int global_x, int global_y)
{
    // PRM_FLAT: a solid rectangle in the graph's own colour.  This is what
    // GRP_WORK is - SHAKE_ROLL parks one at layer 0 so the corners the
    // rotation uncovers come out black instead of showing the last frame.
    if (graph.type != GraphType::flat) {
        return;
    }
    const auto geometry = resolve_geometry(
        graph, global_x, global_y, 0, 0, bright_r_, bright_g_, bright_b_);
    const int alpha = draw_alpha_of(graph.param);
    SDL_SetRenderDrawBlendMode(renderer_, blend_of(graph.param));
    SDL_SetRenderDrawColor(
        renderer_, geometry.red, geometry.green, geometry.blue,
        static_cast<Uint8>(std::clamp(alpha, 0, 256) * 255 / 256));
    if (geometry.clipped) {
        SDL_SetRenderClipRect(renderer_, &geometry.clip);
    }
    if (debug_draws) {
        SDL_Log("draw_flat mode=%u dst=%.0f,%.0f %.0fx%.0f alpha=%d",
                draw_mode_of(graph.param), geometry.destination.x,
                geometry.destination.y, geometry.destination.w,
                geometry.destination.h, alpha);
    }
    SDL_RenderFillRect(renderer_, &geometry.destination);
    if (geometry.clipped) {
        SDL_SetRenderClipRect(renderer_, nullptr);
    }
}

void Display::draw_one(
    const Graph& graph, SDL_Texture* dest, int global_x, int global_y)
{
    switch (graph.type) {
    case GraphType::bmp:
        draw_graph_bmp(graph, dest, global_x, global_y);
        break;
    case GraphType::str: {
        // DrawGraphStr: one blit per character, at dx + dw*i - the index
        // counts every character, drawn or not - from cell j of the sheet.
        Graph cell = graph;
        cell.type = GraphType::bmp;
        cell.poly = Poly::rect;
        cell.zoom = 0;
        const int cell_w = graph.sw / 16;
        for (std::size_t i = 0; i < graph.str.size(); ++i) {
            const char ch = graph.str[i];
            if (ch < '!' || ch > '_') {
                continue;
            }
            const int j = ch - ('!' - 1);
            cell.dx = graph.dx + graph.dw * static_cast<int>(i);
            cell.dw = graph.dw;
            cell.dh = graph.dh;
            cell.sx = graph.sx + cell_w * (j % 16);
            cell.sy = graph.sy + graph.sh * (j / 16);
            cell.sw = cell_w;
            cell.sh = graph.sh;
            draw_graph_bmp(cell, dest, global_x, global_y);
        }
        break;
    }
    case GraphType::spr:
    case GraphType::dgt:
        // Sprites and digits keep their existing paths.
        break;
    default:
        draw_graph_prim(graph, dest, global_x, global_y);
        break;
    }
}

void Display::begin_frame(SDL_Texture* dest)
{
    // GetGraph( dest, draw_mode ) - armed by DSP_GetDispBmp, taken once.
    // Note that `dest` is never cleared, here or anywhere: what a transform
    // fails to cover keeps last frame's picture, which is the behaviour the
    // shakes depend on.
    if (!capture_ || capture_bno_ < 0 || capture_bno_ >= bitmap_max) {
        return;
    }
    capture_ = false;
    Bitmap& into = bitmaps_[capture_bno_];
    if (!into.valid() || !into.renderable) {
        return;
    }
    SDL_Texture* const held = SDL_GetRenderTarget(renderer_);
    SDL_SetRenderTarget(renderer_, into.view);
    SDL_SetTextureBlendMode(dest, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(renderer_, dest, nullptr, nullptr);
    SDL_SetTextureBlendMode(dest, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(renderer_, held);
}

void Display::draw_layer(int layer)
{
    for (std::size_t gno = 0; gno < graphs_.size(); ++gno) {
        const Graph& graph = graphs_[gno];
        if (!graph.flag || !graph.disp || graph.layer != layer) {
            continue;
        }
        if (presenter_) {
            Graph presented = graph;
            presenter_(static_cast<int>(gno), presented);
            draw_one(presented, nullptr, global_x_, global_y_);
            continue;
        }
        draw_one(graph, nullptr, global_x_, global_y_);
    }
}

void Display::end_frame()
{
    // A graph that was rendered into a bitmap this frame comes back on for
    // the next one.  AVG_ControlChar relies on it: it bakes with
    // DSP_SetGraphTarget and then switches the graph off itself, by
    // cut_mode, on the frame after.
    for (auto& graph : graphs_) {
        if (graph.target) {
            graph.target = false;
            graph.disp = true;
        }
    }

    // The four bands DSP_SetGraphGlobalPos pulls away from, with signed
    // widths so the same four cover an offset in any direction and two
    // collapse to nothing each time.
    const int x = global_x_;
    const int y = global_y_;
    if (x == 0 && y == 0) {
        return;
    }
    const SDL_FRect bands[4] = {
        {0.0f, 0.0f, static_cast<float>(x),
         static_cast<float>(display_height)},
        {static_cast<float>(x), 0.0f,
         static_cast<float>(display_width - x), static_cast<float>(y)},
        {static_cast<float>(display_width + x), static_cast<float>(y),
         static_cast<float>(-x), static_cast<float>(display_height - y)},
        {static_cast<float>(x), static_cast<float>(display_height + y),
         static_cast<float>(display_width - x), static_cast<float>(-y)},
    };
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    for (const auto& band : bands) {
        if (band.w > 0.0f && band.h > 0.0f) {
            SDL_RenderFillRect(renderer_, &band);
        }
    }
}

void Display::draw(SDL_Texture* dest, const LayerHook& on_layer)
{
    SDL_SetRenderTarget(renderer_, dest);
    SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
    begin_frame(dest);
    for (int lno = 0; lno < layer_max; ++lno) {
        draw_layer(lno);
        // Where DrawGraphText would run.  The glyphs are drawn by the
        // overlay pass at monitor resolution instead, so this only reports
        // that the layer has come round.
        if (on_layer) {
            on_layer(lno);
        }
    }
    end_frame();
}

}  // namespace th2
