#include "image.hpp"

#include <array>
#include <ranges>
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

bool surface_has_source_alpha(SDL_Surface* surface)
{
    return surface
        && SDL_GetBooleanProperty(
            SDL_GetSurfaceProperties(surface), source_alpha_property, false);
}

void carry_source_alpha(SDL_Surface* from, SDL_Texture* to)
{
    if (to && surface_has_source_alpha(from)) {
        SDL_SetBooleanProperty(
            SDL_GetTextureProperties(to), source_alpha_property, true);
    }
}

bool texture_has_source_alpha(SDL_Texture* texture)
{
    return texture
        && SDL_GetBooleanProperty(
            SDL_GetTextureProperties(texture), source_alpha_property, false);
}

void fold_source_alpha(SDL_Surface* surface)
{
    if (!surface || surface->format != SDL_PIXELFORMAT_RGBA32) {
        throw std::runtime_error("alpha fold requires an RGBA32 surface");
    }
    auto* row = static_cast<std::uint8_t*>(surface->pixels);
    for (int y = 0; y < surface->h; ++y, row += surface->pitch) {
        for (int x = 0; x < surface->w; ++x) {
            std::uint8_t* pixel = row + x * 4;
            const int alpha = pixel[3];
            for (int c = 0; c < 3; ++c) {
                pixel[c] = static_cast<std::uint8_t>((alpha * pixel[c]) >> 8);
            }
        }
    }
}

bool texture_source_folded(SDL_Texture* texture)
{
    return texture
        && SDL_GetBooleanProperty(
            SDL_GetTextureProperties(texture), source_folded_property, false);
}

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
        // LoadBmpSet's own flavour of BMP: bfReserved1 set means an 8 bit
        // picture with a second plane after the pixels, one alpha byte per
        // pixel with the same row padding, and biX/YPelsPerMeter holding the
        // picture's offset (LOWORD) inside a larger logical frame (HIWORD).
        // SDL_LoadBMP reads neither, so the weather's petals came out
        // opaque with black keyed away and a column off.
        if (bytes.size() >= 54 && bytes[0] == 'B' && bytes[1] == 'M'
            && (bytes[6] | (bytes[7] << 8)) != 0) {
            const auto u16 = [&](std::size_t at) {
                return static_cast<std::uint32_t>(bytes[at] | (bytes[at + 1] << 8));
            };
            const auto u32 = [&](std::size_t at) {
                return u16(at) | (u16(at + 2) << 16);
            };
            const std::uint32_t offset = u32(10);
            const std::uint32_t header = u32(14);
            const int width = static_cast<std::int32_t>(u32(18));
            int height = static_cast<std::int32_t>(u32(22));
            const int bits = static_cast<int>(u16(28));
            const std::uint32_t x_ppm = u32(38);
            const std::uint32_t y_ppm = u32(42);
            const bool bottom_up = height > 0;
            height = std::abs(height);
            const int pad = (4 - width % 4) % 4;
            const std::size_t plane =
                static_cast<std::size_t>(width + pad) * height;
            if (bits == 8 && width > 0 && height > 0
                && offset + 2 * plane <= bytes.size()) {
                SDL_Surface* surface = SDL_CreateSurface(
                    width, height, SDL_PIXELFORMAT_RGBA32);
                if (!surface) {
                    throw std::runtime_error(SDL_GetError());
                }
                const std::size_t palette = 14 + header;
                for (int y = 0; y < height; ++y) {
                    const int row = bottom_up ? height - 1 - y : y;
                    const std::size_t src = offset
                        + static_cast<std::size_t>(row) * (width + pad);
                    auto* dst = static_cast<std::uint8_t*>(surface->pixels)
                        + static_cast<std::size_t>(y) * surface->pitch;
                    for (int x = 0; x < width; ++x) {
                        const std::size_t index = bytes[src + x];
                        const std::size_t entry = palette + index * 4;
                        dst[x * 4 + 0] = entry + 2 < offset ? bytes[entry + 2] : 0;
                        dst[x * 4 + 1] = entry + 1 < offset ? bytes[entry + 1] : 0;
                        dst[x * 4 + 2] = entry < offset ? bytes[entry] : 0;
                        dst[x * 4 + 3] = bytes[src + plane + x];
                    }
                }
                const auto props = SDL_GetSurfaceProperties(surface);
                SDL_SetNumberProperty(props, bmp_pos_x_property, x_ppm & 0xffff);
                SDL_SetNumberProperty(props, bmp_pos_y_property, y_ppm & 0xffff);
                SDL_SetBooleanProperty(props, bmp_alpha_plane_property, true);
                return surface;
            }
        }
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
    bool has_alpha = false;
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

    // Only a 32 bit picture.  A paletted one with an alpha palette goes to
    // the rasteriser's 8 bit path (DRW_DrawBMP_TB), which copies the raw
    // palette colour for an opaque texel and folds the alpha in itself for a
    // partial one - exactly the straight-alpha blend.  Marking those too put
    // the time-skip clock's dial (clock98.tga) a level dark.
    header.has_alpha = header.image_type == 2 && header.depth == 32;
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


namespace {
// The rasteriser's bitmaps carry their colour already multiplied by their own
// alpha - BMP_MakeBMP's BlendTable[a][c] - and Draw32 adds that stored colour
// in without scaling it again.  Ours keep straight alpha, so the blend has to
// fold it in itself, and it can only know to do that for a picture that had
// an alpha channel to begin with: a 24 bit background is opaque and must be
// drawn at its own value, while a 32 bit sprite is a level darker even where
// it is solid, because BlendTable[255][c] is (255*c)>>8.
void mark_source_alpha(SDL_Surface* surface, bool has_alpha)
{
    if (!surface || !has_alpha) {
        return;
    }
    SDL_SetBooleanProperty(
        SDL_GetSurfaceProperties(surface), source_alpha_property, true);
}
}  // namespace

SDL_Surface* load_tga(std::span<const std::uint8_t> bytes)
{
    const auto header = parse_tga_header(bytes);
    SDL_Surface* surface = create_tga_surface(header);
    mark_source_alpha(surface, header.has_alpha);
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
    mark_source_alpha(state_->surface, state_->header.has_alpha);
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
    int vividness,
    ToneTarget target)
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
    auto map_channel = [&](int value, int channel) -> int {
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
    // BlendTable[i][j] = (i*j)>>8; BlendTable2[i][j] = min((j<<8)/(i+1), 255)
    const auto fold = [](int alpha, int value) { return (alpha * value) >> 8; };
    const auto unfold = [](int alpha, int value) {
        return std::min((value << 8) / (alpha + 1), 255);
    };

    auto* row = static_cast<std::uint8_t*>(surface->pixels);
    for (int y = 0; y < surface->h; ++y, row += surface->pitch) {
        for (int x = 0; x < surface->w; ++x) {
            std::uint8_t* pixel = row + x * 4;
            int rgb[3] = {pixel[0], pixel[1], pixel[2]};
            const int alpha = pixel[3];
            if (target == ToneTarget::palette) {
                // _BT: nothing at all without a curve.
                if (curve.empty()) {
                    continue;
                }
                if (vividness != 256) {
                    const int gray =
                        ((rgb[0] * 77 + rgb[1] * 28 + rgb[2] * 151) >> 8)
                        * (256 - vividness);
                    for (int& c : rgb) {
                        c = (gray + c * vividness) >> 8;
                    }
                }
            } else if (vividness == 0) {
                const int gray = (rgb[0] * 77 + rgb[1] * 28 + rgb[2] * 151) >> 8;
                rgb[0] = rgb[1] = rgb[2] = gray;
            } else if (vividness != 256) {
                // glay = vivtabl1[...]; c = glay + vivtabl2[c] - into a BYTE.
                const int gray = fold(
                    256 - vividness,
                    (rgb[0] * 77 + rgb[1] * 28 + rgb[2] * 151) >> 8);
                for (int& c : rgb) {
                    c = (gray + fold(vividness, c)) & 255;
                }
            }
            if (!curve.empty()) {
                if (target != ToneTarget::folded || alpha == 255) {
                    for (int c = 0; c < 3; ++c) {
                        rgb[c] = map_channel(rgb[c], c);
                    }
                } else if (alpha != 0) {
                    for (int c = 0; c < 3; ++c) {
                        rgb[c] = fold(alpha,
                                      map_channel(unfold(alpha, rgb[c]), c));
                    }
                }
            }
            for (int c = 0; c < 3; ++c) {
                pixel[c] = static_cast<std::uint8_t>(rgb[c]);
            }
        }
    }
}

}  // namespace th2
