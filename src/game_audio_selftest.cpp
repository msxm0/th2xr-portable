#include "game.hpp"

#include <SDL3/SDL_log.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace th2app {

// TH2_AUDIO_SELFTEST=OUT+ARCHIVE:STRIDE[+ARCHIVE:STRIDE...] - a check of the
// decode path for the harness, native or web alike: every STRIDEth sound of
// each named archive (bgm, se, voice) goes through audio_decoder(), the way
// the game plays it - on the web that is the 64KB head, the browser's
// decoder and the rest of the file once it lands - and OUT gets one line per
// file: its name, rate, channels, frames, and two sums over every 4096
// frames, which two decodes can be compared by without either side having
// to hold the other's audio.
void Game::run_audio_selftest()
{
    auto& t = audio_selftest_;
    if (!t.started) {
        t.started = true;
        const char* spec = SDL_getenv("TH2_AUDIO_SELFTEST");
        if (!spec) {
            return;
        }
        std::string_view rest(spec);
        // '+', not ',': the web harness splits its environment on commas,
        // and its URL hands a '+' over as a space.
        const auto comma = rest.find_first_of("+ ");
        t.out = std::fopen(std::string(rest.substr(0, comma)).c_str(), "w");
        if (!t.out || comma == std::string_view::npos) {
            SDL_Log("audio self-test: cannot start (%s)", spec);
            return;
        }
        rest.remove_prefix(comma + 1);
        while (!rest.empty()) {
            const auto end = rest.find_first_of("+ ");
            const auto item = rest.substr(0, end);
            const auto colon = item.find(':');
            const auto which = item.substr(0, colon);
            const int stride = colon == std::string_view::npos
                ? 1 : std::atoi(std::string(item.substr(colon + 1)).c_str());
            const th2::Archive* archive = which == "bgm" ? &bgm_archive_
                : which == "se"                         ? &se_archive_
                : which == "voice"                      ? &voice_archive_
                                                        : nullptr;
            if (archive && stride > 0) {
                const auto& entries = archive->entries();
                for (std::size_t i = 0; i < entries.size();
                     i += static_cast<std::size_t>(stride)) {
                    t.queue.emplace_back(archive, entries[i].name);
                }
            }
            if (end == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(end + 1);
        }
        SDL_Log("audio self-test: %zu files", t.queue.size());
    }
    if (!t.out) {
        return;
    }
    if (!t.decoder) {
        if (t.next >= t.queue.size()) {
            std::fprintf(t.out, "done %zu\n", t.next);
            std::fclose(t.out);
            t.out = nullptr;
            SDL_Log("audio self-test: done");
            return;
        }
        const auto& [archive, name] = t.queue[t.next];
        t.decoder = audio_decoder(*archive, name, 0, false);
    }
    collect_audio_rests();
    t.decoder->decode(std::chrono::milliseconds(8));
    if (!t.decoder->done() || t.decoder->awaiting_rest()) {
        return;
    }
    const auto& clip = t.decoder->clip();
    const auto channels = static_cast<std::size_t>(std::max(1, clip.channels));
    const auto frames = clip.samples.size() / channels;
    std::fprintf(t.out, "%s %d %d %zu", t.queue[t.next].second.c_str(),
                 clip.sample_rate, clip.channels, frames);
    constexpr std::size_t block = 4096;
    for (std::size_t at = 0; at < clip.samples.size(); at += block * channels) {
        const auto end = std::min(clip.samples.size(), at + block * channels);
        // The plain sum, and one weighted by position, which a shift of
        // even a frame changes.
        double sum = 0.0;
        double weighted = 0.0;
        for (std::size_t i = at; i < end; ++i) {
            sum += clip.samples[i];
            weighted += clip.samples[i]
                * static_cast<double>(static_cast<int>(i / channels % 13) - 6);
        }
        std::fprintf(t.out, " %.4f %.4f", sum, weighted);
    }
    std::fputc('\n', t.out);
    std::fflush(t.out);
    t.decoder.reset();
    ++t.next;
}

}  // namespace th2app
