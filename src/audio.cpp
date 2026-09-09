#include "audio.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace th2 {
namespace {

std::runtime_error ffmpeg_error(std::string_view action, int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return std::runtime_error(std::string(action) + ": " + buffer);
}

struct MemoryInput {
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
};

int read_packet(void* opaque, std::uint8_t* destination, int size)
{
    auto& input = *static_cast<MemoryInput*>(opaque);
    const auto available = input.bytes.size() - input.position;
    const auto copied = std::min<std::size_t>(available, static_cast<std::size_t>(size));
    if (copied == 0) {
        return AVERROR_EOF;
    }
    std::copy_n(input.bytes.data() + input.position, copied, destination);
    input.position += copied;
    return static_cast<int>(copied);
}

std::int64_t seek_packet(void* opaque, std::int64_t offset, int whence)
{
    auto& input = *static_cast<MemoryInput*>(opaque);
    if (whence == AVSEEK_SIZE) {
        return static_cast<std::int64_t>(input.bytes.size());
    }
    const int origin = whence & ~AVSEEK_FORCE;
    std::int64_t base = origin == SEEK_CUR
        ? static_cast<std::int64_t>(input.position)
        : origin == SEEK_END ? static_cast<std::int64_t>(input.bytes.size())
                             : 0;
    const auto next = base + offset;
    if (next < 0 || next > static_cast<std::int64_t>(input.bytes.size())) {
        return AVERROR(EINVAL);
    }
    input.position = static_cast<std::size_t>(next);
    return next;
}

AVCodecContext* open_codec(AVFormatContext* format, int stream)
{
    const auto* parameters = format->streams[stream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) {
        throw std::runtime_error("audio codec not available");
    }
    AVCodecContext* context = avcodec_alloc_context3(codec);
    if (!context) {
        throw std::bad_alloc();
    }
    int result = avcodec_parameters_to_context(context, parameters);
    if (result >= 0) {
        result = avcodec_open2(context, codec, nullptr);
    }
    if (result < 0) {
        avcodec_free_context(&context);
        throw ffmpeg_error("open audio codec", result);
    }
    return context;
}

void append_frame(
    AudioClip& clip, SwrContext* resampler, const AVFrame* frame, int channels)
{
    const int output_samples = av_rescale_rnd(
        swr_get_delay(resampler, frame->sample_rate) + frame->nb_samples,
        frame->sample_rate, frame->sample_rate, AV_ROUND_UP);
    if (output_samples <= 0) {
        return;
    }
    std::vector<float> buffer(
        static_cast<std::size_t>(output_samples) * static_cast<std::size_t>(channels));
    std::uint8_t* output[] = {
        reinterpret_cast<std::uint8_t*>(buffer.data())};
    const int converted = swr_convert(
        resampler, output, output_samples,
        const_cast<const std::uint8_t**>(frame->extended_data),
        frame->nb_samples);
    if (converted > 0) {
        const auto sample_count = static_cast<std::size_t>(converted) * channels;
        clip.samples.insert(
            clip.samples.end(), buffer.data(), buffer.data() + sample_count);
    }
}

}  // namespace

struct AudioDecoder::State {
    std::vector<std::uint8_t> bytes;
    MemoryInput input{};
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    int stream_index = -1;
    bool drained = false;

    ~State()
    {
        swr_free(&resampler);
        avcodec_free_context(&codec);
        if (format) {
            avformat_close_input(&format);
        }
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        av_packet_free(&packet);
        av_frame_free(&frame);
    }
};

AudioDecoder::AudioDecoder(std::vector<std::uint8_t> bytes)
    : state_(std::make_unique<State>())
{
    auto& state = *state_;
    state.bytes = std::move(bytes);
    // State is heap allocated and never moved, so the span stays valid.
    state.input.bytes = state.bytes;

    state.frame = av_frame_alloc();
    state.packet = av_packet_alloc();
    if (!state.frame || !state.packet) {
        throw std::bad_alloc();
    }
    auto* io_buffer = static_cast<std::uint8_t*>(av_malloc(64 * 1024));
    if (!io_buffer) {
        throw std::bad_alloc();
    }
    state.io = avio_alloc_context(
        io_buffer, 64 * 1024, 0, &state.input, read_packet, nullptr,
        seek_packet);
    state.format = avformat_alloc_context();
    if (!state.io || !state.format) {
        throw std::bad_alloc();
    }
    state.format->pb = state.io;
    state.format->flags |= AVFMT_FLAG_CUSTOM_IO;

    int result = avformat_open_input(&state.format, nullptr, nullptr, nullptr);
    if (result < 0) {
        throw ffmpeg_error("open audio", result);
    }
    result = avformat_find_stream_info(state.format, nullptr);
    if (result < 0) {
        throw ffmpeg_error("read audio streams", result);
    }
    state.stream_index = av_find_best_stream(
        state.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (state.stream_index < 0) {
        throw std::runtime_error("audio file has no audio stream");
    }
    state.codec = open_codec(state.format, state.stream_index);
    if (state.codec->sample_rate <= 0
        || state.codec->ch_layout.nb_channels <= 0) {
        throw std::runtime_error("invalid audio stream");
    }

    const int channels = state.codec->ch_layout.nb_channels;
    AVChannelLayout output_layout;
    av_channel_layout_default(&output_layout, channels);
    result = swr_alloc_set_opts2(
        &state.resampler, &output_layout, AV_SAMPLE_FMT_FLT,
        state.codec->sample_rate, &state.codec->ch_layout,
        state.codec->sample_fmt, state.codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&output_layout);
    if (result < 0 || swr_init(state.resampler) < 0) {
        throw std::runtime_error("cannot create audio resampler");
    }

    clip_.sample_rate = state.codec->sample_rate;
    clip_.channels = channels;
}

AudioDecoder::~AudioDecoder() = default;

bool AudioDecoder::decode_all()
{
    return decode(std::chrono::nanoseconds::max());
}

bool AudioDecoder::decode(std::chrono::nanoseconds budget)
{
    if (done_) {
        return true;
    }
    if (budget <= std::chrono::nanoseconds::zero()) {
        return false;  // Out of time, not unlimited.
    }
    auto& state = *state_;
    const auto started = std::chrono::steady_clock::now();
    const auto spent = [&] {
        return std::chrono::steady_clock::now() - started >= budget;
    };

    while (!state.drained) {
        if (av_read_frame(state.format, state.packet) < 0) {
            state.drained = true;
            break;
        }
        if (state.packet->stream_index == state.stream_index) {
            int result = avcodec_send_packet(state.codec, state.packet);
            if (result < 0 && result != AVERROR(EAGAIN)) {
                av_packet_unref(state.packet);
                throw ffmpeg_error("send audio packet", result);
            }
            while (result >= 0) {
                result = avcodec_receive_frame(state.codec, state.frame);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                    break;
                }
                if (result < 0) {
                    av_packet_unref(state.packet);
                    throw ffmpeg_error("receive audio frame", result);
                }
                append_frame(clip_, state.resampler, state.frame,
                             clip_.channels);
            }
        }
        av_packet_unref(state.packet);
        if (spent()) {
            return false;
        }
    }

    // Flush whatever the decoder still holds; this only runs once.
    int result = avcodec_send_packet(state.codec, nullptr);
    if (result < 0 && result != AVERROR_EOF) {
        throw ffmpeg_error("flush audio decoder", result);
    }
    while (true) {
        result = avcodec_receive_frame(state.codec, state.frame);
        if (result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            throw ffmpeg_error("receive flushed audio frame", result);
        }
        append_frame(clip_, state.resampler, state.frame, clip_.channels);
    }
    done_ = true;
    if (clip_.samples.empty()) {
        throw std::runtime_error("empty decoded audio");
    }
    return true;
}

AudioClip AudioDecoder::take()
{
    decode_all();
    return std::move(clip_);
}

AudioClip decode_audio(std::span<const std::uint8_t> bytes)
{
    AudioDecoder decoder{
        std::vector<std::uint8_t>(bytes.begin(), bytes.end())};
    return decoder.take();
}

AudioChannel::~AudioChannel()
{
    stop();
}

AudioChannel::AudioChannel(AudioChannel&& other) noexcept
    : stream_(std::exchange(other.stream_, nullptr)),
      source_(std::move(other.source_)),
      loop_source_(std::move(other.loop_source_)),
      queued_samples_(std::exchange(other.queued_samples_, 0)),
      clip_(std::move(other.clip_)), loop_clip_(std::move(other.loop_clip_)),
      loop_(other.loop_), active_(other.active_),
      gain_(other.gain_), fade_from_(other.fade_from_),
      fade_to_(other.fade_to_), fade_stop_(other.fade_stop_),
      fade_started_(std::move(other.fade_started_)),
      fade_duration_(other.fade_duration_),
      playback_end_(other.playback_end_),
      paused_at_(std::move(other.paused_at_))
{
    other.active_ = false;
    other.paused_at_.reset();
}

AudioChannel& AudioChannel::operator=(AudioChannel&& other) noexcept
{
    if (this != &other) {
        stop();
        stream_ = std::exchange(other.stream_, nullptr);
        source_ = std::move(other.source_);
        loop_source_ = std::move(other.loop_source_);
        queued_samples_ = std::exchange(other.queued_samples_, 0);
        clip_ = std::move(other.clip_);
        loop_clip_ = std::move(other.loop_clip_);
        loop_ = other.loop_;
        active_ = other.active_;
        gain_ = other.gain_;
        fade_from_ = other.fade_from_;
        fade_to_ = other.fade_to_;
        fade_stop_ = other.fade_stop_;
        fade_started_ = std::move(other.fade_started_);
        fade_duration_ = other.fade_duration_;
        playback_end_ = other.playback_end_;
        paused_at_ = std::move(other.paused_at_);
        other.active_ = false;
        other.paused_at_.reset();
    }
    return *this;
}

void AudioChannel::play(AudioClip clip, bool loop, float gain)
{
    stop();
    clip_ = std::move(clip);
    loop_ = loop;
    const SDL_AudioSpec spec{
        SDL_AUDIO_F32,
        clip_.channels,
        clip_.sample_rate,
    };
    stream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        throw std::runtime_error(SDL_GetError());
    }
    if (!SDL_SetAudioStreamGain(stream_, std::clamp(gain, 0.0f, 1.0f))) {
        throw std::runtime_error(SDL_GetError());
    }
    playback_end_ = std::chrono::steady_clock::now();
    queue();
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        throw std::runtime_error(SDL_GetError());
    }
    active_ = true;
    gain_ = std::clamp(gain, 0.0f, 1.0f);
    fade_started_.reset();
    fade_stop_ = false;
    paused_at_.reset();
}

void AudioChannel::play_streaming(
    std::shared_ptr<AudioDecoder> decoder, bool loop, float gain)
{
    if (!decoder) {
        throw std::runtime_error("streaming playback needs a decoder");
    }
    stop();
    source_ = std::move(decoder);
    loop_ = loop;
    const SDL_AudioSpec spec{
        SDL_AUDIO_F32,
        source_->channels(),
        source_->sample_rate(),
    };
    stream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        throw std::runtime_error(SDL_GetError());
    }
    if (!SDL_SetAudioStreamGain(stream_, std::clamp(gain, 0.0f, 1.0f))) {
        throw std::runtime_error(SDL_GetError());
    }
    playback_end_ = std::chrono::steady_clock::now();
    queue();
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        throw std::runtime_error(SDL_GetError());
    }
    active_ = true;
    gain_ = std::clamp(gain, 0.0f, 1.0f);
    fade_started_.reset();
    fade_stop_ = false;
    paused_at_.reset();
}

void AudioChannel::play_intro_loop(AudioClip intro, AudioClip loop, float gain)
{
    if (intro.sample_rate != loop.sample_rate || intro.channels != loop.channels) {
        throw std::runtime_error("BGM intro and loop formats do not match");
    }
    play(std::move(intro), false, gain);
    loop_clip_ = std::move(loop);
}

const AudioClip& AudioChannel::active_clip() const
{
    // A streaming channel plays out of its decoder's clip, which keeps
    // growing; everything else plays out of its own.
    return source_ ? source_->clip() : clip_;
}

void AudioChannel::play_intro_loop_streaming(
    std::shared_ptr<AudioDecoder> intro, std::shared_ptr<AudioDecoder> loop,
    float gain)
{
    if (!intro || !loop) {
        throw std::runtime_error("streaming playback needs a decoder");
    }
    if (intro->sample_rate() != loop->sample_rate()
        || intro->channels() != loop->channels()) {
        throw std::runtime_error("BGM intro and loop formats do not match");
    }
    auto body = std::move(loop);
    play_streaming(std::move(intro), false, gain);
    loop_source_ = std::move(body);
}

void AudioChannel::queue()
{
    const auto& clip = active_clip();
    if (!stream_ || clip.samples.size() <= queued_samples_) {
        return;
    }
    // SDL copies whatever it is handed, so handing it a whole decoded track
    // is a multi-megabyte memcpy on the frame that starts playback - which
    // is what read-ahead makes the common case.  A second of audio is far
    // more than the frame loop needs to stay ahead of the device, and
    // update() tops it up every frame.
    const auto chunk = static_cast<std::size_t>(
        std::max(1, clip.sample_rate)) * static_cast<std::size_t>(
            std::max(1, clip.channels));
    const auto count =
        std::min(clip.samples.size() - queued_samples_, chunk);
    if (!SDL_PutAudioStreamData(
            stream_, clip.samples.data() + queued_samples_,
            static_cast<int>(count * sizeof(float)))) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_FlushAudioStream(stream_);
    queued_samples_ += count;
    advance_playback_end(count);
}

void AudioChannel::stop()
{
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    source_.reset();
    loop_source_.reset();
    queued_samples_ = 0;
    clip_ = {};
    loop_clip_ = {};
    loop_ = false;
    active_ = false;
    fade_started_.reset();
    fade_stop_ = false;
    paused_at_.reset();
}

void AudioChannel::pause(bool paused)
{
    if (!stream_) {
        return;
    }
    if (paused) {
        SDL_PauseAudioStreamDevice(stream_);
        if (!paused_at_) {
            paused_at_ = std::chrono::steady_clock::now();
        }
    } else {
        SDL_ResumeAudioStreamDevice(stream_);
        if (paused_at_) {
            playback_end_ += std::chrono::steady_clock::now() - *paused_at_;
            paused_at_.reset();
        }
    }
}

void AudioChannel::set_gain(float gain)
{
    gain_ = std::clamp(gain, 0.0f, 1.0f);
    fade_started_.reset();
    fade_stop_ = false;
    if (stream_) {
        SDL_SetAudioStreamGain(stream_, gain_);
    }
}

void AudioChannel::fade_to(
    float gain, std::chrono::milliseconds duration, bool stop_after)
{
    if (!stream_ || duration <= std::chrono::milliseconds::zero()) {
        if (stop_after) {
            stop();
        } else {
            set_gain(gain);
        }
        return;
    }
    fade_from_ = gain_;
    fade_to_ = std::clamp(gain, 0.0f, 1.0f);
    fade_stop_ = stop_after;
    fade_duration_ = duration;
    fade_started_ = std::chrono::steady_clock::now();
}

void AudioChannel::finish_fade()
{
    if (!fade_started_) {
        return;
    }
    const auto target = fade_to_;
    const bool stop_after = fade_stop_;
    fade_started_.reset();
    fade_stop_ = false;
    if (stop_after) {
        stop();
    } else {
        set_gain(target);
    }
}

void AudioChannel::update()
{
    if (stream_ && fade_started_) {
        const auto elapsed = std::chrono::steady_clock::now() - *fade_started_;
        const float progress = std::clamp(
            std::chrono::duration<float>(elapsed).count()
                / std::chrono::duration<float>(fade_duration_).count(),
            0.0f, 1.0f);
        gain_ = fade_from_ + (fade_to_ - fade_from_) * progress;
        SDL_SetAudioStreamGain(stream_, gain_);
        if (progress >= 1.0f) {
            const bool stop_after = fade_stop_;
            fade_started_.reset();
            fade_stop_ = false;
            if (stop_after) {
                stop();
                return;
            }
        }
    }
    if (!stream_ || !active_ || paused_at_) {
        return;
    }
    // Hand the device whatever has been decoded since the last frame.  While
    // the decode is still running the track cannot have ended, however far
    // ahead of the decoder the clock has got.
    queue();
    if (source_ && !source_->done()) {
        return;
    }
    if (std::chrono::steady_clock::now() < playback_end_) {
        return;
    }
    if (loop_source_) {
        // The intro has played out; the body takes over as the looping
        // source, exactly as loop_clip_ does for a fully decoded track.
        source_ = std::move(loop_source_);
        loop_source_.reset();
        clip_ = {};
        loop_ = true;
        queued_samples_ = 0;
        queue();
    } else if (!loop_clip_.samples.empty()) {
        source_.reset();
        clip_ = std::move(loop_clip_);
        loop_ = true;
        queued_samples_ = 0;
        queue();
    } else if (loop_) {
        queued_samples_ = 0;
        queue();
    } else {
        active_ = false;
    }
}

bool AudioChannel::playing() const
{
    return stream_ && active_;
}

void AudioChannel::advance_playback_end(std::size_t samples)
{
    // Queued audio extends the end of playback rather than resetting it, so
    // topping a stream up mid-track does not make it look finished.
    const auto& clip = active_clip();
    const auto frames = clip.channels > 0
        ? samples / static_cast<std::size_t>(clip.channels)
        : 0;
    const auto duration = clip.sample_rate > 0
        ? std::chrono::duration<double>(
              static_cast<double>(frames) / clip.sample_rate)
        : std::chrono::duration<double>::zero();
    const auto now = std::chrono::steady_clock::now();
    const auto base = playback_end_ > now ? playback_end_ : now;
    playback_end_ = base
        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            duration);
}

bool AudioChannel::starving() const
{
    return stream_ && active_ && source_ && !source_->done();
}

}  // namespace th2
