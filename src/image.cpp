#include "image.hpp"

#include <array>
#include <span>
#include <memory>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace th2 {
namespace {

std::uint16_t read_u16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8);
}

}  // namespace

SDL_Surface* load_image(
    std::span<const std::uint8_t> bytes, std::string_view name)
{
    std::string extension;
    if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos) {
        extension.assign(name.substr(dot));
        for (char& byte : extension) {
            byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
        }
    }
    if (extension == ".tga") {
        return load_tga(bytes);
    }
    if (extension == ".bmp") {
        SDL_IOStream* input = SDL_IOFromConstMem(bytes.data(), bytes.size());
        if (!input) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Surface* loaded = SDL_LoadBMP_IO(input, true);
        if (!loaded) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Surface* surface =
            SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(loaded);
        if (!surface) {
            throw std::runtime_error(SDL_GetError());
        }
        return surface;
    }
    throw std::runtime_error("unsupported image extension: " + extension);
}

namespace {

// The parts of a TGA header the decoder needs, plus its palette.  Split out so
// the one-shot and the streaming decoders share one parser and one row loop:
// two copies of this would eventually disagree, and a picture that decodes
// differently depending on which path loaded it is not a bug anyone would
// enjoy finding.
struct TgaHeader {
    int width = 0;
    int height = 0;
    int image_type = 0;
    int depth = 0;
    bool top_origin = false;
    std::size_t source_pixel_size = 0;
    std::size_t pixels_at = 0;
    std::vector<std::array<std::uint8_t, 4>> palette;
};

TgaHeader parse_tga_header(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 18) {
        throw std::runtime_error("truncated TGA header");
    }

    const int id_length = bytes[0];
    const int color_map_type = bytes[1];
    const int color_map_length = read_u16(bytes.data() + 5);
    const int color_map_depth = bytes[7];

    TgaHeader header;
    header.image_type = bytes[2];
    header.width = read_u16(bytes.data() + 12);
    header.height = read_u16(bytes.data() + 14);
    header.depth = bytes[16];
    header.top_origin = (bytes[17] & 0x20) != 0;

    if (header.width <= 0 || header.height <= 0
        || (header.image_type != 1 && header.image_type != 2)) {
        throw std::runtime_error("unsupported TGA image type");
    }
    if ((header.image_type == 1
         && (header.depth != 8 || color_map_type != 1))
        || (header.image_type == 2
            && header.depth != 24 && header.depth != 32)) {
        throw std::runtime_error("unsupported TGA pixel format");
    }

    std::size_t position = 18 + id_length;
    if (color_map_type) {
        const std::size_t palette_pixel_size = color_map_depth / 8;
        if ((palette_pixel_size != 3 && palette_pixel_size != 4)
            || position + color_map_length * palette_pixel_size
                > bytes.size()) {
            throw std::runtime_error("invalid TGA palette");
        }
        header.palette.reserve(color_map_length);
        for (int i = 0; i < color_map_length; ++i) {
            const auto* pixel = bytes.data() + position;
            header.palette.push_back(
                {pixel[2], pixel[1], pixel[0],
                 static_cast<std::uint8_t>(
                     palette_pixel_size == 4 ? pixel[3] : 255)});
            position += palette_pixel_size;
        }
    }

    header.source_pixel_size = header.depth / 8;
    header.pixels_at = position;
    if (position
            + static_cast<std::size_t>(header.width) * header.height
                * header.source_pixel_size
        > bytes.size()) {
        throw std::runtime_error("truncated TGA pixels");
    }
    return header;
}

// Rows [first, first + count) of the image, in source order.
void decode_tga_rows(
    const TgaHeader& header, std::span<const std::uint8_t> bytes,
    SDL_Surface* surface, const SDL_PixelFormatDetails* details,
    int first, int count)
{
    auto* destination = static_cast<std::uint32_t*>(surface->pixels);
    const int destination_pitch = surface->pitch / sizeof(std::uint32_t);
    const std::size_t row_bytes =
        static_cast<std::size_t>(header.width) * header.source_pixel_size;

    for (int source_y = first; source_y < first + count; ++source_y) {
        const int destination_y = header.top_origin
            ? source_y : header.height - source_y - 1;
        std::size_t position =
            header.pixels_at + static_cast<std::size_t>(source_y) * row_bytes;
        for (int x = 0; x < header.width; ++x) {
            std::array<std::uint8_t, 4> color{};
            if (header.image_type == 1) {
                const auto index = bytes[position++];
                if (index >= header.palette.size()) {
                    throw std::runtime_error("TGA palette index out of range");
                }
                color = header.palette[index];
            } else {
                color = {bytes[position + 2], bytes[position + 1],
                         bytes[position],
                         static_cast<std::uint8_t>(
                             header.depth == 32 ? bytes[position + 3] : 255)};
                position += header.source_pixel_size;
            }
            destination[destination_y * destination_pitch + x]
                = SDL_MapRGBA(details, nullptr,
                              color[0], color[1], color[2], color[3]);
        }
    }
}

SDL_Surface* create_tga_surface(const TgaHeader& header)
{
    SDL_Surface* surface =
        SDL_CreateSurface(header.width, header.height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        throw std::runtime_error(SDL_GetError());
    }
    return surface;
}

}  // namespace

SDL_Surface* load_tga(std::span<const std::uint8_t> bytes)
{
    const auto header = parse_tga_header(bytes);
    SDL_Surface* surface = create_tga_surface(header);
    try {
        decode_tga_rows(
            header, bytes, surface,
            SDL_GetPixelFormatDetails(surface->format), 0, header.height);
    } catch (...) {
        SDL_DestroySurface(surface);
        throw;
    }
    return surface;
}

struct TgaStream::State {
    std::vector<std::uint8_t> bytes;
    TgaHeader header;
    SDL_Surface* surface = nullptr;
    const SDL_PixelFormatDetails* details = nullptr;
    int row = 0;
};

TgaStream::TgaStream(std::vector<std::uint8_t> bytes)
    : state_(std::make_unique<State>())
{
    state_->bytes = std::move(bytes);
    state_->header = parse_tga_header(state_->bytes);
    state_->surface = create_tga_surface(state_->header);
    state_->details = SDL_GetPixelFormatDetails(state_->surface->format);
}

TgaStream::~TgaStream()
{
    if (state_ && state_->surface) {
        SDL_DestroySurface(state_->surface);
    }
}

TgaStream::TgaStream(TgaStream&&) noexcept = default;
TgaStream& TgaStream::operator=(TgaStream&&) noexcept = default;

bool TgaStream::advance(std::chrono::nanoseconds budget)
{
    if (!state_ || state_->row >= state_->header.height) {
        return true;
    }
    const auto started = std::chrono::steady_clock::now();
    // A band at a time, so the clock is read once per band rather than once
    // per row.  Sixteen rows of an 800-wide picture is about 13k pixels,
    // comfortably under a millisecond.
    constexpr int band = 16;
    while (state_->row < state_->header.height) {
        const int count = std::min(band, state_->header.height - state_->row);
        decode_tga_rows(state_->header, state_->bytes, state_->surface,
                        state_->details, state_->row, count);
        state_->row += count;
        if (state_->row < state_->header.height
            && std::chrono::steady_clock::now() - started >= budget) {
            return false;
        }
    }
    return true;
}

bool TgaStream::done() const
{
    return !state_ || state_->row >= state_->header.height;
}

SDL_Surface* TgaStream::take()
{
    if (!state_) {
        return nullptr;
    }
    SDL_Surface* surface = state_->surface;
    state_->surface = nullptr;
    return surface;
}

void apply_tone_curve(
    SDL_Surface* surface,
    std::span<const std::uint8_t> curve,
    int vividness)
{
    if (!surface) {
        throw std::invalid_argument("tone curve surface is null");
    }
    if (!curve.empty() && curve.size() != 256
        && curve.size() != 768 && curve.size() != 1280) {
        throw std::runtime_error("invalid tone curve size");
    }

    vividness = std::clamp(vividness, 0, 256);
    if (surface->format != SDL_PIXELFORMAT_RGBA32) {
        throw std::runtime_error("tone curve requires an RGBA32 surface");
    }
    auto map_channel = [&](std::uint8_t value, int channel) {
        if (curve.empty()) {
            return value;
        }
        if (curve.size() == 256) {
            return curve[value];
        }
        if (curve.size() == 768) {
            return curve[static_cast<std::size_t>(channel) * 256 + value];
        }
        const auto channel_value =
            curve[256 + static_cast<std::size_t>(channel) * 256 + value];
        return curve[channel_value];
    };

    const auto* format = SDL_GetPixelFormatDetails(surface->format);
    const auto* palette = SDL_GetSurfacePalette(surface);
    auto* pixels = static_cast<std::uint32_t*>(surface->pixels);
    const auto pitch = surface->pitch / sizeof(std::uint32_t);
    for (int y = 0; y < surface->h; ++y) {
        for (int x = 0; x < surface->w; ++x) {
            auto& pixel = pixels[static_cast<std::size_t>(y) * pitch + x];
            std::uint8_t red = 0;
            std::uint8_t green = 0;
            std::uint8_t blue = 0;
            std::uint8_t alpha = 0;
            SDL_GetRGBA(
                pixel, format, palette, &red, &green, &blue, &alpha);
            if (vividness != 256) {
                const int gray = (red * 77 + green * 28 + blue * 151) >> 8;
                red = static_cast<std::uint8_t>(
                    (gray * (256 - vividness) + red * vividness) >> 8);
                green = static_cast<std::uint8_t>(
                    (gray * (256 - vividness) + green * vividness) >> 8);
                blue = static_cast<std::uint8_t>(
                    (gray * (256 - vividness) + blue * vividness) >> 8);
            }
            pixel = SDL_MapRGBA(
                format, palette,
                map_channel(red, 0),
                map_channel(green, 1),
                map_channel(blue, 2),
                alpha);
        }
    }
}

}  // namespace th2
