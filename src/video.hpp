#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace th2 {

// Where a movie's bytes come from.  The player reads as it plays, a buffer
// at a time, so a source need not hold the file: the ending movies are up to
// 80 MB, and copying one whole before its first frame was an 18 ms stall and
// that much more wasm heap in the browser.
class VideoSource {
public:
    virtual ~VideoSource() = default;
    virtual std::uint64_t size() const = 0;
    // Fills `out` from `offset`; false on a short or failed read.
    virtual bool read(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
};

class VideoPlayer {
public:
    VideoPlayer(SDL_Renderer* renderer, std::unique_ptr<VideoSource> source,
                SDL_FRect destination);
    // A movie already in memory; the bytes must outlive the player.
    VideoPlayer(SDL_Renderer* renderer, std::span<const std::uint8_t> bytes,
                SDL_FRect destination);
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    void update();
    void draw() const;
    bool finished() const;
    void set_speed(double speed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
