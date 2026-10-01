#include "game.hpp"

#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <imgui.h>
#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#define localtime_r(timep, result) localtime_s(result, timep)
#endif

namespace th2app {

void Game::begin_background_scroll(
    float x, float y, float width, float height, int frames, int type)
{
    // AVG_SetBackScroll.  back_max is AVG_EffCnt4, which is 30fps units and
    // carries no Avg.wait, so it doubles at sixty; the easing is sc_type%3
    // (linear, accelerating, decelerating) and sc_type/3 picks between a
    // moving window, a zoom, and a four-point warp.
    //
    // The warp is AVG_SetBackScrollPoly, sc_type 6..8, and is unreachable:
    // the only two opcodes that start a scroll are S, whose six uses all
    // pass type 0 or 1, and Z, whose three all pass 3.  So a scroll is
    // always a window or a zoom.
    background_scroll_ = BackgroundScroll{
        current_background_view(),
        {x, y, width, height},
        frames,
        type % 3,
        type / 3 == 1,
        std::chrono::steady_clock::now(),
    };
    // void AVG_SetBackScroll( x, y, w, h, frame, type ): sc_max keeps the
    // raw frame count, so AVG_EffCnt4 is re-asked on every pass of
    // AVG_ControlBackScroll and the skip key ends a pan already running.
    avgback().set_back_scroll(
        static_cast<int>(x), static_cast<int>(y),
        static_cast<int>(width), static_cast<int>(height), frames, type);
}

void Game::reload_character_textures()
{
    // AVG_LoadChar picks its tone curve out of BackStruct, so a tone change
    // means decoding every character again.
    chars().reload_all();
    background_baked_dirty_ = true;
}

void Game::apply_staged_characters()
{
    // AVG_SetBackReleaseChar(): the CW/CRW forms register a change without
    // showing it, and this is where it lands - the ones marked
    // CHAR_TYPE_WAIT2 are released, and AVG_SetBackChar turns the waiting
    // ones into CHAR_COND_NOMAL as it bakes them.
    chars().set_back_release_char();
    background_baked_dirty_ = true;
}

void Game::clear_characters()
{
    chars().init_char();
    background_baked_dirty_ = true;
}

std::size_t Game::character_index(int character_number) const
{
    if (character_number < 0
        || static_cast<std::size_t>(character_number)
            >= character_textures_.size()) {
        throw std::out_of_range(std::format(
            "invalid character number: {}", character_number));
    }
    return static_cast<std::size_t>(character_number);
}

std::vector<ToneCurveSpec> Game::effect_tone_curves(
    int tone, bool character)
{
    switch (tone / 4) {
    case 1: return {{"sepia.amp", 0}};
    case 2: return {{"nega.amp", 256}};
    case 3: return {{"", 0}};
    case 4: return {{"blue.amp", 128}};
    case 5: return {{"red.amp", 128}};
    case 6: return {{"green.amp", 128}};
    case 7: return {{"blue2.amp", 128}};
    case 8: return {{"brown.amp", 128}};
    case 9: return {{"sepia_half.amp", 128}};
    case 10: return {{"black.amp", character ? 0 : 256}};
    case 11: return {{"yoritomo.amp", character ? 0 : 256}};
    default: return {};
    }
}

std::string Game::base_tone_curve(int tone)
{
    switch (tone % 4) {
    case 1: return "evening.amp";
    case 2: return "night.amp";
    case 3: return "indoor.amp";
    default: return {};
    }
}

std::vector<ToneCurveSpec> Game::background_tone_curves() const
{
    const int tone = tone_back_ < 0 ? tone_ : tone_back_;
    return effect_tone_curves(tone, false);
}

std::vector<ToneCurveSpec> Game::character_tone_curves() const
{
    const int tone = tone_char_ < 0 ? tone_ : tone_char_;
    std::vector<ToneCurveSpec> result;
    if (tone_char_ < 0 && !background_tone_curve_.empty()) {
        result.push_back({background_tone_curve_, 256});
    } else if (const auto base = base_tone_curve(tone); !base.empty()) {
        result.push_back({base, 256});
    }
    if (weather_ != 0) {
        result.push_back({"rain.amp", 256});
    }
    auto effect = effect_tone_curves(tone, true);
    result.insert(result.end(), effect.begin(), effect.end());
    return result;
}

int Game::character_effect_frames(int frames) const
{
    // Character animations go through AVG_EffCnt() as well.
    return effect_frames(frames);
}

bool Game::character_animation_active() const
{
    return avg_char_ && chars().any_animating();
}

int Game::control_ticks_due()
{
    // MAIN_GameControl runs the AVG_Control* chain once a frame at sixty
    // frames a second, and every counter in it - CharStruct.cnt,
    // BackStruct.fd_cnt, HalfTone.tcount - is a whole number of those.  On a
    // display that refreshes faster the chain has to be stepped by elapsed
    // time instead, or every animation runs at the refresh rate rather than
    // at the speed the script asked for.
    if (trace_mode_) {
        // A tick is the frame.  No accumulator, no clamp, no clock - so a
        // frame that takes four hundred milliseconds to decode a background
        // still advances the engine by exactly one sixtieth.
        ++global_count_;
        return 1;
    }
    const auto now = std::chrono::steady_clock::now();
    if (character_control_time_ == std::chrono::steady_clock::time_point{}) {
        character_control_time_ = now;
    }
    character_control_debt_ += std::chrono::duration<double>(
        now - character_control_time_).count() * 60.0;
    character_control_time_ = now;
    // A long stall must not be made up all at once, or an animation jumps.
    character_control_debt_ = std::min(character_control_debt_, 8.0);
    const int steps = static_cast<int>(character_control_debt_);
    character_control_debt_ -= steps;
    global_count_ += steps;
    return steps;
}

void Game::update_character_animations(int steps)
{
    // AVG_ControlChar, run between the script and the draw.  It advances
    // every character's counters, bakes the settled ones into BMP_BACK, and
    // closes and reopens the message window around an animation.
    //
    // The original calls it once a frame at sixty frames a second and counts
    // in whole frames, so on a faster display it has to be stepped by
    // elapsed time instead - otherwise every character animation runs at the
    // refresh rate rather than at the speed the script asked for.
    const bool was_animating = character_animation_active();
    if (steps <= 0) {
        // No sixtieth has gone by, but the frame is about to be drawn and
        // the engine would have run AVG_ControlChar before it.  Everything
        // that decides what is on screen runs; only the counters wait.
        chars().control_char(false);
    }
    for (int i = 0; i < steps; ++i) {
        chars().control_char();
    }
    if (half_tone_copy_stale_) {
        half_tone_copy_stale_ = false;
        if (half_tone_armed_ && display().bmp_flag(th2::bmp_backhalf)) {
            build_half_tone_background();
        }
    }
    static_cast<void>(was_animating);
    // Nothing is resumed here.  The C instruction that started the animation
    // is still the current instruction - the program counter never moved -
    // and it asks AVG_WaitChar again at the top of the next frame.
}

// The decoder a channel is about to play from.  Read-ahead usually means it
// is already finished; when it is not, this pays a few milliseconds to get a
// buffer in front of the device rather than the whole track at once.
std::shared_ptr<th2::AudioDecoder> Game::ready_audio_decoder(
    const th2::Archive& archive, std::string_view name)
{
    // Not kept.  A track being played now is not a prediction, and the cache
    // it would go into is governed entirely by the explorer: entries live by
    // being named again on each scan, and the instruction that started this
    // one is behind the interpreter, so it can never be named again.  It
    // would be stamped with the current generation, fall behind on the very
    // next scan, and become the first thing evicted - while playing.  Worse,
    // a track the explorer missed altogether, which is exactly why this path
    // had to read it, would take a slot from one the explorer got right.
    //
    // The channel holds the decoder for as long as it plays, so nothing is
    // lost by leaving it out; the cost is a re-decode if the same sound is
    // played again after the channel lets go.
    auto decoder = audio_decoder(archive, name, 0, false);
    if (!decoder->done()) {
        decoder->decode(std::chrono::milliseconds(3));
    }
    return decoder;
}

std::shared_ptr<th2::AudioDecoder> Game::audio_decoder(
    const th2::Archive& archive, std::string_view name, int rank, bool keep)
{
    std::string key(name);
    if (auto* held = audio_decoders_.find(key)) {
        return *held;
    }
    const auto* entry = archive.find(name);
    if (!entry) {
        throw std::runtime_error("audio not found: " + key);
    }
    if (!keep) {
        note_asset_use(key);
    }
    // Blocking on a whole track freezes everything - the read suspends the
    // wasm stack, so nothing draws until it lands.  Take the first 64KB
    // instead, which is about two and a half seconds of Vorbis and one round
    // trip, and collect the rest while it plays.  read_head() falls back to
    // the whole entry when splitting would not pay.
    auto split = archive.read_head(*entry, audio_head_bytes);
    auto decoder = std::make_shared<th2::AudioDecoder>(
        std::move(split.bytes), split.ready);
    if (decoder->awaiting_rest()) {
        pending_audio_rest_.push_back(
            PendingAudioRest{&archive, entry, decoder, split.ready});
    }
    // Held only if this is read-ahead, and then only if there is room.  A
    // decoder the cache refuses still works - the caller keeps the
    // shared_ptr - it simply will not be found again, which for read-ahead
    // means it is decoded when it is played instead of before.
    if (keep) {
        audio_decoders_.insert(key, decoder, scan_generation_, rank);
    }
    return decoder;
}

// The decoder a channel is about to play from.  Read-ahead usually means it
// is already finished; when it is not, this pays a few milliseconds to get a
// buffer in front of the device rather than the whole track at once.

void Game::collect_audio_rests()
{
    for (auto at = pending_audio_rest_.begin();
         at != pending_audio_rest_.end();) {
        auto decoder = at->decoder.lock();
        if (!decoder || !decoder->awaiting_rest()) {
            at = pending_audio_rest_.erase(at);
            continue;
        }
        if (!at->archive->resident_rest(*at->entry, at->ready)) {
            ++at;  // Still on the wire; nothing here blocks on it.
            continue;
        }
        // Resident, so this is a copy rather than a wait - the whole point
        // is that the frame never blocks a second time.
        try {
            at->archive->read_rest_into(
                *at->entry, at->ready, decoder->rest_buffer());
            decoder->supply_rest();
        } catch (const std::exception& error) {
            SDL_Log("audio tail failed: %s", error.what());
        }
        at = pending_audio_rest_.erase(at);
    }
}

void Game::update_audio_decode()
{
    collect_audio_rests();
    // Drawn from the frame's shared allowance rather than a private two
    // milliseconds, so decoding ahead cannot pile on top of a picture being
    // decoded ahead and hand the frame more than either of them intended.
    const auto remaining = [&] {
        return background_budget_.remaining();
    };

    // A track that is playing is not read-ahead and is not the pre-decoder's
    // business: it has no budget, takes nothing from the frame's allowance,
    // and is never deferred.  Losing a frame of read-ahead costs a decode
    // later; losing a frame here is a hole in the sound.
    //
    // Below a second of headroom it is topped up outright, however long that
    // takes.  Above that it is still filled to the five-second target, which
    // is what keeps the unbudgeted case rare: by the time a frame is busy
    // enough to matter, the buffer is already seconds deep.  The target
    // bounds the work either way - a channel five seconds ahead asks for
    // nothing.
    const auto feed = [&](th2::AudioChannel& channel) {
        if (!channel.starving()) {
            return;
        }
        channel.decoder()->decode(
            th2::AudioDecoder::unbudgeted, channel.desired_samples());
    };
    feed(bgm_);
    for (auto& channel : voice_channels_) {
        feed(channel);
    }
    for (auto& channel : se_channels_) {
        feed(channel);
    }
    for (auto& channel : transient_se_) {
        feed(channel);
    }

    // Then read-ahead, in the order the scan reached them: imminent before
    // branch, and within a rank the one the script reaches first.
    std::ranges::sort(
        audio_decode_queue_, [](const auto& left, const auto& right) {
            return std::tie(left.rank, left.order)
                < std::tie(right.rank, right.order);
        });
    // Work the queue in order - nearest first - one piece at a time, until
    // the frame's allowance runs out or everything the scan asked for is
    // decoded.
    while (!audio_decode_queue_.empty()
           && remaining() > std::chrono::nanoseconds::zero()) {
        auto& request = audio_decode_queue_.front();
        const auto* entry = request.archive->find(request.name);
        if (!entry) {
            audio_decode_queue_.erase(audio_decode_queue_.begin());
            continue;
        }
        // Never off the network.  Read-ahead exists to get work done early,
        // not to make the frame wait: reading bytes that have not arrived
        // suspends the wasm stack, which would turn a decode nobody is
        // waiting for into a stall everybody feels.  If the bytes are not in
        // a cache yet, the range is already on its way and a later scan will
        // pick this up.
        if (!request.archive->resident(*entry)) {
            audio_decode_queue_.erase(audio_decode_queue_.begin());
            continue;
        }
        try {
            const bool held = audio_decoders_.contains(request.name);
            if (!held) {
                // Opening builds the stream's VLC tables and cannot be cut
                // short, so it is rationed by count rather than by clock:
                // one a frame keeps a six-millisecond atomic step from
                // landing twice on the same one.
                if (audio_opens_this_frame_ >= audio_opens_per_frame) {
                    break;
                }
                ++audio_opens_this_frame_;
                if (th2app::trace_prefetch) {
                    ++trace_.audio_created;
                    if (++audio_seen_[request.name] > 1) {
                        ++trace_.audio_recreated;
                    }
                }
            }
            auto decoder = background_budget_.spend([&] {
                // Read-ahead: this one does belong in the cache the
                // explorer governs, because the explorer is what named
                // it and what will keep naming it while it stays ahead.
                return audio_decoder(
                    *request.archive, request.name, request.rank, true);
            });
            // Two seconds, not the whole file.  Read-ahead exists so
            // playback can start instantly and stay ahead through the frames
            // it takes the streaming window to take over; the rest arrives
            // while it plays.
            const auto preroll = 2 * static_cast<std::size_t>(
                std::max(1, decoder->sample_rate()))
                * static_cast<std::size_t>(std::max(1, decoder->channels()));
            if (background_budget_.spend([&] {
                    return decoder->decode(remaining(), preroll);
                })) {
                audio_decode_queue_.erase(audio_decode_queue_.begin());
            } else {
                break;  // Out of time; the rest of this one waits a frame.
            }
        } catch (const std::exception&) {
            // A file that will not decode is not worth retrying every frame.
            audio_decode_queue_.erase(audio_decode_queue_.begin());
        }
    }
}



void Game::play_se(int channel, int sound, bool loop, int volume, int fade,
             bool wait_for_completion)
{
    const auto name = std::format("SE_{:04d}.WAV", sound);
    if (channel >= 0 && static_cast<std::size_t>(channel) < se_channels_.size()) {
        trace_note_audio("se2", channel, sound, loop ? 1 : 0, volume);
        note_sound_started(static_cast<std::size_t>(channel));
        se_channels_[channel].play_streaming(
            ready_audio_decoder(se_archive_, name), loop,
            fade > 0 ? 0.0f : se_gain(volume));
        if (fade > 0) {
            se_channels_[channel].fade_to(
                se_gain(volume),
                audio_fade_duration(fade));
        }
        se_sound_[channel] = sound;
        se_loop_[channel] = loop;
        se_volume_[channel] = volume;
        return;
    }
    auto found = std::find_if(
        transient_se_.begin(), transient_se_.end(),
        [](const th2::AudioChannel& audio) { return !audio.playing(); });
    if (found == transient_se_.end()) {
        found = transient_se_.begin();
    }
    const auto index = static_cast<std::size_t>(
        std::distance(transient_se_.begin(), found));
    transient_se_volume_[index] = volume;
    trace_note_audio("se", -1, sound, loop ? 1 : 0, volume);
    note_sound_started(se_channels_.size() + index);
    found->play_streaming(
        ready_audio_decoder(se_archive_, name), false, se_gain(volume));
    if (wait_for_completion) {
        audio_wait_ = AudioWait{
            AudioWaitKind::sound_effect, se_channels_.size() + index};
    }
}

void Game::sync_game_flags()
{
    const auto flags = runtime_.all_game_flags();
    if (std::ranges::equal(flags, persistent_game_flags_)) {
        return;
    }
    std::ranges::copy(flags, persistent_game_flags_.begin());
    persistent_state_.save_game_flags(persistent_game_flags_);
}

void Game::stop_bgm(int fade)
{
    trace_note_audio("bgmstop", -1, -1, fade, 0);
    if (fade <= 0) {
        bgm_.stop();
    } else {
        bgm_.fade_to(0.0f, audio_fade_duration(fade), true);
    }
    bgm_track_ = -1;
}

// for(i=0;i<WAVE_SOUND_NUM;i++) AVG_StopSE2( i, fade ) - AVG_FadeSeAll and
// the title return's loop.  WAVE_SOUND_NUM is 12; ours keeps more channels,
// and the ones past it are not the engine's to stop.
void Game::stop_all_se(int fade)
{
    constexpr int wave_sound_num = 12;
    for (int channel = 0; channel < wave_sound_num
         && static_cast<std::size_t>(channel) < se_channels_.size(); ++channel) {
        trace_note_audio("sestop", channel, -1, fade, 0);
        se_channels_[channel].fade_to(
            0.0f, audio_fade_duration(std::max(0, fade)), true);
        se_sound_[channel] = -1;
    }
}

void Game::play_bgm(int music, bool loop, int volume, int fade, bool change)
{
    trace_note_audio("bgm", -1, music, loop ? 1 : 0, volume);
    // AVG_PlayBGMEx opens with the negative case and the unchanged case:
    //     if( mus_no<0 ){ AVG_StopBGM( fade ); return; }
    //     if( !change ){ if( PlayMusicNo==mus_no ) return; }
    // `change` is what lets AVG_SetMovie restart a track already playing.
    if (music < 0) {
        stop_bgm(fade);
        return;
    }
    if (!change && bgm_track_ == music) {
        return;
    }
    static constexpr std::array music_room_tracks{
        0, 10, 29, 11, 12, 13, 14, 30, 27, 1,
        2, 4, 3, 5, 6, 8, 7, 9, 18, 37,
        38, 41, 42, 39, 40, 15, 16, 17, 19, 20,
        22, 32, 21, 23, 26, 31, 25, 24, 28, 50,
    };
    const auto music_slot =
        std::ranges::find(music_room_tracks, music);
    if (music_slot != music_room_tracks.end()) {
        runtime_.set_game_flag(
            128 + static_cast<std::size_t>(
                std::distance(music_room_tracks.begin(), music_slot)),
            1);
        sync_game_flags();
    }
    bgm_track_ = music;
    bgm_loop_ = loop;
    bgm_volume_ = volume;
    const auto gain = bgm_gain(volume);
    const auto single = std::format("BGM_{:03d}.OGG", music);
    if (bgm_archive_.find(single)) {
        bgm_.play_streaming(
            ready_audio_decoder(bgm_archive_, single), loop, gain);
        return;
    }
    const auto intro = std::format("BGM_{:03d}_A.OGG", music);
    const auto body = std::format("BGM_{:03d}_B.OGG", music);
    if (!bgm_archive_.find(intro) || !bgm_archive_.find(body)) {
        throw std::runtime_error("BGM track not found: " + std::to_string(music));
    }
    if (loop) {
        bgm_.play_intro_loop_streaming(
            ready_audio_decoder(bgm_archive_, intro),
            ready_audio_decoder(bgm_archive_, body), gain);
    } else {
        bgm_.play_streaming(
            ready_audio_decoder(bgm_archive_, intro), false, gain);
    }
}

void Game::play_voice(const th2::Event& event)
{
    const int script_character = number(event, 0);
    const int volume = number(event, 1) < 0
        ? (event.instruction.name == "VV" ? 256 : 255)
        : number(event, 1);
    const bool loop = number(event, 2) > 0;
    const int voice = number(event, 3);
    const int channel = number(event, 4) < 0 ? 0 : number(event, 4);
    // Where the reference's probe sits: first thing in AVG_PlayVoice, with
    // the arguments as they arrived - before the cno>=10 -> 99 remap and
    // before any of the ways the voice turns out not to play.
    trace_note_audio("voice", channel, script_character,
                     scenario_number(runtime_.script_name()), voice);
    int character = script_character;
    if (character >= 10 && character != 28) {
        character = 99;
    }
    if (channel < 0
        || static_cast<std::size_t>(channel) >= voice_channels_.size()) {
        return;
    }
    int scenario = scenario_number(runtime_.script_name());
    if (vi_event_voice_no_all_ >= 0) {
        scenario = vi_event_voice_no_all_;
    } else if (vi_event_voice_no_ >= 0) {
        scenario = scenario / 100 * 100 + vi_event_voice_no_;
    }
    // if(test==0) SetNovelMessageVoice1( sno, vno, cno, a_cut ): the log
    // entry the next message makes remembers this voice, whether or not it
    // turns out to play.  VC is the a_cut form.
    msg().set_novel_message_voice1(
        scenario, voice, character, event.instruction.name == "VC" ? 1 : 0);
    const auto standard_name = std::format(
        "K{:09d}_{:03d}{:03d}.OGG", scenario, voice, character);
    auto name = standard_name;
    auto& voice_channel = voice_channels_[channel];
    voice_channel.stop();
    //     if( skip_voice || Avg.voice==0
    //         || (AVG_GetMesCut() && Avg.demo==0 && test==0) ) return;
    // A line being skipped is not voiced at all - AVG_StopVoice above has
    // already cut whatever was playing, and nothing new starts.
    if (message_cut() && !demo_mode_) {
        voice_sound_[channel] = -1;
        voice_loop_[channel] = false;
        return;
    }
    if (runtime_.flag(5) == 0) {
        const auto alternate_name = std::format(
            "K{:09d}_{:03d}{:03d}A.OGG",
            scenario, voice, character);
        if (voice_archive_.find(alternate_name)) {
            name = alternate_name;
        } else if (event.instruction.name == "VC") {
            voice_sound_[channel] = -1;
            voice_loop_[channel] = false;
            return;
        }
    }
    const bool alternate = name != standard_name;
    pending_backlog_voice_ = BacklogVoice{
        0, 0, scenario, voice, character, volume, alternate};
    const auto* voice_entry = voice_archive_.find(name);
    if (!voice_entry) {
        voice_sound_[channel] = -1;
        voice_loop_[channel] = false;
        return;
    }
    trace_note_audio("voicefile", 0, loop ? 1 : 0, volume, 0, name.c_str());
    voice_channel.play_streaming(
        ready_audio_decoder(voice_archive_, voice_entry->name), loop,
        voice_gain(volume, character));
    voice_sound_[channel] = voice;
    voice_character_[channel] = character;
    voice_scenario_[channel] = scenario;
    voice_volume_[channel] = volume;
    voice_loop_[channel] = loop;
}

void Game::play_log_voice(int character, int scenario, int voice, bool a_cut)
{
    // AVG_PlayVoice( 0, cno, sno, vno, 255, 0, a_cut, 1 ), from a click on a
    // voiced line in the engine's log.  test==1, so nothing is logged again.
    trace_note_audio("voice", 0, character, scenario, voice);
    auto& voice_channel = voice_channels_[0];
    voice_channel.stop();
    const auto standard_name = std::format(
        "K{:09d}_{:03d}{:03d}.OGG", scenario, voice, character);
    auto name = standard_name;
    if (runtime_.flag(5) == 0) {
        const auto alternate_name = std::format(
            "K{:09d}_{:03d}{:03d}A.OGG", scenario, voice, character);
        if (voice_archive_.find(alternate_name)) {
            name = alternate_name;
        } else if (a_cut) {
            voice_sound_[0] = -1;
            voice_loop_[0] = false;
            return;
        }
    }
    const auto* voice_entry = voice_archive_.find(name);
    if (!voice_entry) {
        voice_sound_[0] = -1;
        voice_loop_[0] = false;
        return;
    }
    trace_note_audio("voicefile", 0, 0, 255, 0, name.c_str());
    voice_channel.play_streaming(
        ready_audio_decoder(voice_archive_, voice_entry->name), false,
        voice_gain(255, character));
    voice_sound_[0] = voice;
    voice_character_[0] = character;
    voice_scenario_[0] = scenario;
    voice_volume_[0] = 255;
    voice_loop_[0] = false;
}

void Game::replay_backlog_voice(const Game::BacklogVoice& voice)
{
    auto name = std::format(
        "K{:09d}_{:03d}{:03d}{}.OGG",
        voice.scenario, voice.voice, voice.character,
        voice.alternate ? "A" : "");
    if (!voice_archive_.find(name) && voice.alternate) {
        name = std::format(
            "K{:09d}_{:03d}{:03d}.OGG",
            voice.scenario, voice.voice, voice.character);
    }
    if (!voice_archive_.find(name)) {
        return;
    }
    voice_channels_[0].stop();
    note_sound_started(se_channels_.size() + transient_se_.size());
    voice_channels_[0].play_streaming(
        ready_audio_decoder(voice_archive_, name), false,
        voice_gain(voice.volume, voice.character));
    voice_sound_[0] = voice.voice;
    voice_character_[0] = voice.character;
    voice_scenario_[0] = voice.scenario;
    voice_volume_[0] = voice.volume;
    voice_loop_[0] = false;
}

void Game::update_audio()
{
    bgm_.update();
    for (auto& channel : transient_se_) {
        channel.update();
    }
    for (auto& channel : se_channels_) {
        channel.update();
    }
    for (auto& channel : voice_channels_) {
        channel.update();
    }
    // After the script pass: the SE and voice waits.  See
    // Game::refresh_audio_wait for why the BGM wait is not among them.
    refresh_audio_wait(false);
}

// Re-ask whether the sound being waited on has finished.
//
// Split out and called immediately before the script pass, because that is
// when the engine asks.  MW and its siblings are ESC_WAIT opcodes: they park
// on the instruction and re-evaluate their own predicate inside
// EXEC_ControlLang every frame, so the frame the sound ends is the frame the
// script moves on.  Ours used to clear the wait in update_audio(), which
// runs after pump_script(), so the script did not see it until the frame
// after - one tick late on every single MW in the game.  It cost exactly one
// tick at pc 4041 of 010301000.sdt, which is how it was found.
void Game::refresh_audio_wait(bool before_script)
{
    // Which waits clear before the script runs and which after is not a
    // detail - the two differ by a frame, and the engine is not consistent
    // about it:
    //
    //   SEW  -> AVG_WaitSe (GM_Avg.cpp:2272), which does not look at the
    //           sound at all: it goes through SeStruct[sno], and the script
    //           sees the wait a frame late.  Where exactly the flag is
    //           dropped is NOT identified - SeStruct[].flag is set in
    //           AVG_PlaySE2 and never cleared anywhere in GM_Avg.cpp - so
    //           the one-frame lag is measured, not traced to a line.
    //   MW   -> AVG_WaitBGM (GM_Avg.cpp:2048), asked by the opcode itself
    //           inside EXEC_ControlLang (main.cpp:267), so it clears the
    //           same frame.
    //
    // Treating both the same way is wrong whichever way you pick: one costs
    // a tick at pc 4041 of 010301000.sdt, the other gains one at pc 547.
    if (audio_wait_
        && (audio_wait_->kind == AudioWaitKind::bgm) != before_script) {
        return;
    }
    if (audio_wait_) {
        if (trace_mode_) {
            // From the tick the sound started, which is what
            // th2ref_pcm_play records on the other side - not from the tick
            // something began waiting for it.
            const auto found = trace_sound_started_.find(audio_wait_->channel);
            // Nothing recorded means nothing ever started on that channel, and
            // the reference agrees loudly: th2ref_pcm_status answers PCM_STOP
            // for a handle it has never seen, so the wait clears at once.
            // Dating it from `trace_tick_` instead made `started` today's tick
            // every time it was asked, so the difference below was forever
            // zero and the wait never cleared - an infinite hang written as an
            // expression, waiting for the first script to wait on a channel
            // nothing had played.
            const bool ever_started = found != trace_sound_started_.end();
            if (!ever_started
                || trace_tick_ - found->second >= trace_sound_ticks) {
                const auto started = ever_started ? found->second : trace_tick_;
                if (SDL_getenv("TH2_AUDIO_LOG")) {
                    SDL_Log("audio: wait ch=%zu started %llu cleared at %llu",
                            audio_wait_->channel,
                            static_cast<unsigned long long>(started),
                            static_cast<unsigned long long>(trace_tick_));
                }
                audio_wait_.reset();
            }
        } else {
            const auto& channel = waited_audio_channel();
            const bool complete = audio_wait_->kind == AudioWaitKind::bgm
                ? !channel.fading()
                : !channel.playing();
            if (complete) {
                audio_wait_.reset();
            }
        }
    }
}

bool Game::trace_se_playing(std::size_t channel) const
{
    const auto found = trace_sound_started_.find(channel);
    if (found == trace_sound_started_.end()) {
        return false;
    }
    // <=, not <.  The reference's sound is our own stub: th2ref_pcm_status
    // reports playing while g_tick - start < TH2REF_PCM_TICKS (50), so the
    // stub stops one tick sooner than this does, and AVG_WaitSe then sees
    // the wait a frame later still.  Measured: with <, SEW at offset 1305 of
    // 070000300.sdt fell through a frame before the reference's did.
    return trace_tick_ - found->second <= trace_sound_ticks;
}

void Game::trace_note_audio(const char* kind, int a, int b, int c, int d,
                            const char* name)
{
    if (!trace_mode_) {
        return;
    }
    static std::FILE* log = [] {
        const char* path = SDL_getenv("TH2_AUDIO_LOG");
        return path ? std::fopen(path, "a") : nullptr;
    }();
    if (!log) {
        return;
    }
    std::fprintf(log, "%llu %s %d %d %d %d %s\n",
                 static_cast<unsigned long long>(trace_tick_), kind,
                 a, b, c, d, name ? name : "-");
    std::fflush(log);
}

void Game::note_sound_started(std::size_t channel)
{
    if (trace_mode_) {
        trace_sound_started_[channel] = trace_tick_;
        if (SDL_getenv("TH2_AUDIO_LOG")) {
            SDL_Log("audio: start ch=%zu at tick %llu", channel,
                    static_cast<unsigned long long>(trace_tick_));
        }
    }
}

void Game::load_background_bitmap(Texture texture)
{
    // AVG_SetBack:
    //     DSP_LoadBmp( BMP_BACK, ... );
    //     DSP_CopyBmp( BMP_BACK2, BMP_BACK );
    // The picture arrives in BMP_BACK and BMP_BACK2 is the clean copy taken
    // before any character is composited in - so the plate can always be
    // put back to it.
    background_baked_dirty_ = true;
    if (!texture) {
        display().release_bmp(th2::bmp_back);
        display().release_bmp(th2::bmp_back2);
        return;
    }
    // AVG_SetBack drops the whole scroll block when a picture arrives, and
    // after the "was it found" return above rather than before it: the flag,
    // the type, the sx/sy chains, and sc_cnt / sc_max.  Only the flag was
    // being cleared, and only in AvgBack::begin_back, which nothing calls -
    // so a Z scroll left its counters behind for good.  After the scroll at
    // pc 5943 of 040415000.sdt ours held sc_cnt 60 / sc_max 30 for the rest
    // of the run where the reference zeroed both as the next picture landed.
    // sh is not in the engine's list either.
    auto& bk = back();
    bk.sc_flag = 0;
    bk.sc_type = 0;
    bk.sw = 0;
    bk.sx = bk.sx2 = bk.sx3 = bk.sx4 = 0;
    bk.sy = bk.sy2 = bk.sy3 = bk.sy4 = 0;
    bk.sc_cnt = 0;
    bk.sc_max = 0;
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(texture.get(), &width, &height);
    display().set_bmp(
        th2::bmp_back, std::move(texture), static_cast<int>(width),
        static_cast<int>(height));
    display().copy_bmp(th2::bmp_back2, th2::bmp_back);
    // ...then BackStruct.zoom = 0 and AVG_SetBackPos( x, y ), which puts
    // w/h back to the screen.  Without it they kept the last zoom scroll's
    // end window, and the next Z interpolated from a frame the new picture
    // never had: two zooms in a row (040415000.sdt, pc 5943 and 6397) and
    // the second one started from 700x525 instead of 800x600.
    if (avg_back_) {
        bk.zoom = 0;
        avgback().set_back_pos(static_cast<int>(background_view_.x),
                               static_cast<int>(background_view_.y));
    }
}

void Game::set_bg_scene(int scene)
{
    bg_scene_ = scene;
    // BackStruct.bno, which AVG_SetBack sets from the same value.  Kept in
    // step here rather than left to the one caller that happened to need it:
    // an unset bno made AVG_ResetBackHalfTone's early-out compare against
    // zero forever, and showed up in a state trace as a background the
    // engine had loaded and we had not.
    if (avg_back_) {
        // Only on the way in.  Clearing the background is BackStruct.flag
        // going to zero in the original; bno keeps whatever it last held,
        // which is exactly what AVG_ResetBackHalfTone's early-out wants to
        // compare the next scene against.  Writing -1 here would make that
        // comparison always fail and re-run a change the engine skips.
        back().flag = scene >= 0 ? 1 : 0;
        if (scene >= 0) {
            back().bno = scene;
        }
    }
}

void Game::set_background(const th2::Event& event, bool keep_characters)
{
    if (number(event, 1) < 0) {
        display().release_bmp(th2::bmp_back);
        display().release_bmp(th2::bmp_back2);
        background_baked_dirty_ = true;
        set_bg_scene(-1);
        background_kind_ = BackgroundKind::background;
        background_tone_curve_.clear();
        background_view_ = {0.0f, 0.0f, 800.0f, 600.0f};
        background_scroll_.reset();
        return;
    }
    int scene = number(event, 1) * 10
        + std::max<std::int32_t>(0, number(event, 2));
    scene = seasonal_background_scene(scene);
    set_bg_scene(scene);
    background_kind_ = BackgroundKind::background;
    background_view_ = {
        static_cast<float>(std::max(0, number(event, 4))),
        static_cast<float>(std::max(0, number(event, 5))),
        800.0f,
        600.0f,
    };
    background_scroll_.reset();
    update_background_sakura(scene, true);
    if (keep_characters) {
        apply_staged_characters();
    } else {
        clear_characters();
    }
    const auto name = std::format(
        "B{:03d}{}{}{}.bmp", scene / 10,
        (tone_back_ < 0 ? tone_ : tone_back_) % 4,
        weather_, scene % 10);
    const auto curve_name =
        std::filesystem::path(name).replace_extension(".amp").string();
    background_tone_curve_ =
        graphics_.find(curve_name) ? curve_name : std::string{};
    // Decoded ahead if the scan saw it coming - the LZS unpack of an 800x600
    // background was a 10-25 ms frame at every scene change.
    load_background_bitmap(load_toned_texture(
        renderer_, backgrounds_, name, graphics_,
        background_tone_curves(), nullptr,
        take_predecoded_image(true, name)));
    if (keep_characters) {
        reload_character_textures();
    }
}

void Game::set_cg(
    const th2::Event& event, BackgroundKind kind, char prefix)
{
    int visual = number(event, 1) * 10;
    if (number(event, 2) >= 0) {
        visual += number(event, 2);
    }
    set_bg_scene(visual);
    background_kind_ = kind;
    background_view_ = {
        static_cast<float>(std::max(0, number(event, 5))),
        static_cast<float>(std::max(0, number(event, 6))),
        800.0f,
        600.0f,
    };
    background_scroll_.reset();
    update_background_sakura(visual, false);
    const auto cg = std::format("{}{:06d}.tga", prefix, visual);
    load_background_bitmap(load_toned_texture(
        renderer_, graphics_, cg, graphics_, background_tone_curves(),
        nullptr, take_predecoded_image(false, cg)));
    auto& unlocked = kind == BackgroundKind::visual
        ? unlocked_visual_cgs_ : unlocked_h_cgs_;
    if (unlocked.emplace(visual).second) {
        persistent_state_.unlock(
            kind == BackgroundKind::visual
                ? th2::PersistentState::UnlockKind::visual_cg
                : th2::PersistentState::UnlockKind::h_cg,
            visual);
    }
    const bool keep_characters = number(event, 4) > 0;
    if (keep_characters) {
        apply_staged_characters();
        reload_character_textures();
    } else {
        clear_characters();
    }
}

void Game::restore_background()
{
    if (bg_scene_ < 0) {
        return;
    }
    if (background_kind_ != BackgroundKind::background) {
        const char prefix =
            background_kind_ == BackgroundKind::visual ? 'v' : 'h';
        load_background_bitmap(load_toned_texture(
            renderer_, graphics_,
            std::format("{}{:06d}.tga", prefix, bg_scene_),
            graphics_, background_tone_curves()));
    } else {
        const auto name = std::format(
            "B{:03d}{}{}{}.bmp", bg_scene_ / 10,
            (tone_back_ < 0 ? tone_ : tone_back_) % 4,
            weather_, bg_scene_ % 10);
        const auto curve_name =
            std::filesystem::path(name).replace_extension(".amp").string();
        background_tone_curve_ =
            graphics_.find(curve_name) ? curve_name : std::string{};
        load_background_bitmap(load_toned_texture(
            renderer_, backgrounds_, name, graphics_,
            background_tone_curves()));
    }
    reload_character_textures();
}

std::optional<std::size_t> Game::overlay_index(int requested) const
{
    if (requested == -1) {
        return overlay_states_.size() - 1;
    }
    if (requested < 0
        || static_cast<std::size_t>(requested) >= overlay_states_.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(requested);
}

bool Game::note_asset_use(const std::string& key)
{
    const auto found = named_recently_.find(key);
    const bool predicted = found != named_recently_.end()
        && scan_generation_ - found->second <= named_recently_generations;
    if (!predicted && th2app::trace_prefetch) {
        ++trace_.unpredicted_uses;
        SDL_Log("unpredicted: %s", key.c_str());
    }
    return !predicted;
}

void Game::report_prefetch_trace()
{
    if (!th2app::trace_prefetch) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_trace_report_.time_since_epoch().count() != 0
        && now - last_trace_report_ < std::chrono::seconds(10)) {
        return;
    }
    last_trace_report_ = now;
    SDL_Log("prefetch: audio created %d (%d of them a second time); "
            "images queued %d decoded %d used %d evicted-unused %d; "
            "held: %zu audio, %zu images",
            trace_.audio_created, trace_.audio_recreated,
            trace_.image_queued, trace_.image_decoded, trace_.image_used,
            trace_.image_evicted_unused,
            audio_decoders_.size(), decoded_images_.size());
    const auto& reads = th2::data_cache_stats();
    SDL_Log("reads: %llu total = pinned %llu + lru %llu (+%llu assembled) + "
            "store %llu + BLOCKED %llu (%lluKB); scan named %zu assets, "
            "%d unpredicted uses",
            static_cast<unsigned long long>(reads.reads),
            static_cast<unsigned long long>(reads.pinned_hits),
            static_cast<unsigned long long>(reads.lru_exact),
            static_cast<unsigned long long>(reads.lru_assembled),
            static_cast<unsigned long long>(reads.store_hits),
            static_cast<unsigned long long>(reads.blocking),
            static_cast<unsigned long long>(reads.blocking_bytes >> 10),
            plan_.size(), trace_.unpredicted_uses);
}

void Game::update_image_decode()
{
    // Never more than the frame's remaining allowance, and never at all once
    // it is gone.  Everything here is speculative: the picture is being
    // decoded before anyone has asked for it, so it always yields to the
    // frame rather than the other way round.
    if (background_budget_.exhausted()) {
        return;
    }
    if (!pending_image_ && image_decode_queue_.empty()) {
        return;
    }

    if (!pending_image_) {
        const th2::Archive* source = nullptr;
        const th2::ArchiveEntry* entry = nullptr;
        std::string key;
        std::string name;
        // Take the nearest one whose bytes are here.  Never off the network:
        // reading bytes that have not arrived suspends the wasm stack, which
        // would turn a decode nobody is waiting for into a stall everybody
        // feels.  Anything skipped is already on its way and the next scan
        // queues it again, so passing over it costs nothing but keeps the
        // frame's slot from going to waste.
        while (!image_decode_queue_.empty()) {
            key = image_decode_queue_.front();
            image_decode_queue_.pop_front();
            if (decoded_images_.contains(key)) {
                continue;
            }
            name = key.substr(4);
            source = key.starts_with("bak:") ? &backgrounds_ : &graphics_;
            entry = source->find(name);
            if (entry && source->resident(*entry)) {
                break;
            }
            entry = nullptr;
        }
        if (!entry) {
            return;
        }
        try {
            auto stored = background_budget_.spend(
                [&] { return source->read_stored(*entry); });
            PendingImage pending;
            pending.key = key;
            // Only TGA decodes in bands; a BMP goes through SDL, which has no
            // way to be interrupted, so those stay one-shot.  They are the
            // transition masks, which are small.
            pending.streamable = name.size() > 4
                && name.compare(name.size() - 4, 4, ".tga") == 0;
            if (stored.compressed) {
                pending.unpacking.emplace(
                    std::move(stored.bytes), stored.output_size);
            } else {
                pending.bytes = std::move(stored.bytes);
            }
            pending_image_ = std::move(pending);
        } catch (const std::exception& error) {
            SDL_Log("pre-decode of %s failed: %s",
                    key.c_str(), error.what());
            return;
        }
    }

    auto& pending = *pending_image_;
    try {
        if (pending.unpacking) {
            const bool finished = background_budget_.spend([&] {
                return pending.unpacking->advance(
                    background_budget_.remaining());
            });
            if (!finished) {
                return;
            }
            pending.bytes = pending.unpacking->take();
            pending.unpacking.reset();
        }
        if (background_budget_.exhausted()) {
            return;
        }
        Surface surface;
        if (pending.streamable) {
            if (!pending.decoding) {
                pending.decoding.emplace(std::move(pending.bytes));
            }
            const bool finished = background_budget_.spend([&] {
                return pending.decoding->advance(
                    background_budget_.remaining());
            });
            if (!finished) {
                return;
            }
            surface.reset(pending.decoding->take());
        } else {
            const auto key = pending.key;
            surface.reset(background_budget_.spend([&] {
                return th2::load_image(pending.bytes, key.substr(4));
            }));
        }
        if (surface) {
            if (decoded_images_.insert(
                    pending.key, std::move(surface), scan_generation_,
                    plan_.depth_of(pending.key))) {
                if (th2app::trace_prefetch) {
                    ++trace_.image_decoded;
                }
            } else if (th2app::trace_prefetch) {
                // Nothing held was worth less, so this was decoded for
                // nothing.
                ++trace_.image_evicted_unused;
            }
        }
    } catch (const std::exception& error) {
        // A picture that will not decode is not worth a crash here; the load
        // path will hit the same failure and report it in context.
        SDL_Log("pre-decode of %s failed: %s",
                pending.key.c_str(), error.what());
    }
    pending_image_.reset();
}

Surface Game::take_predecoded_image(bool background, std::string_view name)
{
    const auto key =
        std::string(background ? "bak:" : "grp:") + std::string(name);
    if (!decoded_images_.contains(key)) {
        note_asset_use(key);
        return {};
    }
    if (th2app::trace_prefetch) {
        ++trace_.image_used;
    }
    // Handed over rather than shared: the tone curves rewrite it in place.
    return decoded_images_.take(key);
}

void Game::restore_overlay(std::size_t slot, const OverlayState& held)
{
    // AVG_LoadBmpSetting: reload the picture, then replay every setter in
    // the order the engine does, so the graph ends up as it was.
    const int graph = th2::grp_script + static_cast<int>(slot);
    if (held.name.empty()) {
        display().reset_graph(graph);
        display().release_bmp(th2::bmp_script + static_cast<int>(slot));
        overlay_states_[slot] = {};
        return;
    }
    load_overlay(
        slot, held.name, held.archive, held.tone_type, held.layer,
        held.nuki);
    overlay_states_[slot] = held;
    display().set_graph_disp(graph, held.visible);
    display().set_graph_param(
        graph,
        static_cast<std::uint32_t>(held.parameter)
            | (static_cast<std::uint32_t>(held.parameter_value) << 16));
    display().set_graph_rev_param(
        graph, static_cast<std::uint32_t>(held.reverse));
    display().set_graph_bright(graph, held.red, held.green, held.blue);
    display().set_graph_pos(
        graph, held.destination_x * 800 / 640, held.destination_y * 600 / 448,
        held.source_x * 800 / 640, held.source_y * 600 / 448,
        held.destination_width * 800 / 640,
        held.destination_height * 600 / 448);
    if (held.zoom) {
        display().set_graph_zoom2(
            graph, held.zoom_center_x * 800 / 640,
            held.zoom_center_y * 600 / 448, held.zoom);
    } else if (held.destination_width != held.source_width
               || held.destination_height != held.source_height) {
        display().set_graph_zoom(
            graph, held.destination_x * 800 / 640,
            held.destination_y * 600 / 448,
            held.destination_width * 800 / 640,
            held.destination_height * 600 / 448);
    }
}

void Game::reset_overlays()
{
    // ResetBmp for every slot: the graph goes, the bitmap goes, and
    // SpriteBmp[i] goes with them.
    for (std::size_t i = 0; i < overlay_states_.size(); ++i) {
        display().reset_graph(th2::grp_script + static_cast<int>(i));
        display().release_bmp(th2::bmp_script + static_cast<int>(i));
        overlay_states_[i] = {};
    }
}

void Game::load_overlay(
    std::size_t slot, std::string name, std::string archive,
    int tone_type, int layer, int nuki)
{
    // The default branch of AVG_SetBmp: decode into BMP_SCRIPT+i, then
    //     DSP_SetGraph( GRP_SCRIPT+s_bno, BMP_SCRIPT+s_bno, layer, ON, nuki )
    // and record the same thing in SpriteBmp[i] for the savegame.
    const auto& source = archive == "bak" ? backgrounds_ : graphics_;
    Texture texture = load_toned_texture(
        renderer_, source, name, graphics_,
        tone_type == 1
            ? character_tone_curves()
            : background_tone_curves(),
        nullptr,
        take_predecoded_image(archive == "bak", name));
    auto& state = overlay_states_[slot];
    state = {};
    state.name = std::move(name);
    state.archive = std::move(archive);
    state.tone_type = tone_type;
    state.layer = layer;
    state.nuki = nuki;
    const int bmp = th2::bmp_script + static_cast<int>(slot);
    const int graph = th2::grp_script + static_cast<int>(slot);
    if (!texture) {
        display().release_bmp(bmp);
        display().reset_graph(graph);
        return;
    }
    float width = 0.0f;
    float height = 0.0f;
    SDL_GetTextureSize(texture.get(), &width, &height);
    display().set_bmp(
        bmp, std::move(texture), static_cast<int>(width),
        static_cast<int>(height));
    display().set_graph(graph, bmp, layer, true, nuki);
    // SpriteBmp's own rectangle is in the script's 640x448, which is what
    // the setters below take and scale on the way in.
    state.destination_width = static_cast<int>(width * 640.0f / 800.0f);
    state.destination_height = static_cast<int>(height * 448.0f / 600.0f);
    state.source_width = state.destination_width;
    state.source_height = state.destination_height;
}

}  // namespace th2app
