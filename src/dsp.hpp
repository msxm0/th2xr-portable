#pragma once

// A transcription of the original engine's display layer (my_inc2/DISP.CPP,
// DISP.H) onto SDL.  The game code drives the screen entirely through this:
// it never draws, it sets fields on a numbered graph and lets DSP_DrawGraph
// walk the layers once a frame.  Reproducing that structure - rather than
// working out what each AVG_ function ought to look like on screen - is the
// point, because every rendering bug so far has come from re-deriving a
// behaviour instead of copying it.
//
// What is deliberately *not* transcribed:
//
//   - The rasteriser (Draw24.cpp and friends, 23k lines of CPU blitters).
//     SDL draws.  GRP_STRUCT::param, the DRW_* blend selector, maps onto
//     SDL blend modes instead - see blend_of().
//   - TEXT_STRUCT.  The original rasterises text into the 800x600
//     framebuffer at LAY_WINDOW; we draw it at monitor resolution on a
//     separate target so it stays sharp.  draw() reports each layer as it
//     comes round (see LayerHook) so the overlay pass can reproduce the
//     ordering without the glyphs going through here.
//   - Sprites, movies, AVI and Dmoji, which already have their own paths.

#include "texture.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace th2 {

using th2app::Texture;

// DISP_X / DISP_Y.  The scene is authored at this size and magnified at
// present time, so everything in here is in art-target space.
inline constexpr int display_width = 800;
inline constexpr int display_height = 600;

inline constexpr int graph_max = 768;    // GRP_MAX
inline constexpr int bitmap_max = 256;   // BMP_MAX
inline constexpr int layer_max = 64;     // LAYER_MAX

inline constexpr int bright_neutral = 128;  // BRT_NML
inline constexpr int check_none = -1;       // CHK_NO

// PRM_*.  Only bmp and the primitive kinds reach us; spr/dgt/str keep their
// numbers so a transcribed switch still reads the same.
enum class GraphType {
    bmp = 0,    // PRM_BMP
    spr = 1,    // PRM_SPR
    dgt = 2,    // PRM_DGT
    str = 3,    // PRM_STR
    flat = 4,   // PRM_FLAT   a solid rectangle - this is GRP_WORK
    ygra = 5,   // PRM_YGRA   vertical gradient
    gulo = 6,   // PRM_GULO
};

// POL_*.  Selects which of the three draw shapes DrawGraphBmp dispatches to:
// a straight blit, a scaled one, or a four-point quad.
enum class Poly {
    rect = 0,
    zoom = 1,
    poly4 = 2,
    poly3 = 3,
    line = 4,
    box = 5,
    kage = 6,
    rpple = 7,
};

// GRP_STRUCT.  Field names follow the original so a transcribed AVG_
// function can be diffed against it line by line.  The four-point variants
// (dx2..dy4, sx2..sy4) are only read when poly == poly4.
struct Graph {
    bool flag = false;   // slot in use
    bool disp = false;   // drawn this frame
    int layer = 0;
    GraphType type = GraphType::bmp;
    Poly poly = Poly::rect;

    int bno = -1;    // bitmap drawn
    int bset = 0;    // 0 none, 1 blend with bno2, 2 blend masked by bno3
    int bno2 = -1;
    int bno3 = -1;

    // Brightness, 128 = neutral.  Below that it darkens; the original also
    // brightens towards white above it, which a colour modulation cannot do.
    int r = bright_neutral;
    int g = bright_neutral;
    int b = bright_neutral;
    bool brt_flag = false;  // ignore the global brightness

    std::uint32_t param = 0;   // DRW_* blend selector
    std::uint32_t param2 = 0;  // blend of the second bitmap
    std::uint32_t param3 = 0;  // pattern fade rate
    std::uint32_t rparam = 0;  // REV_W / REV_H mirroring

    int nuki = check_none;  // colour key

    int dx = 0, dy = 0;
    int dx2 = 0, dy2 = 0;
    int dx3 = 0, dy3 = 0;
    int dx4 = 0, dy4 = 0;
    int dw = 0, dh = 0;

    int sx = 0, sy = 0;
    int sx2 = 0, sy2 = 0;
    int sx3 = 0, sy3 = 0;
    int sx4 = 0, sy4 = 0;
    int sw = 0, sh = 0;
    int sw2 = 0, sh2 = 0;

    int zoom = 0;  // 256ths above 1.0, about (cx, cy)
    int cx = 0, cy = 0;

    // DSP_SetGraphTarget draws the graph into a bitmap and switches it off;
    // DSP_DrawGraph switches it back on at the end of the frame.
    bool target = false;

    std::optional<SDL_Rect> clip;
};

// One bitmap slot.  The original's BmpSet entries are raw pixel buffers with
// an origin; here they are textures, with `pos` kept because DrawGraphBmp
// subtracts it from every source coordinate.
struct Bitmap {
    Texture texture;
    int width = 0;
    int height = 0;
    int pos_x = 0;
    int pos_y = 0;
    // Whether the texture was created with target access.  A bitmap only
    // needs it once something renders into it (DSP_CopyBmp, SetGraphTarget,
    // GetDispBmp), so it is promoted on demand rather than up front.
    bool renderable = false;

    bool valid() const { return texture != nullptr; }
};

// What DrawGraphBmp works out before handing the blit to the rasteriser:
// the destination, the source, the clip and the colour, with the global
// offset and the zoom already folded in.  Pure, so it can be tested without
// a renderer - which is where the arithmetic that keeps being wrong lives.
struct GraphGeometry {
    Poly poly = Poly::rect;
    SDL_FRect destination{};        // rect and zoom
    SDL_FPoint corners[4]{};        // poly4, in the original's 1,2,3,4 order
    SDL_FRect source{};
    SDL_FPoint source_corners[4]{};
    SDL_Rect clip{};
    bool clipped = false;
    Uint8 red = 255, green = 255, blue = 255;
    bool flip_x = false;
    bool flip_y = false;
};

// DrawGraphBmp's opening arithmetic, split out so it can be checked on its
// own.  `bitmap_x`/`bitmap_y` are BmpSet[bno].pos, subtracted from every
// source coordinate; the globals are DSP_SetGraphGlobalPos / GlobalBright.
GraphGeometry resolve_geometry(
    const Graph& graph, int global_x, int global_y,
    int bitmap_x, int bitmap_y,
    int global_r = bright_neutral,
    int global_g = bright_neutral,
    int global_b = bright_neutral);

// The DRW_* selector packs a mode in the low 16 bits and its parameter in
// the high 16.  These pull the two apart the way the macros build them.
constexpr std::uint32_t draw_mode_of(std::uint32_t param)
{
    return param & 0xffffu;
}
constexpr int draw_param_of(std::uint32_t param)
{
    return static_cast<int>((param >> 16) & 0xffffu);
}

// The modes the game actually reaches, counted across the whole engine:
// DRW_BLD 161, DRW_NML 88, DRW_AMI 19, DRW_LPP 15, DRW_ADD 14, DRW_LCF 12,
// DRW_FLT 10, everything else five or fewer.
enum : std::uint32_t {
    drw_nml = 0x00,
    drw_add = 0x01,
    drw_ooi = 0x02,
    drw_sub = 0x03,
    drw_dim = 0x04,
    drw_mul = 0x05,
    drw_oly = 0x06,
    drw_cml = 0x07,
    drw_cmd = 0x08,
    drw_neg = 0x09,
    drw_tnc = 0x0a,
    drw_bld = 0x0b,   // DRW_BLD2: alpha, parameter 0..256
    drw_viv = 0x0c,
    drw_ami = 0x0d,   // dither mesh
    drw_nis = 0x0e,
    drw_moz = 0x0f,
    drw_bom = 0x10,
    drw_gnm = 0x11,
    drw_flt = 0x12,
};

// DRW_BLD(0..256) is the only one that carries an alpha; everything else
// draws solid.  Returns 256 when the mode has no alpha of its own.
int draw_alpha_of(std::uint32_t param);

// The SDL blend mode standing in for a DRW_ selector.  Modes with no SDL
// equivalent fall back to a plain blend rather than dropping the draw.
SDL_BlendMode blend_of(std::uint32_t param);

// DSP_*.  One instance owns the graph table and the bitmap slots; the game
// holds one and drives it exactly as the original drives its globals.
class Display {
public:
    explicit Display(SDL_Renderer* renderer) : renderer_(renderer) {}

    // Called for each text slot as its layer comes round in draw(), so the
    // overlay pass can reproduce the ordering without the glyphs being
    // rasterised in here.  Argument is the layer number just finished.
    using LayerHook = std::function<void(int layer)>;

    // --- bitmaps -------------------------------------------------------
    void create_bmp(int bno, int width, int height);       // DSP_CreateBmp
    void release_bmp(int bno);                             // DSP_ReleaseBmp
    void release_bmp_all();                                // DSP_ReleaseBmpAll
    // Hands an already-decoded picture to a slot - this stands in for
    // DSP_LoadBmp, which decodes from the archive.  Loading stays where it
    // is; only the residency is modelled here.
    void set_bmp(int bno, Texture texture, int width, int height);
    void copy_bmp(int db_no, int sb_no);                   // DSP_CopyBmp
    // DSP_CopyBmp2: the same copy with a brightness applied, 128 neutral.
    void copy_bmp2(int db_no, int sb_no, int r, int g, int b);
    void get_bmp_size(int bno, int* sx, int* sy) const;    // DSP_GetBmpSize
    bool bmp_flag(int bno) const;                          // DSP_GetBmpFlag
    SDL_Texture* bmp_texture(int bno) const;

    // Arms a capture of the framebuffer into a slot; the next draw() does
    // it, exactly as GetGraph is gated on GetBackFlag.  This is how a
    // transition gets its outgoing picture.
    void get_disp_bmp(int bno);                            // DSP_GetDispBmp

    // --- graphs --------------------------------------------------------
    void set_graph(int gno, int bno, int lno, bool disp,
                   int nuki = check_none);                 // DSP_SetGraph
    void set_graph_prim(int gno, GraphType type, Poly poly,
                        int lno, bool disp);               // DSP_SetGraphPrim
    void reset_graph(int gno);                             // DSP_ResetGraph
    void reset_graph_all();                                // DSP_ResetGraphAll

    void set_graph_bmp(int gno, int bno);                  // DSP_SetGraphBmp
    void set_graph_disp(int gno, bool disp);               // DSP_SetGraphDisp
    void set_graph_layer(int gno, int lno);                // DSP_SetGraphLayer
    void set_graph_nuki(int gno, int nuki);                // DSP_SetGraphNuki
    void set_graph_param(int gno, std::uint32_t param);    // DSP_SetGraphParam
    void set_graph_param2(int gno, std::uint32_t param);   // DSP_SetGraphParam2
    void set_graph_type(int gno, GraphType type);          // DSP_SetGraphType
    void set_graph_bright_flag(int gno, bool brt_flag);
    void set_graph_rev_param(int gno, std::uint32_t rparam);

    // Renders the graph into a bitmap once, then switches it off.  draw()
    // switches it back on at the end of the frame, which is what lets
    // AVG_ControlChar bake a character and still have its graph available.
    void set_graph_target(int gno, int target);            // DSP_SetGraphTarget

    void set_graph_bright(int gno, int r, int g, int b);   // DSP_SetGraphBright
    void set_graph_fade(int gno, int br);                  // DSP_SetGraphFade
    // Points 1..3 only feed the gradient primitives (PRM_YGRA, PRM_GULO),
    // which nothing in ToHeart2 places, so only point 0 is carried.
    void set_graph_bright_point(int gno, int point, int r, int g, int b);
    void set_graph_global_bright(int r, int g, int b);
    void set_graph_global_pos(int x, int y);               // DSP_SetGraphGlobalPos

    void set_graph_move(int gno, int dx, int dy);          // DSP_SetGraphMove
    void set_graph_move2(int gno, int dx, int dy);         // DSP_SetGraphMove2
    void set_graph_smove(int gno, int sx, int sy);         // DSP_SetGraphSMove
    void set_graph_spos(int gno, int sx, int sy, int sw, int sh,
                        int poly = -1);                    // DSP_SetGraphSPos
    void set_graph_width(int gno, int w, int h);           // DSP_SetGraphWidth
    void set_graph_pos(int gno, int dx, int dy,
                       int sx, int sy, int w, int h);      // DSP_SetGraphPos
    void set_graph_pos_zoom(int gno, int dx, int dy, int dw, int dh,
                            int sx, int sy, int sw, int sh);
    void set_graph_zoom(int gno, int dx, int dy, int dw, int dh);
    void set_graph_zoom2(int gno, int cx, int cy, int zoom);
    void set_graph_zoom3(int gno, int cx, int cy, int zoom);
    void set_graph_roll(int gno, int cx, int cy, int zoom, int rate,
                        int sx, int sy, int sw, int sh);   // DSP_SetGraphRoll
    void set_graph_pos_poly(int gno,
        int dx1, int dy1, int dx2, int dy2,
        int dx3, int dy3, int dx4, int dy4,
        int sx1, int sy1, int sx2, int sy2,
        int sx3, int sy3, int sx4, int sy4);
    void set_graph_pos_rect(int gno, int dx, int dy, int w, int h);
    void set_graph_pos_point(int gno, int point, int dx, int dy);
    void set_graph_clip(int gno, int dx, int dy, int w, int h);

    void set_graph_bset(int gno, int bno2, int blnd, int vague = 128);
    void set_graph_bset2(int gno, int bno2, int bno3, int blnd);
    void reset_graph_bset(int gno);

    bool graph_flag(int gno) const;                        // DSP_GetGraphFlag
    bool graph_disp(int gno) const;                        // DSP_GetGraphDisp
    int graph_layer(int gno) const;                        // DSP_GetGraphLayer
    std::uint32_t graph_param(int gno) const;
    void get_graph_move(int gno, int* dx, int* dy) const;
    void get_graph_bmp_size(int gno, int* sx, int* sy) const;
    void get_graph_bright(int gno, int* r, int* g, int* b) const;

    const Graph& graph(int gno) const { return graphs_.at(gno); }

    // --- the frame -----------------------------------------------------
    // DSP_DrawGraph.  Walks layers 0..layer_max, draws every visible graph
    // on each, then paints the four bands the global offset pulls away
    // from.  `dest` is never cleared - that is the whole reason a shake
    // leaves last frame's picture in the corners it does not cover.
    void draw(SDL_Texture* dest, const LayerHook& on_layer = {});

private:
    Graph& at(int gno) { return graphs_.at(gno); }
    bool ensure_renderable(int bno);
    void draw_graph_bmp(const Graph& graph, SDL_Texture* dest,
                        int global_x, int global_y);
    void draw_graph_prim(const Graph& graph, SDL_Texture* dest,
                         int global_x, int global_y);
    void draw_one(const Graph& graph, SDL_Texture* dest,
                  int global_x, int global_y);

    SDL_Renderer* renderer_ = nullptr;
    std::array<Graph, graph_max> graphs_{};
    std::array<Bitmap, bitmap_max> bitmaps_{};

    int global_x_ = 0;
    int global_y_ = 0;
    int bright_r_ = bright_neutral;
    int bright_g_ = bright_neutral;
    int bright_b_ = bright_neutral;

    int capture_bno_ = -1;   // GetBackNo
    bool capture_ = false;   // GetBackFlag
};

}  // namespace th2
