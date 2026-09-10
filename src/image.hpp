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

SDL_Surface* load_image(
    std::span<const std::uint8_t> bytes, std::string_view name);
SDL_Surface* load_tga(std::span<const std::uint8_t> bytes);
void apply_tone_curve(
    SDL_Surface* surface,
    std::span<const std::uint8_t> curve,
    int vividness);

}  // namespace th2
