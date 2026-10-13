#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace th2 {

// A CuNNy network: 3x3 convolutions, ReLU between them, and a 2x2 pixel
// shuffle of the last one's outputs added to the bilinear 2x picture.
struct CunnyNetwork {
    struct Conv {
        int outputs = 0;
        int inputs = 0;
        std::vector<float> weight;   // (outputs, inputs, ky, kx)
        std::vector<float> bias;     // outputs
    };
    // Luma models read Rec.601 Y and shuffle 4 outputs into it; rgb models
    // read R, G, B and shuffle 12, channel c*4 + (dy*2 + dx).
    bool rgb = false;
    std::vector<Conv> convs;
};

// tools/cunny_convert.py's format; nullopt for anything that is not exactly
// one well-formed network of that shape.
std::optional<CunnyNetwork> parse_cunny(std::span<const unsigned char> data);

// One draw of the network: a fragment shader and what it reads and writes.
struct CunnyPass {
    std::string fragment;
    // Inputs: the art itself (first layer and the output pass), then
    // `input_blocks` textures of four channels from the previous layer.
    bool reads_art = false;
    int input_layer = -1;        // which layer's textures, -1 for none
    int input_blocks = 0;
    // Outputs: textures [first_block, first_block + blocks) of `layer`, at
    // the art's size; or, for the last pass, the 2x picture.
    int layer = 0;
    int first_block = 0;
    int blocks = 0;
    bool output = false;
    // The weights: uniform blocks W0, W1, ... (W<g> for input group g), as
    // their std140 images - every member is made of vec4 columns, so that
    // is just the floats in order.  Each fits the 16384 bytes GLES 3.0
    // guarantees for GL_MAX_UNIFORM_BLOCK_SIZE.
    std::vector<std::vector<float>> uniform_blocks;
};

// The passes that run `network`, in order, writing at most `max_outputs`
// four-channel textures each (GL_MAX_DRAW_BUFFERS; 4 is what GLES 3.0 and
// WebGL2 guarantee).  The shaders are GLSL ES 3.00, the vertex stage being
// cunny_vertex_shader().
std::vector<CunnyPass> cunny_passes(const CunnyNetwork& network,
                                    int max_outputs);
const char* cunny_vertex_shader();

}  // namespace th2
