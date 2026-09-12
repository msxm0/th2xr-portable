#include "texture.hpp"

namespace th2app {

void TextureDeleter::operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
void SurfaceDeleter::operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
void WindowDeleter::operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
void RendererDeleter::operator()(SDL_Renderer* renderer) const { SDL_DestroyRenderer(renderer); }
void IoDeleter::operator()(SDL_IOStream* stream) const
{
    if (stream) {
        SDL_CloseIO(stream);
    }
}

}  // namespace th2app
