#include "archive.hpp"
#include "image.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: th2-image-info ARCHIVE IMAGE [OUT.bmp]\n";
        return 2;
    }
    try {
        const th2::Archive archive(argv[1]);
        const auto* entry = archive.find(argv[2]);
        if (!entry) {
            throw std::runtime_error("image not found");
        }
        SDL_Surface* surface = th2::load_image(archive.read(*entry), entry->name);
        std::cout << entry->name << ": " << surface->w << 'x' << surface->h
                  << ", " << SDL_GetPixelFormatName(surface->format) << '\n';
        if (argc == 4 && !SDL_SaveBMP(surface, argv[3])) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_DestroySurface(surface);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
