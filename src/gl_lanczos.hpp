#pragma once

#include <memory>

namespace th2 {

// Lanczos2 resampling with anti-ringing in (approximately) linear light, the
// filter CuNNy pairs with its network (magpie/Downscale.hlsl, CC0):
//     lanczos(x) = sin(pi x) sin(pi x / 2) / x^2, weights normalised;
//     samples squared before filtering and the result square-rooted - gamma
//     2 standing in for 2.2;
//     the result clamped to the min/max of the samples it was made from.
// Separable here - a horizontal pass into an 8-bit intermediate and a
// vertical pass, each squaring its samples and taking the root of its
// result, so the intermediate is back in gamma and needs no float target -
// so a pixel costs 4 + 4 taps rather than 16.  Shrinking, the kernel is
// widened by the ratio (up to 8 taps an axis at half size), which the Magpie
// effect, applied only between 1x and 2x of its input, does not need.
//
// Raw GLES on SDL's context: the caller flushes SDL first and puts the
// framebuffer binding back afterwards, as GlCunny does around both.
class GlLanczos {
public:
    GlLanczos();
    ~GlLanczos();

    GlLanczos(const GlLanczos&) = delete;
    GlLanczos& operator=(const GlLanczos&) = delete;

    bool ready() const;

    // Resamples GL texture `source` (sw x sh) into GL texture `target`
    // (tw x th).  Leaves its own framebuffer bound.
    bool run(unsigned source, int sw, int sh, unsigned target, int tw, int th);

    // `target` is about to be deleted: a new texture may reuse its name,
    // and the framebuffer would still hold the old one.
    void forget_target();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
