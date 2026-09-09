#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace th2 {

struct AudioClip {
    int sample_rate;
    int channels;
    std::vector<float> samples;
};

AudioClip decode_audio(std::span<const std::uint8_t> bytes);

// Decoding a whole file in one go costs 100-200ms for a BGM track, and it
// happens on the thread that draws, at a scene change - which is exactly
// where it is most visible.  This walks the same decode in bounded steps, so
// the cost can be spread over frames or paid ahead of time while the player
// is reading.  It owns its input, because the decode outlives the read.
class AudioDecoder {
public:
    explicit AudioDecoder(std::vector<std::uint8_t> bytes);
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // Decodes until the budget is spent or the input ends, and returns true
    // once there is nothing left.  A zero budget decodes the rest in one go.
    bool decode(std::chrono::nanoseconds budget);
    bool done() const { return done_; }

    // The format is known as soon as the decoder exists, so a stream can be
    // opened before a single sample has been decoded.
    int sample_rate() const { return clip_.sample_rate; }
    int channels() const { return clip_.channels; }
    const AudioClip& clip() const { return clip_; }
    AudioClip take();

private:
    struct State;
    std::unique_ptr<State> state_;
    AudioClip clip_{};
    bool done_ = false;
};

class AudioChannel {
public:
    AudioChannel() = default;
    ~AudioChannel();

    AudioChannel(const AudioChannel&) = delete;
    AudioChannel& operator=(const AudioChannel&) = delete;

    AudioChannel(AudioChannel&& other) noexcept;
    AudioChannel& operator=(AudioChannel&& other) noexcept;

    void play(AudioClip clip, bool loop, float gain);
    void play_intro_loop(AudioClip intro, AudioClip loop, float gain);
    // Starts playing a decode that is still in progress, feeding the device
    // whatever has been decoded so far and topping it up from update().  The
    // decoder only needs to know its format, not to have finished.
    void play_streaming(std::shared_ptr<AudioDecoder> decoder, bool loop,
                        float gain);
    void play_intro_loop_streaming(std::shared_ptr<AudioDecoder> intro,
                                   std::shared_ptr<AudioDecoder> loop,
                                   float gain);
    // True while the channel is playing ahead of its own decoder, so the
    // caller knows to give this one decoding time before any read-ahead.
    bool starving() const;
    AudioDecoder* decoder() const { return source_.get(); }
    void stop();
    void pause(bool paused);
    void set_gain(float gain);
    void fade_to(float gain, std::chrono::milliseconds duration,
                 bool stop_after = false);
    void finish_fade();
    void update();
    bool playing() const;
    bool fading() const { return fade_started_.has_value(); }

private:
    SDL_AudioStream* stream_ = nullptr;
    std::shared_ptr<AudioDecoder> source_;
    std::shared_ptr<AudioDecoder> loop_source_;
    std::size_t queued_samples_ = 0;
    AudioClip clip_{};
    AudioClip loop_clip_{};
    bool loop_ = false;
    bool active_ = false;
    float gain_ = 1.0f;
    float fade_from_ = 1.0f;
    float fade_to_ = 1.0f;
    bool fade_stop_ = false;
    std::optional<std::chrono::steady_clock::time_point> fade_started_;
    std::chrono::milliseconds fade_duration_{0};
    std::chrono::steady_clock::time_point playback_end_{};
    std::optional<std::chrono::steady_clock::time_point> paused_at_;

    const AudioClip& active_clip() const;
    void queue();
    void advance_playback_end(std::size_t samples);
};

}  // namespace th2
