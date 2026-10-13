// th2-cunny-check MODEL IN.bmp OUT.bmp [RUNS [WIDTHxHEIGHT]]
//
// Runs one CuNNy model (fast, 4x16, 4x32) on IN through GlCunny, exactly as
// the game does - the picture in an SDL render target, SDL's GLES renderer -
// and writes the result to OUT: the 2x picture by default, which is what
// tools/cunny_reference.py computes (the Lanczos pass is an identity at
// exactly 2x), or WIDTHxHEIGHT through the Lanczos resampling.  With RUNS,
// also times that many runs of the network and resampling, each finished
// with glFinish, after as many untimed ones, and prints the median.

#include "gl_cunny.hpp"

#include <GLES3/gl3.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s fast|4x16|4x32 IN.bmp OUT.bmp "
                     "[RUNS [WIDTHxHEIGHT]]\n", argv[0]);
        return 2;
    }
    const std::string_view name = argv[1];
    th2::CunnyModel model{};
    if (name == "fast") {
        model = th2::CunnyModel::fast;
    } else if (name == "4x16") {
        model = th2::CunnyModel::c4x16;
    } else if (name == "4x32") {
        model = th2::CunnyModel::c4x32;
    } else {
        std::fprintf(stderr, "unknown model %s\n", argv[1]);
        return 2;
    }
    const int runs = argc > 4 ? std::atoi(argv[4]) : 0;

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("cunny", 64, 64, SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr)
                                    : nullptr;
    SDL_Surface* loaded = SDL_LoadBMP(argv[2]);
    SDL_Surface* picture = loaded
        ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
    if (!renderer || !picture) {
        std::fprintf(stderr, "setup: %s\n", SDL_GetError());
        return 1;
    }
    // The game's art is a render target the engine draws into; this one has
    // the picture drawn into it the same way.
    SDL_Texture* source = SDL_CreateTextureFromSurface(renderer, picture);
    SDL_Texture* art = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET,
        picture->w, picture->h);
    SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(renderer, art);
    SDL_RenderTexture(renderer, source, nullptr, nullptr);
    SDL_SetRenderTarget(renderer, nullptr);

    const auto building_from = std::chrono::steady_clock::now();
    th2::GlCunny cunny(renderer, model);
    const std::chrono::duration<double, std::milli> construct =
        std::chrono::steady_clock::now() - building_from;
    int out_w = picture->w * 2;
    int out_h = picture->h * 2;
    if (argc > 5 && std::sscanf(argv[5], "%dx%d", &out_w, &out_h) != 2) {
        std::fprintf(stderr, "bad size %s\n", argv[5]);
        return 2;
    }
    // As the game does: upscale() each frame, null until the build is over.
    SDL_Texture* output = nullptr;
    while (cunny.building()) {
        output = cunny.upscale(renderer, art, true, out_w, out_h);
        if (cunny.building()) {
            SDL_Delay(1);
        }
    }
    const std::chrono::duration<double, std::milli> built =
        std::chrono::steady_clock::now() - building_from;
    std::printf("%s: constructor returned after %.0f ms, ready after %.0f ms "
                "(KHR_parallel_shader_compile %s)\n", argv[1],
                construct.count(), built.count(),
                SDL_GL_ExtensionSupported("GL_KHR_parallel_shader_compile")
                    ? "yes" : "no");
    if (!cunny.available()) {
        std::fprintf(stderr, "CuNNy %s is not available\n", argv[1]);
        return 1;
    }
    if (!output) {
        std::fprintf(stderr, "upscale failed\n");
        return 1;
    }
    if (runs > 0) {
        // As many runs again first, untimed: the GPU's clocks ramp up.
        std::vector<double> times;
        for (int i = 0; i < 2 * runs; ++i) {
            glFinish();
            const auto started = std::chrono::steady_clock::now();
            cunny.upscale(renderer, art, true, out_w, out_h);
            glFinish();
            const std::chrono::duration<double, std::milli> took =
                std::chrono::steady_clock::now() - started;
            if (i >= runs) {
                times.push_back(took.count());
            }
        }
        std::sort(times.begin(), times.end());
        std::printf("%s: median %.2f ms, fastest %.2f ms per run (%d runs, "
                    "%dx%d to %dx%d)\n", argv[1], times[times.size() / 2],
                    times.front(), runs, picture->w, picture->h, out_w,
                    out_h);
    }
    SDL_SetRenderTarget(renderer, output);
    SDL_Surface* result = SDL_RenderReadPixels(renderer, nullptr);
    if (!result || !SDL_SaveBMP(result, argv[3])) {
        std::fprintf(stderr, "read back: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DestroySurface(result);
    SDL_DestroySurface(picture);
    SDL_DestroySurface(loaded);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
