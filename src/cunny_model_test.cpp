// The embedded CuNNy models parse, have the shapes the port expects, and turn
// into a sensible set of passes.  With --dump DIR the generated shaders are
// written out (vertex.vert, <model>-<n>.frag) for glslangValidator.

#include "cunny_model.hpp"
#include "cunny_weights.hpp"

#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

struct Expected {
    th2::CunnyModel model;
    const char* name;
    bool rgb;
    std::vector<int> outputs;
};

}  // namespace

int main(int argc, char** argv)
{
    const char* dump = nullptr;
    if (argc == 3 && std::string_view(argv[1]) == "--dump") {
        dump = argv[2];
    }
    const std::vector<Expected> expected{
        {th2::CunnyModel::fast, "fast", false, {12, 12, 8, 4}},
        {th2::CunnyModel::c4x16, "4x16", true, {16, 16, 16, 16, 16, 12}},
        {th2::CunnyModel::c4x32, "4x32", true, {32, 32, 32, 32, 32, 12}},
    };
    if (dump) {
        std::ofstream(std::format("{}/vertex.vert", dump))
            << th2::cunny_vertex_shader();
    }
    for (const auto& e : expected) {
        const auto data = th2::cunny_weights(e.model);
        const auto network = th2::parse_cunny(data);
        check(network.has_value(), std::format("{} parses", e.name));
        if (!network) {
            continue;
        }
        check(network->rgb == e.rgb, std::format("{} colour mode", e.name));
        std::vector<int> outputs;
        for (const auto& conv : network->convs) {
            outputs.push_back(conv.outputs);
        }
        check(outputs == e.outputs, std::format("{} layer sizes", e.name));

        // A truncated or extended file is rejected, not half-read.
        check(!th2::parse_cunny(data.first(data.size() - 4)),
              std::format("{} truncated is rejected", e.name));
        std::vector<unsigned char> longer(data.begin(), data.end());
        longer.push_back(0);
        check(!th2::parse_cunny(longer),
              std::format("{} with trailing bytes is rejected", e.name));

        for (const int max_outputs : {4, 8}) {
            const auto passes = th2::cunny_passes(*network, max_outputs);
            check(!passes.empty() && passes.back().output,
                  std::format("{} ends in the output pass", e.name));
            int blocks = 0;
            for (const auto& pass : passes) {
                check(pass.blocks <= max_outputs,
                      std::format("{} pass writes {} blocks", e.name,
                                  pass.blocks));
                if (!pass.output) {
                    blocks += pass.blocks;
                }
                // One block per input texture (the art counts as one),
                // each within GLES 3.0's minimum block size and the 12
                // blocks a fragment shader is guaranteed.
                const std::size_t groups = pass.layer == 0
                    ? 1 : static_cast<std::size_t>(pass.input_blocks);
                check(pass.uniform_blocks.size() == groups
                          && groups <= 12,
                      std::format("{} pass has {} uniform blocks", e.name,
                                  pass.uniform_blocks.size()));
                for (const auto& block : pass.uniform_blocks) {
                    check(block.size() * sizeof(float) <= 16384,
                          std::format("{} uniform block of {} floats",
                                      e.name, block.size()));
                }
            }
            int hidden = 0;
            for (std::size_t i = 0; i + 1 < outputs.size(); ++i) {
                hidden += outputs[i] / 4;
            }
            check(blocks == hidden,
                  std::format("{} writes every hidden block once", e.name));
            // Every weight and bias goes to the GPU exactly once.
            std::size_t floats = 0;
            std::size_t expected_floats = 0;
            for (const auto& pass : passes) {
                for (const auto& block : pass.uniform_blocks) {
                    floats += block.size();
                }
            }
            for (const auto& conv : network->convs) {
                expected_floats += conv.weight.size() + conv.bias.size();
            }
            check(floats == expected_floats,
                  std::format("{} uploads {} of {} weights", e.name, floats,
                              expected_floats));
            if (dump && max_outputs == 4) {
                for (std::size_t i = 0; i < passes.size(); ++i) {
                    std::ofstream(std::format("{}/{}-{}.frag", dump, e.name, i))
                        << passes[i].fragment;
                }
            }
        }
    }
    std::vector<unsigned char> junk{'C', 'N', 'N', 'Y', 9, 0, 0, 0};
    check(!th2::parse_cunny(junk), "unknown version is rejected");
    check(!th2::parse_cunny({}), "empty is rejected");
    if (failures == 0) {
        std::puts("cunny model: ok");
    }
    return failures == 0 ? 0 : 1;
}
