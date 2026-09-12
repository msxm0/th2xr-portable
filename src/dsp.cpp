#include "dsp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace th2 {
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
    bitmap.owned.reset(SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
        width, height));
    bitmap.view = bitmap.owned.get();
    bitmap.width = width;
    bitmap.height = height;
    bitmap.pos_x = 0;
    bitmap.pos_y = 0;
    bitmap.renderable = bitmap.view != nullptr;
    if (bitmap.view) {
        SDL_SetTextureBlendMode(bitmap.view, SDL_BLENDMODE_BLEND);
    }
}

void Display::release_bmp(int bno)
{
    if (bno < 0 || bno >= bitmap_max) {
        return;
    }
    bitmaps_[bno] = Bitmap{};
}

void Display::release_bmp_all()
{
    for (auto& bitmap : bitmaps_) {
        bitmap = Bitmap{};
    }
}

void Display::set_bmp(int bno, Texture texture, int width, int height)
{
    if (bno < 0 || bno >= bitmap_max) {
        return;
    }
    auto& bitmap = bitmaps_[bno];
    bitmap.owned = std::move(texture);
    bitmap.view = bitmap.owned.get();
    bitmap.width = width;
    bitmap.height = height;
    bitmap.pos_x = 0;
    bitmap.pos_y = 0;
    bitmap.renderable = false;
}

void Display::borrow_bmp(
    int bno, SDL_Texture* texture, int width, int height, bool renderable)
{
    if (bno < 0 || bno >= bitmap_max) {
        return;
    }
    auto& bitmap = bitmaps_[bno];
    bitmap.owned.reset();
    bitmap.view = texture;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.pos_x = 0;
    bitmap.pos_y = 0;
    bitmap.renderable = renderable && texture != nullptr;
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
    Texture promoted{SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
        bitmap.width, bitmap.height)};
    if (!promoted) {
        return false;
    }
    SDL_SetTextureBlendMode(promoted.get(), SDL_BLENDMODE_BLEND);
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
    const auto shade = [](int value) {
        return static_cast<Uint8>(
            std::clamp(value * 255 / bright_neutral, 0, 255));
    };
    SDL_SetTextureColorMod(
        source.view, shade(r), shade(g), shade(b));
    SDL_SetTextureBlendMode(source.view, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(renderer_, source.view, nullptr, nullptr);
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
    Graph& graph = at(gno);
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

// --- drawing -----------------------------------------------------------

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

    SDL_Texture* const texture = bitmap.view;
    SDL_SetTextureColorMod(
        texture, geometry.red, geometry.green, geometry.blue);
    const int alpha = draw_alpha_of(graph.param);
    SDL_SetTextureAlphaMod(
        texture,
        static_cast<Uint8>(std::clamp(alpha, 0, 256) * 255 / 256));
    SDL_SetTextureBlendMode(texture, blend_of(graph.param));

    const bool clipping = geometry.clipped;
    if (clipping) {
        SDL_SetRenderClipRect(renderer_, &geometry.clip);
    }
    if (graph.poly == Poly::poly4) {
        // A four-point quad: two triangles through RenderGeometry, which is
        // what the rasteriser's DRW_DrawPOLY4 does by hand.  Corner order is
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
    } else {
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
        if (flip == SDL_FLIP_NONE) {
            SDL_RenderTexture(renderer_, texture, &source, &destination);
        } else {
            SDL_RenderTextureRotated(
                renderer_, texture, &source, &destination,
                0.0, nullptr, flip);
        }
        if (geometry.brighten > 0) {
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
    case GraphType::spr:
    case GraphType::dgt:
    case GraphType::str:
        // Sprites, digits and strings keep their existing paths.
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
    for (const auto& graph : graphs_) {
        if (!graph.flag || !graph.disp || graph.layer != layer) {
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
