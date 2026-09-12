#pragma once

// The SDL owning-pointer aliases, split out of game.hpp so that code which
// only needs a texture handle - the display layer, say - does not have to
// pull in the whole game.

#include <SDL3/SDL.h>

#include <memory>

namespace th2app {

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const;
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;
struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const;
};
using Surface = std::unique_ptr<SDL_Surface, SurfaceDeleter>;
struct WindowDeleter {
    void operator()(SDL_Window* window) const;
};
struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const;
};
struct IoDeleter {
    void operator()(SDL_IOStream* stream) const;
};
using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;
using IoPtr = std::unique_ptr<SDL_IOStream, IoDeleter>;

}  // namespace th2app
