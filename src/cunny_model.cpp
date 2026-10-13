#include "cunny_model.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>

namespace th2 {

namespace {

class Reader {
public:
    explicit Reader(std::span<const unsigned char> data) : data_(data) {}

    bool u32(std::uint32_t& value)
    {
        if (data_.size() - offset_ < 4) {
            return false;
        }
        value = static_cast<std::uint32_t>(data_[offset_])
            | static_cast<std::uint32_t>(data_[offset_ + 1]) << 8
            | static_cast<std::uint32_t>(data_[offset_ + 2]) << 16
            | static_cast<std::uint32_t>(data_[offset_ + 3]) << 24;
        offset_ += 4;
        return true;
    }

    bool floats(std::size_t count, std::vector<float>& out)
    {
        out.resize(count);
        for (auto& value : out) {
            std::uint32_t bits = 0;
            if (!u32(bits)) {
                return false;
            }
            value = std::bit_cast<float>(bits);
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }

    bool done() const { return offset_ == data_.size(); }

private:
    std::span<const unsigned char> data_;
    std::size_t offset_ = 0;
};

float weight(const CunnyNetwork::Conv& conv, int out, int in, int ky, int kx)
{
    return conv.weight[((static_cast<std::size_t>(out) * conv.inputs + in)
                        * 3 + ky) * 3 + kx];
}

constexpr const char* header = R"(#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
)";

// The nine taps of a 3x3 window around p, clamped to the picture - the
// Magpie effect samples with clamp addressing.  t<ky*3+kx>.
std::string taps(const char* centre, const char* sized_by)
{
    std::string s = std::format(
        "    ivec2 hi = textureSize({}, 0) - 1;\n", sized_by);
    for (int ky = 0; ky < 3; ++ky) {
        for (int kx = 0; kx < 3; ++kx) {
            s += std::format(
                "    ivec2 t{} = clamp({} + ivec2({}, {}), ivec2(0), hi);\n",
                ky * 3 + kx, centre, kx - 1, ky - 1);
        }
    }
    return s;
}

// The convolution of one pass: its uniform blocks (declared before main)
// and the statements that accumulate r<o>, output block first+o.  Input
// group g - the art for the first layer, else the last layer's texture
// u_in<g> - has its weights in block W<g> as w<g>[t*count + o], one vec4
// (luma), mat3x4 (rgb) or mat4 per tap and output block, columns being
// inputs and rows outputs; W0 starts with the biases, b[o].  Weights rather
// than literals in the source: a shader with thousands of literal matrices
// takes Mesa seconds to compile, and it compiles on the calling thread
// even with KHR_parallel_shader_compile.
struct Convolution {
    std::string declarations;
    std::string body;
    std::vector<std::vector<float>> blocks;
};

Convolution convolution(const CunnyNetwork& network, std::size_t layer,
                        int first, int count)
{
    const auto& conv = network.convs[layer];
    Convolution c;
    std::vector<float> bias;
    for (int o = 0; o < count; ++o) {
        const int base = (first + o) * 4;
        bias.insert(bias.end(), conv.bias.begin() + base,
                    conv.bias.begin() + base + 4);
        c.body += std::format("    vec4 r{} = b[{}];\n", o, o);
    }
    const bool art = layer == 0;
    const int channels = art ? (network.rgb ? 3 : 1) : 4;
    const int groups = art ? 1 : conv.inputs / 4;
    const char* type = channels == 1 ? "vec4"
        : channels == 3 ? "mat3x4" : "mat4";
    c.body += channels == 1 ? "    float s;\n"
        : channels == 3 ? "    vec3 s;\n" : "    vec4 s;\n";
    for (int g = 0; g < groups; ++g) {
        std::vector<float> data = g == 0 ? bias : std::vector<float>{};
        for (int t = 0; t < 9; ++t) {
            if (channels == 1) {
                c.body += std::format(
                    "    s = dot(texelFetch(u_art, t{}, 0).rgb, "
                    "vec3(0.299, 0.587, 0.114));\n", t);
            } else if (channels == 3) {
                c.body += std::format(
                    "    s = texelFetch(u_art, t{}, 0).rgb;\n", t);
            } else {
                c.body += std::format(
                    "    s = texelFetch(u_in{}, t{}, 0);\n", g, t);
            }
            for (int o = 0; o < count; ++o) {
                const int out = (first + o) * 4;
                // Column-major: column i (input g*4+i), rows the outputs.
                for (int i = 0; i < channels; ++i) {
                    for (int r = 0; r < 4; ++r) {
                        data.push_back(weight(conv, out + r, g * 4 + i,
                                              t / 3, t % 3));
                    }
                }
                c.body += std::format("    r{} += w{}[{}] * s;\n", o, g,
                                      t * count + o);
            }
        }
        c.declarations += std::format(
            "layout(std140) uniform W{} {{\n{}    {} w{}[{}];\n}};\n", g,
            g == 0 ? std::format("    vec4 b[{}];\n", count) : "", type, g,
            9 * count);
        c.blocks.push_back(std::move(data));
    }
    return c;
}

std::string samplers(bool art, int blocks)
{
    std::string s;
    if (art) {
        s += "uniform sampler2D u_art;\n";
    }
    for (int i = 0; i < blocks; ++i) {
        s += std::format("uniform sampler2D u_in{};\n", i);
    }
    return s;
}

}  // namespace

std::optional<CunnyNetwork> parse_cunny(std::span<const unsigned char> data)
{
    if (data.size() < 4 || std::memcmp(data.data(), "CNNY", 4) != 0) {
        return std::nullopt;
    }
    Reader in(data.subspan(4));
    std::uint32_t version = 0;
    std::uint32_t rgb = 0;
    std::uint32_t count = 0;
    if (!in.u32(version) || version != 1 || !in.u32(rgb) || rgb > 1
        || !in.u32(count) || count < 2 || count > 32) {
        return std::nullopt;
    }
    CunnyNetwork network;
    network.rgb = rgb == 1;
    const int channels = network.rgb ? 3 : 1;
    for (std::uint32_t n = 0; n < count; ++n) {
        std::uint32_t outputs = 0;
        std::uint32_t inputs = 0;
        if (!in.u32(outputs) || !in.u32(inputs)) {
            return std::nullopt;
        }
        const bool last = n + 1 == count;
        const std::uint32_t expected_inputs = n == 0
            ? static_cast<std::uint32_t>(channels)
            : static_cast<std::uint32_t>(network.convs.back().outputs);
        const bool outputs_ok = last
            ? outputs == static_cast<std::uint32_t>(4 * channels)
            : outputs % 4 == 0 && outputs >= 4 && outputs <= 64;
        if (inputs != expected_inputs || !outputs_ok) {
            return std::nullopt;
        }
        CunnyNetwork::Conv conv;
        conv.outputs = static_cast<int>(outputs);
        conv.inputs = static_cast<int>(inputs);
        if (!in.floats(std::size_t{outputs} * inputs * 9, conv.weight)
            || !in.floats(outputs, conv.bias)) {
            return std::nullopt;
        }
        network.convs.push_back(std::move(conv));
    }
    if (!in.done()) {
        return std::nullopt;
    }
    return network;
}

const char* cunny_vertex_shader()
{
    // One triangle over the whole target, from gl_VertexID alone - no
    // vertex buffer to keep.
    return R"(#version 300 es
void main()
{
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";
}

std::vector<CunnyPass> cunny_passes(const CunnyNetwork& network,
                                    int max_outputs)
{
    max_outputs = std::max(1, max_outputs);
    std::vector<CunnyPass> passes;
    const std::size_t hidden = network.convs.size() - 1;
    for (std::size_t layer = 0; layer < hidden; ++layer) {
        const auto& conv = network.convs[layer];
        const int input_blocks = layer == 0 ? 0 : conv.inputs / 4;
        for (int first = 0; first < conv.outputs / 4; first += max_outputs) {
            const int count = std::min(max_outputs, conv.outputs / 4 - first);
            CunnyPass pass;
            pass.reads_art = layer == 0;
            pass.input_layer = static_cast<int>(layer) - 1;
            pass.input_blocks = input_blocks;
            pass.layer = static_cast<int>(layer);
            pass.first_block = first;
            pass.blocks = count;
            auto c = convolution(network, layer, first, count);
            pass.uniform_blocks = std::move(c.blocks);
            std::string& s = pass.fragment;
            s = header;
            s += samplers(layer == 0, input_blocks);
            s += c.declarations;
            for (int o = 0; o < count; ++o) {
                s += std::format(
                    "layout(location = {}) out vec4 o{};\n", o, o);
            }
            s += "void main()\n{\n    ivec2 p = ivec2(gl_FragCoord.xy);\n";
            s += taps("p", layer == 0 ? "u_art" : "u_in0");
            s += c.body;
            // ReLU; the 8-bit target clamps to 1 and rounds to n/255, which
            // is what the NVL models were trained with (quantised, clamped
            // activations) and what the Magpie effect stores.
            for (int o = 0; o < count; ++o) {
                s += std::format("    o{} = max(r{}, 0.0);\n", o, o);
            }
            s += "}\n";
            passes.push_back(std::move(pass));
        }
    }

    // The last convolution, run per output pixel of the 2x picture: each
    // computes its input pixel's 4 (or 12) outputs and keeps the one the
    // pixel shuffle puts at its position, (dy*2 + dx).
    const auto& conv = network.convs.back();
    CunnyPass pass;
    pass.reads_art = true;
    pass.input_layer = static_cast<int>(hidden) - 1;
    pass.input_blocks = conv.inputs / 4;
    pass.layer = static_cast<int>(hidden);
    pass.blocks = 1;
    pass.output = true;
    auto c = convolution(network, hidden, 0, conv.outputs / 4);
    pass.uniform_blocks = std::move(c.blocks);
    std::string& s = pass.fragment;
    s = header;
    s += samplers(true, pass.input_blocks);
    s += c.declarations;
    s += "layout(location = 0) out vec4 o_color;\n";
    s += "void main()\n{\n"
         "    ivec2 q = ivec2(gl_FragCoord.xy);\n"
         "    ivec2 p = q / 2;\n"
         "    int k = (q.x & 1) + 2 * (q.y & 1);\n";
    s += taps("p", "u_in0");
    s += c.body;
    // The base is the art sampled bilinearly at this pixel's centre - the
    // art texture is set to GL_LINEAR before this pass.
    s += "    vec2 size2 = vec2(textureSize(u_art, 0) * 2);\n"
         "    vec4 base = texture(u_art, (vec2(q) + 0.5) / size2);\n";
    if (network.rgb) {
        s += "    o_color = vec4(clamp(base.rgb + vec3(r0[k], r1[k], r2[k]),"
             " 0.0, 1.0), base.a);\n";
    } else {
        // The Magpie effect's own Rec.601 matrices, both ways.
        s += "    vec3 yuv = vec3(dot(base.rgb, vec3(0.299, 0.587, 0.114)),\n"
             "                    dot(base.rgb, vec3(-0.169, -0.331, 0.5)),\n"
             "                    dot(base.rgb, vec3(0.5, -0.419, -0.081)));\n"
             "    yuv.x = clamp(yuv.x + r0[k], 0.0, 1.0);\n"
             "    o_color = vec4(dot(yuv, vec3(1.0, -0.00093, 1.401687)),\n"
             "                   dot(yuv, vec3(1.0, -0.3437, -0.71417)),\n"
             "                   dot(yuv, vec3(1.0, 1.77216, 0.00099)),"
             " base.a);\n";
    }
    s += "}\n";
    passes.push_back(std::move(pass));
    return passes;
}

}  // namespace th2
