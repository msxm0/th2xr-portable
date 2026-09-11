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
    // once there is nothing left.  A budget that has already run out does
    // nothing: an exhausted budget must not read as "no limit", or a caller
    // that is out of time ends up decoding the whole file.
    // Decodes until the budget is spent, the input ends, or the clip holds
    // target_samples - whichever comes first.  A target of zero means the
    // whole file.
    //
    // The target is what separates reading ahead from playing back.  Work
    // done on the chance a line will be reached only needs enough to start
    // instantly; the track actually playing needs to stay ahead of the
    // device and nothing else.  Decoding every speculative clip in full put
    // a 32MB track in a pool sized for guesses and evicted everything else
    // to make room.
    bool decode(std::chrono::nanoseconds budget,
                std::size_t target_samples = 0);
    // The whole of what is left, however long that takes.
    bool decode_all();
    bool done() const { return done_; }
    std::size_t decoded_samples() const { return clip_.samples.size(); }
    // For a caller that must not be cut short.  decode() stops on whichever
    // comes first, the sample target or the clock, so "no clock" has to be
    // spelled as a duration; this is that, named, rather than an arbitrary
    // large number at each call site.  It is never waited out - the target
    // is what ends the call.
    static constexpr std::chrono::nanoseconds unbudgeted =
        std::chrono::hours(24);

    // The format is known as soon as the decoder exists, so a stream can be
    // opened before a single sample has been decoded.
    int sample_rate() const { return clip_.sample_rate; }
    int channels() const { return clip_.channels; }
    const AudioClip& clip() const { return clip_; }
    AudioClip take();

private:
    void open_stream();
#ifdef __EMSCRIPTEN__
    // Ogg goes to the browser, which decodes it off this thread; what is left
    // on this one is a copy, sliced to the budget like everything else.
    bool decode_browser(std::chrono::nanoseconds budget,
                        std::size_t target_samples);
#endif
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
    // True once the window in front of the device has drained to the low
    // mark, so the caller knows to give this one decoding time before any
    // read-ahead.  Distinct from desired_samples(), which is the high mark:
    // filling to the same point that triggers the fill would mean topping up
    // every frame forever.
    bool starving() const;
    // How much decoded audio this channel wants to have in hand: what it has
    // already handed the device plus the high mark.  The track being played
    // streams on its own terms rather than out of the read-ahead pool.
    std::size_t desired_samples() const;
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
    // The window of decoded audio kept in front of the device.  It drains as
    // the device plays and is refilled only once it reaches the low mark, so
    // a track is decoded at roughly the rate it is heard rather than as fast
    // as frames go by.
    //
    // This is what bounds the work: a second is far more than the frame loop
    // needs to stay ahead - even a 60ms hitch is four frames of slack - and
    // the 200ms band above it means the refill happens a few times a second
    // rather than on every frame.
    static constexpr int window_low_ms = 1000;
    static constexpr int window_high_ms = 1200;
    // Samples of the active clip that correspond to those durations.
    std::size_t mark_samples(int milliseconds) const;
    // What the device still holds and has not played, in samples.  This is
    // the real headroom: samples handed over are gone from our buffer but
    // have not been heard yet, and it is the gap ahead of the *device* that
    // a dropout is measured against.
    std::size_t device_samples() const;

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
