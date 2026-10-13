#pragma once

#include <span>

namespace th2 {

// The CuNNy NVL models the port ships, smallest first.  Their weights are
// shaders/cunny/cunny-<model>-nvl.bin, written by tools/cunny_convert.py from
// CuNNy's Magpie effects and embedded at build time (cmake/embed_cunny.cmake,
// in this order).
enum class CunnyModel {
    fast,     // luma, 12-12-8:       1.2 GMAC per 800x600 picture
    c4x16,    // rgb, 4 x 16 layers:  5.5 GMAC
    c4x32,    // rgb, 4 x 32 layers: 19.8 GMAC
};

// The model's weight file, as tools/cunny_convert.py wrote it.
std::span<const unsigned char> cunny_weights(CunnyModel model);

}  // namespace th2
