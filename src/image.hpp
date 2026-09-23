#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <chrono>
#include <memory>
#include <vector>
#include <span>
#include <string_view>

namespace th2 {

// Decodes a TGA a band of rows at a time.
//
// A large picture is twenty milliseconds of work - three vblanks - and the
// frame that wants it cannot absorb that, so decoding it early is only useful
// if it can also be put down and picked up again.  The rows are independent
// once the header is parsed, so this is simply a matter of remembering which
// one is next.
class TgaStream {
public:
    // Owns its bytes: whatever queued this will not be around on the frame
    // that finishes it.
    explicit TgaStream(std::vector<std::uint8_t> bytes);
    ~TgaStream();
    TgaStream(TgaStream&&) noexcept;
    TgaStream& operator=(TgaStream&&) noexcept;
    TgaStream(const TgaStream&) = delete;
    TgaStream& operator=(const TgaStream&) = delete;

    // Decodes until the budget runs out or the picture is finished.  True
    // when there is nothing left to do.
    bool advance(std::chrono::nanoseconds budget);
    bool done() const;
    // Only meaningful once done(); the caller takes ownership.
    SDL_Surface* take();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Set on a surface, and copied onto the texture made from it, when the file
// it came from carried an alpha channel.  Such a picture is one the engine
// stores premultiplied, so the blend has to fold the alpha in rather than
// take the colour at face value - see mark_source_alpha in image.cpp.
inline constexpr const char* source_alpha_property = "th2.source_alpha";
bool surface_has_source_alpha(SDL_Surface* surface);
void carry_source_alpha(SDL_Surface* from, SDL_Texture* to);
bool texture_has_source_alpha(SDL_Texture* texture);
// A source-alpha surface whose colour has been folded on the CPU already -
// BlendTable[a][c], as DSP_LoadBmp does before it applies a tone curve - so
// a draw must not fold it again.
inline constexpr const char* source_folded_property = "th2.source_folded";
// LoadBmpSet's alpha-plane BMP: BmpSet.pos, the picture's offset inside its
// logical frame, which DSP_DrawGraph takes off every source coordinate.
inline constexpr const char* bmp_pos_x_property = "th2.bmp_pos_x";
inline constexpr const char* bmp_pos_y_property = "th2.bmp_pos_y";
inline constexpr const char* bmp_alpha_plane_property = "th2.bmp_alpha_plane";
void fold_source_alpha(SDL_Surface* surface);
bool texture_source_folded(SDL_Texture* texture);

SDL_Surface* load_image(
    std::span<const std::uint8_t> bytes, std::string_view name);
SDL_Surface* load_tga(std::span<const std::uint8_t> bytes);
// Which of Bmp.cpp's BMP_SetTonecurve_* the load would have gone through -
// keyed, like the fold, on the depth DSP_LoadBmp was asked for:
//   palette: _BT - the vividness mix truncates once, and only runs when
//            there is a curve to apply;
//   full:    _F  - the mix truncates each term, and runs with no curve too;
//   folded:  _T  - _F's mix, then a partial texel is unfolded through
//            BlendTable2[a], mapped, and folded again through BlendTable[a].
enum class ToneTarget { palette, full, folded };
void apply_tone_curve(
    SDL_Surface* surface,
    std::span<const std::uint8_t> curve,
    int vividness,
    ToneTarget target = ToneTarget::palette);

}  // namespace th2
