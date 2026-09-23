#include "gl_blend.hpp"

#include "image.hpp"

// This is the compositing path for every build: GLES is the default renderer
// and this is what draws through it.  With it, the port reproduces the 2002
// engine's framebuffer byte for byte over the whole opening - 6000 ticks,
// state and pixels, no tolerance.
//
// Three coordinate facts, each established by drawing a mark and reading
// back where it went, and each stated here rather than probed for at
// startup: a wrong one is meant to be obvious on screen.
//   - The vertex quad goes into clip space with y NOT inverted.  SDL's
//     target projection already puts its y-down origin at GL's bottom-left.
//     Wrong: the picture is upside down.
//   - The destination lookup is gl_FragCoord.xy / target, no flip, because
//     glCopyTexSubImage2D gives the scratch the framebuffer's orientation.
//     Wrong: blended regions sample from the far end of the screen.
//   - Source channels are read .rgb, as the ABGR8888 the textures declare.
//     Wrong: every frame is red/blue swapped.

// Same build rule as gl_transition: wherever SDL draws through GLES and the
// headers exist.  See gl_blend.hpp for why this exists alongside the SDL_GPU
// version rather than replacing it outright.
#if defined(__EMSCRIPTEN__) || defined(TH2_HAVE_GLES3)
#define TH2_GL_BLEND 1
#include <GLES3/gl3.h>
#endif

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace th2 {

#ifdef TH2_GL_BLEND

namespace {

constexpr char vertex_source[] = R"(#version 300 es
precision highp float;
in vec2 a_position;      // clip space
in vec2 a_uv;            // source texture, normalised
out vec2 v_uv;
void main()
{
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// PRECISION.  `precision highp int` is not decoration: the default integer
// precision in a GLES fragment shader is mediump, which is allowed to be as
// narrow as 16 bits, and the products below reach 255*256 = 65280.  A
// mediump *float* is worse - fp16 is exact on integers only to 2048 and
// overflows to infinity past 65504, so 256*256 would be Inf.  That is the
// shape of bug that works on a desktop and destroys the screen on a phone,
// so it is stated here and measured at startup by self_test().
constexpr char fragment_source[] = R"(#version 300 es
precision highp float;
precision highp int;

uniform sampler2D u_source;
uniform sampler2D u_source2;    // mode 4: the pose being left
uniform vec2 u_source2_scale;   // v_uv is normalised against u_source
uniform sampler2D u_dest;
uniform vec2 u_target;          // render target size, for the dest lookup
// The source rectangle, the destination rectangle and the second source's
// origin, all in texels.  DRW_DrawZOOM_TT walks the source with a Bresenham
// step - px = sw/dw, plus an error term that carries one more texel whenever
// it passes dw - so the texel under destination pixel i is floor(i*sw/dw):
// the one at the pixel's leading EDGE.  A sampler asked for the same stretch
// reads at the pixel's centre, half a texel along, which on a zooming
// background is a different picture wherever the two round apart.  Indexing
// the texel directly is also exact for a 1:1 blit, where it is the identity.
uniform ivec4 u_src_rect;
uniform ivec4 u_dst_rect;
uniform ivec2 u_src2_origin;
uniform ivec2 u_flip;           // 1 when the axis is reversed
// Mode 5: DRW_DrawPOLY4_TT, the rotated/skewed quad.  One row per display
// line, two texels each: (dx1, w, sx2, sy2) and (dsx, dsy, ww, 0), built on
// the CPU exactly as DrawMinMaxTableSrc and DRW_DrawXLine_TT build them.
uniform highp isampler2D u_rows;
uniform int u_row_count;
uniform int u_alpha;            // DRW_BLD parameter, 0..256
uniform int u_bright_r;         // BrightTable index, 0..255, 128 neutral
uniform int u_bright_g;
uniform int u_bright_b;
uniform int u_mode;             // 0: source-over (DRW_NML/BLD), 1: add,
                                // 2: a glyph mask, 3: source-over with the
                                //    texel's own alpha folded in first,
                                // 4: DRW_DrawBMP_TTT_Bld, two sources at once
uniform int u_pair;             // mode 4: DSP_SetGraphBSet's rate, 0..256
uniform int u_folded;           // mode 4: bit 0/1 - source 1/2 is stored
                                // folded already (a toned 32 bit load)
uniform ivec3 u_ink;            // mode 2: the colour the mask is drawn in

in vec2 v_uv;
out vec4 fragment;

// BlendTable[i][j] = LIM((i*j)>>8, 0, 255), i on the 0..256 alpha scale.
int blend_table(int alpha, int value)
{
    return clamp((clamp(alpha, 0, 256) * clamp(value, 0, 255)) >> 8, 0, 255);
}

// BlendTable16[i][j] = i*j/15, indexed by FNT_Draw with the glyph's 4 bit
// coverage and TXT_DrawTextEx's alph2 (0..256).
int blend_table16(int coverage, int alpha)
{
    return clamp(clamp(coverage, 0, 15) * clamp(alpha, 0, 256) / 15, 0, 256);
}

// BMP_CreateTableBright_T:
//     j <  128:  (i*j)>>7
//     j >= 128:  (((255-i)*(j-128))>>7) + i
int bright_of(int value, int level)
{
    int j = clamp(level, 0, 255);
    int v = clamp(value, 0, 255);
    return j < 128 ? (v * j) >> 7
                   : ((((255 - v) * (j - 128)) >> 7) + v);
}

void main()
{
    // Channel order is taken as declared: SDL hands these textures over as
    // ABGR8888, whose bytes are R,G,B,A, so a sample is .rgb.  If a backend
    // ever disagrees the whole screen comes out red/blue swapped, which is
    // the intended failure - obvious in one glance, rather than a startup
    // probe quietly compensating in a direction nobody can see afterwards.
    ivec2 at;
    ivec2 texel_at;
    if (u_mode == 5) {
        // DRW_DrawXLine_TT: pixel k of the row's span reads
        //     start + sign * floor(k * |delta| / ww)
        // - the integer step sx/ww plus a carry each time dsx overflows ww,
        // which is exactly that floor.  No alpha anywhere in these walkers:
        // nuki is compared as a colour key, and -1 keys nothing.
        ivec2 p = ivec2(gl_FragCoord.xy);
        if (p.y < 0 || p.y >= u_row_count) {
            discard;
        }
        ivec4 ra = texelFetch(u_rows, ivec2(0, p.y), 0);
        ivec4 rb = texelFetch(u_rows, ivec2(1, p.y), 0);
        int k = p.x - ra.x;
        if (rb.z <= 0 || k < 0 || k >= ra.y) {
            discard;
        }
        texel_at = ivec2(
            ra.z + (rb.x < 0 ? -1 : 1) * ((k * abs(rb.x)) / rb.z),
            ra.w + (rb.y < 0 ? -1 : 1) * ((k * abs(rb.y)) / rb.z));
        at = texel_at;
    } else {
        ivec2 d = ivec2(gl_FragCoord.xy) - u_dst_rect.xy;
        d = clamp(d, ivec2(0), max(u_dst_rect.zw - 1, ivec2(0)));
        ivec2 step = (d * u_src_rect.zw) / max(u_dst_rect.zw, ivec2(1));
        // A reversed axis walks the source backwards from its far edge,
        // which is where DRW_GetStartPointerSrc1 starts it.
        at = ivec2(
            u_flip.x != 0 ? u_src_rect.z - 1 - step.x : step.x,
            u_flip.y != 0 ? u_src_rect.w - 1 - step.y : step.y);
        texel_at = u_src_rect.xy + at;
    }
    vec4 texel = texelFetch(u_source, texel_at, 0);

    // A glyph is a coverage mask in a solid colour, composited once per
    // pixel exactly as FNT_Draw does it - which is why the glyphs are
    // uploaded as textures rather than plotted point by point: a point draw
    // leaves the destination half of the blend to the blend unit, and the
    // blend unit rounds.  Coverage is stored as 17*c so that it survives an
    // 8 bit channel exactly and divides back out.
    if (u_mode == 2) {
        int coverage = int(round(texel.a * 255.0)) / 17;
        if (coverage == 0) {
            discard;
        }
        vec2 gat = gl_FragCoord.xy / u_target;
        ivec3 gdst = ivec3(round(texture(u_dest, gat).rgb * 255.0));
        ivec3 gout;
        if (u_alpha >= 256) {
            // FNT_DrawTextBuf_Fx2's alph==256 branch: BlendTable16 on both
            // terms, which is a divide by 15 and not by 256.
            //     bld_tbl = BlendTable16[c];  rev_tbl = BlendTable16[15-c];
            //     dest = rev_tbl[dest] + bld_tbl[ink];
            gout = ((15 - coverage) * gdst) / 15 + (coverage * u_ink) / 15;
        } else {
            // The alpha branch of the same function:
            //     eff = BlendTable16[c][alph];
            //     dest = BlendTable[255-eff][dest] + BlendTable[eff][ink];
            // 255-eff, not 256-eff.  That one is the whole of the remaining
            // bias: with 256 the destination keeps a fraction too much and
            // every antialiased glyph pixel came out a level bright.  The
            // engine can use 255 safely because alph==256 never reaches
            // here, so eff is at most 15*255/15 = 255.
            int geff = blend_table16(coverage, u_alpha);
            gout = ivec3(
                blend_table(255 - geff, gdst.r) + blend_table(geff, u_ink.r),
                blend_table(255 - geff, gdst.g) + blend_table(geff, u_ink.g),
                blend_table(255 - geff, gdst.b) + blend_table(geff, u_ink.b));
        }
        fragment = vec4(vec3(clamp(gout, 0, 255)) / 255.0, 1.0);
        return;
    }

    // DRW_DrawBMP_TTT_Bld: a graph with a second bitmap is ONE sprite whose
    // pixels are a mix of the two, composited in a single pass - not two
    // sprites laid over each other, and not a mix made in a scratch buffer
    // first.  Pre-blending it through SDL cost a level on half the sprite,
    // because the rasteriser's two truncating multiplies are what the
    // destination sees, not a rounded sum of them.  This is the DRW_BLD2
    // form, where the pair rate and the graph's own alpha combine:
    //     blnd3 = (blnd*dnum)>>8;  brev3 = (brev*dnum)>>8;
    if (u_mode == 4) {
        vec4 texel2 = texelFetch(u_source2, u_src2_origin + at, 0);
        int a1 = int(round(texel.a * 255.0));
        int a2 = int(round(texel2.a * 255.0));
        if (a1 == 0 && a2 == 0) {
            discard;
        }
        int pair = clamp(u_pair, 0, 256);
        int own = clamp(u_alpha, 0, 256);
        int blnd3 = (pair * own) >> 8;
        int brev3 = ((256 - pair) * own) >> 8;
        // The rasteriser's bitmaps carry their own alpha; ours do not, so it
        // is folded in here exactly as BMP_MakeBMP does - BlendTable[a][c],
        // which at a==255 is still (255*c)>>8.
        ivec3 s1 = ivec3(bright_of(int(round(texel.rgb.r * 255.0)), u_bright_r),
                         bright_of(int(round(texel.rgb.g * 255.0)), u_bright_g),
                         bright_of(int(round(texel.rgb.b * 255.0)), u_bright_b));
        ivec3 s2 = ivec3(bright_of(int(round(texel2.rgb.r * 255.0)), u_bright_r),
                         bright_of(int(round(texel2.rgb.g * 255.0)), u_bright_g),
                         bright_of(int(round(texel2.rgb.b * 255.0)), u_bright_b));
        ivec3 p1 = (u_folded & 1) != 0 ? s1
            : ivec3(blend_table(a1, s1.r), blend_table(a1, s1.g),
                    blend_table(a1, s1.b));
        ivec3 p2 = (u_folded & 2) != 0 ? s2
            : ivec3(blend_table(a2, s2.r), blend_table(a2, s2.g),
                    blend_table(a2, s2.b));
        ivec3 t1 = ivec3(blend_table(blnd3, p1.r), blend_table(blnd3, p1.g),
                         blend_table(blnd3, p1.b));
        ivec3 t2 = ivec3(blend_table(brev3, p2.r), blend_table(brev3, p2.g),
                         blend_table(brev3, p2.b));
        int df;
        ivec3 sum;
        if (a1 != 0 && a2 != 0) {
            if (a1 == 255 && a2 == 255) {
                df = 256 - blnd3 - brev3;
            } else if (a1 == 255) {
                df = 256 - blnd3 - blend_table(brev3, a2);
            } else if (a2 == 255) {
                df = 256 - brev3 - blend_table(blnd3, a1);
            } else {
                df = 256 - blend_table(blnd3, a1) - blend_table(brev3, a2);
            }
            sum = t1 + t2;
        } else if (a1 != 0) {
            df = a1 == 255 ? 256 - blnd3 : 255 - blend_table(blnd3, a1);
            sum = t1;
        } else {
            df = a2 == 255 ? 256 - brev3 : 255 - blend_table(brev3, a2);
            sum = t2;
        }
        vec2 pair_at = gl_FragCoord.xy / u_target;
        ivec3 pdst = ivec3(round(texture(u_dest, pair_at).rgb * 255.0));
        ivec3 pout = ivec3(blend_table(df, pdst.r), blend_table(df, pdst.g),
                           blend_table(df, pdst.b)) + sum;
        fragment = vec4(vec3(clamp(pout, 0, 255)) / 255.0, 1.0);
        return;
    }

    ivec3 raw = ivec3(round(texel.rgb * 255.0));
    // Brightness first, then the alpha - the rasteriser's order, and the two
    // truncations do not commute.
    ivec3 src = ivec3(bright_of(raw.r, u_bright_r),
                      bright_of(raw.g, u_bright_g),
                      bright_of(raw.b, u_bright_b));
    int texel_alpha = u_mode == 5 ? 255 : int(round(texel.a * 255.0));

    // Draw32.cpp splits on the source texel being opaque: that branch uses
    // BlendTable[256 - blnd] against the destination and blnd itself against
    // the source, while the partial branch folds the texel's own alpha in
    // through BlendTable first.
    // A fully transparent source texel is SKIPPED, not blended:
    //     if( src1_p->a != 0 ){ ... }
    // Without this the partial branch runs with eff = 0, whose destination
    // factor is 255 rather than 256, and writes (255*dst)>>8 - one level
    // darker than the destination it was supposed to leave alone.  Every
    // transparent pixel of every sprite was a level dark; the click
    // indicator's 40x40 box showed it as a uniform -1 square.
    if (texel_alpha == 0) {
        discard;
    }

    int blnd = clamp(u_alpha, 0, 256);
    // Draw32.cpp splits on the source texel being fully opaque, and the two
    // branches do NOT use the same destination factor:
    //     a == 255:  brev_tbl  = BlendTable[ 256 - blnd ];
    //     a <  255:  brev_tbl2 = BlendTable[ 255 - blnd_tbl[a] ];
    // 256 in one and 255 in the other.  Collapsing them into 256 left every
    // antialiased edge of a partially transparent sprite a level bright -
    // which is where the last of the difference was, on the sakura petals.
    // Mode 3 is the same blend reading a source that already carries its
    // own alpha - BlendTable[a][c], the shape a premultiplied bitmap has -
    // so the blend level applies to the stored colour and nothing else.  It
    // is not mode 0 with a==255 folded in: at full alpha that fold is still
    // (255*c)>>8, a level short of c, and the system bar's whole strip was
    // one level bright until it was there.  Measured against the reference
    // over the bar's 30x600 strip, all 3600 opaque channel values.  Only the
    // caller that knows its source is stored that way may ask for it; mode 0
    // stays what every ordinary graph uses.
    // Mode 6 is mode 3 on a bitmap that was never premultiplied: a 32 bit
    // TGA that DSP_LoadBmp was asked for as BMP_FULL keeps its alpha (the
    // loader takes the file's depth) but skips the BlendTable[a] fold (the
    // fold is keyed on the depth asked for).  The 32 bit blits then add the
    // raw colour as if it had been folded - and the sum wraps, because the
    // channels are unsigned char.  The map's fields are loaded this way.
    if (u_mode == 3) {
        src = ivec3(blend_table(texel_alpha, src.r),
                    blend_table(texel_alpha, src.g),
                    blend_table(texel_alpha, src.b));
    }
    int eff, rev;
    if (texel_alpha == 255) {
        eff = blnd;
        rev = 256 - blnd;
    } else {
        // The destination factor is the coverage the source did not cover,
        // in both modes.  What the blend level multiplies differs: mode 0
        // has to fold the alpha into it, mode 3's source carries it already.
        rev = 255 - blend_table(blnd, texel_alpha);
        eff = (u_mode == 3 || u_mode == 6) ? blnd
                                           : blend_table(blnd, texel_alpha);
    }

    // No flip: the scratch is filled by glCopyTexSubImage2D straight out of
    // the framebuffer, so it carries the framebuffer's own orientation, and
    // gl_FragCoord is in those same coordinates.
    vec2 dest_at = gl_FragCoord.xy / u_target;
    ivec3 dst = ivec3(round(texture(u_dest, dest_at).rgb * 255.0));

    ivec3 result;
    if (u_mode == 1) {
        // DRW_ADD, Draw32.cpp: dest = AddTable[dest + src], and
        // AddTable[i] = min(i, 255) over 0..511 - a saturating add of the
        // stored source with no alpha scaling of its own.  Our textures keep
        // straight alpha where the rasteriser's are premultiplied, so the
        // texel's own coverage is folded in through BlendTable first; for an
        // opaque texel that is the identity and this is the engine's add.
        ivec3 contribution = ivec3(blend_table(eff, src.r),
                                   blend_table(eff, src.g),
                                   blend_table(eff, src.b));
        result = min(dst + contribution, ivec3(255));
    } else {
        result = ivec3(
            blend_table(rev, dst.r) + blend_table(eff, src.r),
            blend_table(rev, dst.g) + blend_table(eff, src.g),
            blend_table(rev, dst.b) + blend_table(eff, src.b));
        if (u_mode == 6) {
            result = result & ivec3(255);   // unsigned char, not saturated
        }
    }
    fragment = vec4(vec3(clamp(result, 0, 255)) / 255.0, 1.0);
}
)";

GLuint compile(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512]{};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        SDL_Log("exact blend shader failed to compile: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// SDL exposes the GL name under a different property per backend.
GLuint texture_name(SDL_Texture* texture)
{
    if (!texture) {
        return 0;
    }
    const SDL_PropertiesID properties = SDL_GetTextureProperties(texture);
    if (!properties) {
        return 0;
    }
    auto name = static_cast<GLuint>(SDL_GetNumberProperty(
        properties, SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0));
    if (!name) {
        name = static_cast<GLuint>(SDL_GetNumberProperty(
            properties, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0));
    }
    return name;
}

bool renderer_uses_gl(SDL_Renderer* renderer)
{
    const char* name = SDL_GetRendererName(renderer);
    if (!name) {
        return false;
    }
    const std::string_view driver(name);
    return driver == "opengl" || driver == "opengles2"
        || driver == "opengles";
}

struct TextureDelete {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};

}  // namespace

struct GlExactBlend::Impl {
    GLuint program = 0;
    GLuint vertex_buffer = 0;
    GLuint vertex_array = 0;
    GLint source_location = -1;
    GLint source2_location = -1;
    GLint source2_scale_location = -1;
    GLint rows_location = -1;
    GLint row_count_location = -1;
    GLuint rows_name = 0;
    GLint src_rect_location = -1;
    GLint dst_rect_location = -1;
    GLint src2_origin_location = -1;
    GLint flip_location = -1;
    GLint pair_location = -1;
    GLint folded_location = -1;
    GLint dest_location = -1;
    GLint target_location = -1;
    GLint alpha_location = -1;
    GLint bright_locations[3] = {-1, -1, -1};
    GLint mode_location = -1;
    GLint ink_location = -1;
    bool ready = false;

    // A copy of the render target.  Sampling the texture being drawn into is
    // a feedback loop and undefined, so the destination is read from here
    // instead.
    //
    // A raw GL texture rather than an SDL one: SDL's GLES backend does not
    // publish SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER for textures created
    // with SDL_TEXTUREACCESS_TARGET (an ordinary texture gets a name, a
    // target gets 0), so an SDL scratch could never be bound as a sampler.
    // glCopyTexSubImage2D reads the bound framebuffer directly, which is
    // both simpler and one less full render pass than copying through SDL.
    GLuint scratch_name = 0;
    int scratch_width = 0;
    int scratch_height = 0;
    bool captured = false;

    Impl()
    {
        const GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
        const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
        if (!vertex || !fragment) {
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            return;
        }
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glBindAttribLocation(program, 0, "a_position");
        glBindAttribLocation(program, 1, "a_uv");
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512]{};
            glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
            SDL_Log("exact blend shader failed to link: %s", log);
            glDeleteProgram(program);
            program = 0;
            return;
        }

        source_location = glGetUniformLocation(program, "u_source");
        source2_location = glGetUniformLocation(program, "u_source2");
        source2_scale_location =
            glGetUniformLocation(program, "u_source2_scale");
        pair_location = glGetUniformLocation(program, "u_pair");
        folded_location = glGetUniformLocation(program, "u_folded");
        rows_location = glGetUniformLocation(program, "u_rows");
        row_count_location = glGetUniformLocation(program, "u_row_count");
        src_rect_location = glGetUniformLocation(program, "u_src_rect");
        dst_rect_location = glGetUniformLocation(program, "u_dst_rect");
        src2_origin_location = glGetUniformLocation(program, "u_src2_origin");
        flip_location = glGetUniformLocation(program, "u_flip");
        dest_location = glGetUniformLocation(program, "u_dest");
        target_location = glGetUniformLocation(program, "u_target");
        alpha_location = glGetUniformLocation(program, "u_alpha");
        bright_locations[0] = glGetUniformLocation(program, "u_bright_r");
        bright_locations[1] = glGetUniformLocation(program, "u_bright_g");
        bright_locations[2] = glGetUniformLocation(program, "u_bright_b");
        mode_location = glGetUniformLocation(program, "u_mode");
        ink_location = glGetUniformLocation(program, "u_ink");

        // Four vertices, rewritten per draw: position then uv.
        glGenVertexArrays(1, &vertex_array);
        glGenBuffers(1, &vertex_buffer);
        glBindVertexArray(vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 16, nullptr,
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                              sizeof(GLfloat) * 4, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 4,
                              reinterpret_cast<const void*>(
                                  sizeof(GLfloat) * 2));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        ready = glGetError() == GL_NO_ERROR;
    }

    ~Impl()
    {
        if (scratch_name) glDeleteTextures(1, &scratch_name);
        if (rows_name) glDeleteTextures(1, &rows_name);
        if (vertex_array) glDeleteVertexArrays(1, &vertex_array);
        if (vertex_buffer) glDeleteBuffers(1, &vertex_buffer);
        if (program) glDeleteProgram(program);
    }
};

GlExactBlend::GlExactBlend(SDL_Renderer* renderer)
    : impl_(renderer_uses_gl(renderer) ? std::make_unique<Impl>() : nullptr)
{
}

GlExactBlend::~GlExactBlend() = default;

bool GlExactBlend::available() const { return impl_ && impl_->ready; }

bool GlExactBlend::capture_destination(SDL_Renderer* renderer,
                                      const SDL_FRect* region)
{
    if (!available()) {
        return false;
    }
    impl_->captured = false;
    SDL_Texture* const target = SDL_GetRenderTarget(renderer);
    if (!target) {
        return false;           // the window; nothing to sample
    }
    float w = 0.0f;
    float h = 0.0f;
    if (!SDL_GetTextureSize(target, &w, &h) || w <= 0.0f || h <= 0.0f) {
        return false;
    }
    const int width = static_cast<int>(w);
    const int height = static_cast<int>(h);

    // Everything SDL has queued has to reach the driver first: the copy
    // below reads the framebuffer as it stands, and a queued draw that has
    // not landed yet is not in it.
    if (!SDL_FlushRenderer(renderer)) {
        return false;
    }

    if (!impl_->scratch_name || impl_->scratch_width != width
        || impl_->scratch_height != height) {
        if (impl_->scratch_name) {
            glDeleteTextures(1, &impl_->scratch_name);
        }
        glGenTextures(1, &impl_->scratch_name);
        glBindTexture(GL_TEXTURE_2D, impl_->scratch_name);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        impl_->scratch_width = width;
        impl_->scratch_height = height;
    } else {
        glBindTexture(GL_TEXTURE_2D, impl_->scratch_name);
    }
    // Reads whatever framebuffer SDL left bound, which is the render target
    // we are about to draw into.  A region is in SDL's top-left coordinates
    // and the framebuffer counts from the bottom, so the row range flips;
    // the offsets into the scratch are the same numbers, which is what keeps
    // it aligned with gl_FragCoord.
    int cx = 0, cy = 0, cw = width, ch = height;
    if (region) {
        cx = std::clamp(static_cast<int>(region->x), 0, width);
        cw = std::clamp(static_cast<int>(region->w) + 1, 0, width - cx);
        const int top = std::clamp(static_cast<int>(region->y), 0, height);
        const int bottom =
            std::clamp(static_cast<int>(region->y + region->h) + 1, 0, height);
        ch = bottom - top;
        cy = height - bottom;
        if (cw <= 0 || ch <= 0) {
            return false;
        }
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, cx, cy, cx, cy, cw, ch);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        return false;
    }
    impl_->captured = true;
    return true;
}

bool GlExactBlend::draw(
    SDL_Renderer* renderer, SDL_Texture* source,
    const SDL_FRect& src, const SDL_FRect& requested_dst, bool flip_x,
    bool flip_y, int mode, int alpha, int bright_r, int bright_g,
    int bright_b, SDL_Texture* source2, int pair,
    const SDL_Rect* requested_clip, const int* poly_rows, int poly_row_count)
{
    if (poly_rows) {
        mode = 5;
    }
    // SDL's own draws are relative to the viewport, and this one has to land
    // where they would: the text shakes move the message by moving the
    // viewport, and a glyph placed in absolute target pixels stayed put.
    SDL_Rect sdl_viewport{};
    SDL_GetRenderViewport(renderer, &sdl_viewport);
    SDL_FRect dst = requested_dst;
    dst.x += static_cast<float>(sdl_viewport.x);
    dst.y += static_cast<float>(sdl_viewport.y);
    SDL_Rect clip_rect{};
    const SDL_Rect* clip = nullptr;
    if (requested_clip) {
        clip_rect = *requested_clip;
        clip_rect.x += sdl_viewport.x;
        clip_rect.y += sdl_viewport.y;
        clip = &clip_rect;
    }
    if (!available() || !impl_->captured) {
        return false;
    }
    // Flush first, then ask for the names.  SDL creates a texture's GL
    // object lazily, on the first draw that actually reaches the driver -
    // and the copy capture_destination() made into the scratch is still
    // sitting in SDL's queue at this point, so asking now returns 0 for it
    // and the whole path silently declines on every single draw.
    if (!SDL_FlushRenderer(renderer)) {
        return false;
    }
    const GLuint source_name = texture_name(source);
    const GLuint dest_name = impl_->scratch_name;
    if (!source_name || !dest_name) {
        return false;
    }
    GLuint source2_name = 0;
    float source2_w = 0.0f;
    float source2_h = 0.0f;
    if (source2) {
        source2_name = texture_name(source2);
        if (!source2_name
            || !SDL_GetTextureSize(source2, &source2_w, &source2_h)
            || source2_w <= 0.0f || source2_h <= 0.0f) {
            return false;   // no half-blended sprite; let the caller fall back
        }
    }
    float source_w = 0.0f;
    float source_h = 0.0f;
    if (!SDL_GetTextureSize(source, &source_w, &source_h)
        || source_w <= 0.0f || source_h <= 0.0f) {
        return false;
    }
    const float target_w = static_cast<float>(impl_->scratch_width);
    const float target_h = static_cast<float>(impl_->scratch_height);

    // Destination rectangle to clip space, y NOT inverted.  SDL renders
    // into a target through a projection that already puts its y-down origin
    // at GL's bottom-left, so a quad written straight into clip space lands
    // the right way up; inverting it here put the picture at the wrong end
    // of the screen.  Established by drawing a corner mark into a known
    // target and reading back where it went - if it is ever wrong again the
    // picture is upside down, which is not subtle.
    const float x0 = dst.x / target_w * 2.0f - 1.0f;
    const float x1 = (dst.x + dst.w) / target_w * 2.0f - 1.0f;
    const float y0 = dst.y / target_h * 2.0f - 1.0f;
    const float y1 = (dst.y + dst.h) / target_h * 2.0f - 1.0f;

    float u0 = src.x / source_w;
    float u1 = (src.x + src.w) / source_w;
    float v0 = src.y / source_h;
    float v1 = (src.y + src.h) / source_h;
    if (flip_x) std::swap(u0, u1);
    if (flip_y) std::swap(v0, v1);

    const GLfloat quad[16] = {
        x0, y0, u0, v0,
        x1, y0, u1, v0,
        x0, y1, u0, v1,
        x1, y1, u1, v1,
    };

    glUseProgram(impl_->program);
    const auto bind = [](GLenum unit, GLuint name) {
        glActiveTexture(unit);
        glBindTexture(GL_TEXTURE_2D, name);
        // Sampling state travels with the texture and these belong to SDL,
        // so whatever it last set is what applies.  Nearest on both: this is
        // a one-texel-per-fragment composite and any filtering would be an
        // error rather than a smoothing.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    bind(GL_TEXTURE0, source_name);
    bind(GL_TEXTURE1, dest_name);
    if (source2_name) {
        bind(GL_TEXTURE2, source2_name);
    }
    glUniform1i(impl_->source_location, 0);
    glUniform1i(impl_->dest_location, 1);
    glUniform1i(impl_->source2_location, 2);
    // v_uv is normalised against the first bitmap; this rescales it so the
    // second is read at the same texel.
    glUniform2f(
        impl_->source2_scale_location,
        source2_name ? source_w / source2_w : 1.0f,
        source2_name ? source_h / source2_h : 1.0f);
    glUniform1i(impl_->pair_location, std::clamp(pair, 0, 256));
    glUniform1i(impl_->folded_location,
                (th2::texture_source_folded(source) ? 1 : 0)
                    | (th2::texture_source_folded(source2) ? 2 : 0));
    glUniform1i(impl_->rows_location, 3);
    glUniform1i(impl_->row_count_location, poly_rows ? poly_row_count : 0);
    if (poly_rows) {
        if (!impl_->rows_name) {
            glGenTextures(1, &impl_->rows_name);
        }
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, impl_->rows_name);
        // Integer textures cannot be filtered; texelFetch needs NEAREST set
        // or the texture is incomplete and reads zero.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        GLint row_length = 0;
        GLint alignment = 4;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, 2, poly_row_count, 0,
                     GL_RGBA_INTEGER, GL_INT, poly_rows);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    }
    const auto to_int = [](float value) {
        return static_cast<GLint>(std::lround(value));
    };
    glUniform4i(impl_->src_rect_location, to_int(src.x), to_int(src.y),
                to_int(src.w), to_int(src.h));
    glUniform4i(impl_->dst_rect_location, to_int(dst.x), to_int(dst.y),
                to_int(dst.w), to_int(dst.h));
    glUniform2i(impl_->src2_origin_location, to_int(src.x), to_int(src.y));
    glUniform2i(impl_->flip_location, flip_x ? 1 : 0, flip_y ? 1 : 0);
    glUniform2f(impl_->target_location, target_w, target_h);
    glUniform1i(impl_->alpha_location, std::clamp(alpha, 0, 256));
    glUniform1i(impl_->bright_locations[0], bright_r);
    glUniform1i(impl_->bright_locations[1], bright_g);
    glUniform1i(impl_->bright_locations[2], bright_b);
    glUniform1i(impl_->mode_location, mode);
    glUniform3i(impl_->ink_location, bright_r, bright_g, bright_b);

    // The shader has already folded the destination in, so the blend unit
    // must not do it again.  Put these back rather than trusting SDL to
    // reset them: it caches its own idea of the GL state.
    const GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    const GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    glDisable(GL_BLEND);
    // Scissor and viewport are SDL's, set for whatever it last drew - a clip
    // rectangle, or a sub-rect of the target.  The quad below is in clip
    // space over the whole target, so both have to be the whole target too
    // or it lands scaled into a corner and clipped to something unrelated.
    // The probes never caught this: they run on a freshly cleared target
    // where SDL happened to have left the viewport covering all of it.
    if (clip) {
        // ClipRect, as a scissor.  GL counts its box from the bottom of the
        // framebuffer and SDL's rectangle from the top, so only the row
        // range flips; the quad itself is unchanged, because clipping is not
        // a smaller draw but the same draw with less of it kept.
        glEnable(GL_SCISSOR_TEST);
        glScissor(clip->x, impl_->scratch_height - (clip->y + clip->h),
                  clip->w, clip->h);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glViewport(0, 0, impl_->scratch_width, impl_->scratch_height);
    glBindVertexArray(impl_->vertex_array);
    glBindBuffer(GL_ARRAY_BUFFER, impl_->vertex_buffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    if (blend_was_on) {
        glEnable(GL_BLEND);
    }
    if (scissor_was_on) {
        glEnable(GL_SCISSOR_TEST);
    }
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // The target has changed under SDL's feet; the next capture must see it.
    impl_->captured = false;
    return glGetError() == GL_NO_ERROR;
}

#else   // no GLES headers: a stub that is never available

struct GlExactBlend::Impl {};
GlExactBlend::GlExactBlend(SDL_Renderer*) : impl_(nullptr) {}
GlExactBlend::~GlExactBlend() = default;
bool GlExactBlend::available() const { return false; }
bool GlExactBlend::capture_destination(SDL_Renderer*, const SDL_FRect*) { return false; }
bool GlExactBlend::draw(SDL_Renderer*, SDL_Texture*, const SDL_FRect&,
                        const SDL_FRect&, bool, bool, int, int, int, int, int,
                        SDL_Texture*, int, const SDL_Rect*, const int*, int)
{
    return false;
}

#endif

}  // namespace th2
