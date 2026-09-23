#version 450

// The 2002 engine's compositing arithmetic, in the engine's integers.
//
// Draw32.cpp blends premultiplied, truncating both halves at /256:
//
//     brev_tbl = BlendTable[ 256 - src1_p->a ];
//     dest_p->b = brev_tbl[ dest_p->b ] + src1_p->b;
//
// with BlendTable[i][j] = LIM((i*j)>>8, 0, 255), and BMP_CreateTableBright_T
// building the brightness the same way at /128.  SDL rounds at /255 in both
// places, which is never darker, so every composited pixel came out up to
// two levels bright and a fade disagreed with the reference on the whole
// screen at once.
//
// PRECISION.  Every product here is in `int`, deliberately, and never in
// float.  A mediump float is fp16: it represents integers exactly only to
// 2048 and overflows to infinity past 65504, so `255*256` would be wrong and
// `256*256` would be Inf - which is how this class of shader fails on a
// phone while working on a desktop.  int avoids that (GLSL ES requires highp
// int in fragment shaders, and this compiles to SPIRV with 32-bit OpTypeInt
// and no RelaxedPrecision decorations), and the largest product below is
// 255*256 = 65280.  It is checked rather than trusted: Display::ExactBlend
// runs the tables through this shader at startup and compares against the
// CPU, and turns the whole path off if the GPU disagrees anywhere.
layout(set = 3, binding = 0) uniform Blend {
    int engine_alpha;       // DRW_BLD's parameter, 0..256
    int bright_r;           // BrightTable index, 0..255, 128 neutral
    int bright_g;
    int bright_b;
} blend;

layout(set = 2, binding = 0) uniform sampler2D source_image;

// There is deliberately no destination sampler.  The other half of the
// blend - BlendTable[256-a] against what is already in the target - would
// need the target readable here, and SDL 3.4.16 does not populate
// SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER for any texture, so an SDL_Texture
// cannot be bound as an extra sampler at all.  Until that lands (or the art
// pass moves off SDL_Render onto the GPU API directly) the destination stays
// with the fixed-function blend unit, which rounds and cannot be told not
// to, and each composite can land one level high.

layout(location = 0) in vec4 color;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec4 output_color;

// BlendTable[i][j] = (i*j)>>8, with i on the rasteriser's 0..256 alpha scale.
int blend_table(int alpha, int value)
{
    return clamp((clamp(alpha, 0, 256) * clamp(value, 0, 255)) >> 8, 0, 255);
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
    vec4 texel = texture(source_image, uv) * color;
    ivec3 raw = ivec3(round(texel.rgb * 255.0));
    // Brightness first, then the alpha - the order the rasteriser applies
    // them in, and the two truncations do not commute.
    ivec3 src = ivec3(bright_of(raw.r, blend.bright_r),
                      bright_of(raw.g, blend.bright_g),
                      bright_of(raw.b, blend.bright_b));
    int texel_alpha = int(round(texel.a * 255.0));
    int blnd = clamp(blend.engine_alpha, 0, 256);

    // Draw32.cpp splits on the source texel being fully opaque: that branch
    // uses BlendTable[256 - blnd] against the destination and blnd itself
    // against the source, while the partial branch folds the texel's own
    // alpha in through BlendTable first.
    int eff = texel_alpha == 255 ? blnd : blend_table(blnd, texel_alpha);
    ivec3 premultiplied = ivec3(blend_table(eff, src.r),
                                blend_table(eff, src.g),
                                blend_table(eff, src.b));

    // Premultiplied out, with the alpha on the engine's /256 scale so the
    // blend unit at least scales the destination the way the engine does.
    output_color = vec4(vec3(premultiplied) / 255.0, float(eff) / 256.0);
}
