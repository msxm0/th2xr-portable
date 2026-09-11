#include "game.hpp"

#include "bytecode.hpp"
#include "character.hpp"
#include "event.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace th2app {
namespace {

// The scanner runs over instructions the interpreter has not reached, so an
// argument may still be a register or a comparison rather than a value.  Only
// literals can be resolved to a file name; anything else is left alone.
const std::int32_t* literal(const th2::Event& event, std::size_t index)
{
    if (index >= event.arguments.size()) {
        return nullptr;
    }
    return std::get_if<std::int32_t>(&event.arguments[index]);
}

const std::string* literal_text(const th2::Event& event, std::size_t index)
{
    if (index >= event.arguments.size()) {
        return nullptr;
    }
    return std::get_if<std::string>(&event.arguments[index]);
}

// Decoding an event copies its arguments, and a script is mostly message
// text that this scan has no use for, so instructions are filtered by name
// first - that comes straight out of the opcode table without allocating.
bool interesting_opcode(std::string_view name)
{
    static constexpr std::array<std::string_view, 21> opcodes{
        "C", "CW", "CP",                         // character sprites
        "B", "BT", "BC", "BCT",                  // backgrounds
        "H", "HT", "V", "VT",                    // event CGs
        "SetBmpEx",                              // overlays
        "M",                                     // music
        "SE", "SEP",                             // sound effects
        "VV", "VA", "VB", "VC",                  // voices
        "LoadScript", "SetMapEvent",             // where the script goes next
    };
    return std::ranges::find(opcodes, name) != opcodes.end()
        || name == "SetSelectMesEx";
}

}  // namespace

int Game::prefetch_event_assets(
    const th2::Event& event, std::string_view script_name)
{
    // Every asset the event names is recorded at the depth the walk reached
    // it, whether or not it still needs fetching: the plan describes what the
    // script wants, and a resident asset is still wanted - recording it is
    // what stops the caches treating it as passed and dropping it moments
    // before it is used.
    const auto note = [this](const char* prefix, std::string_view asset,
                             bool background) {
        auto key = std::string(prefix) + std::string(asset);
        plan_.note(key, scanning_depth_);
        scan_assets_.push_back(
            ScanAsset{std::move(key), std::string(asset), nullptr,
                      background, false, scanning_depth_});
    };
    // Collected, not requested.  The whole scan's wants are sorted and
    // admitted in one pass afterwards, so a range three instructions away
    // cannot lose its place to one two hundred instructions away that the
    // walk happened to reach first.
    const auto request = [this](const th2::Archive& archive,
                                std::string_view asset) {
        const auto* entry = archive.find(asset);
        if (!entry) {
            return 0;
        }
        const auto range = archive.range_of(*entry);
        auto& want = scan_wants_[range.path + ':'
                                 + std::to_string(range.offset) + ':'
                                 + std::to_string(range.size)];
        if (want.size == 0) {
            want = WantedRange{range.path, range.offset, range.size,
                               scanning_depth_};
        } else {
            want.depth = std::min(want.depth, scanning_depth_);
        }
        return 1;
    };

    // Audio needs decoding as well as fetching, and decoding reads the bytes,
    // so the decode is only worth queuing once they have landed.  Scans
    // repeat while the player reads, so an asset fetched by one scan gets its
    // decode queued by the next.
    const auto request_audio = [&](const th2::Archive& archive,
                                   std::string_view asset) {
        plan_.note(std::string(asset), scanning_depth_);
        scan_assets_.push_back(
            ScanAsset{std::string(asset), std::string(asset), &archive,
                      false, true, scanning_depth_});
        return request(archive, asset);
    };

    const auto& name = event.instruction.name;
    if (name == "C" || name == "CW" || name == "CP") {
        // Character sprites: a pose change is one archive entry, and a scene
        // with three characters talking turns over several of them a line.
        const auto* character = literal(event, 0);
        const auto* pose = literal(event, 1);
        if (!character || !pose || *character < 0 || *pose < 0) {
            return 0;
        }
        const auto asset = th2::character_asset_name(*character, *pose);
        note("grp:", asset, false);
        // note() is what puts this in front of the pre-decoder: the scan
        // collects, and the one pass afterwards decides which twenty are
        // nearest.  Queueing here as well only added work the rebuild threw
        // away.
        return request(graphics_, asset);
    }
    if (name == "B" || name == "BT" || name == "BC" || name == "BCT") {
        // A pattern wipe needs its mask prepared as well as its background
        // fetched, and preparing one is an LZS decompress plus a walk over
        // every pixel - too much for the frame the wipe starts on.
        if (const auto* change = literal(event, 0);
            change && *change >= 0x80) {
            request(graphics_, std::format("f0{:03d}.bmp", *change & 0x7f));
            if (!transition_masks_.contains(*change)) {
                pending_transition_masks_.insert(*change);
            }
        }
        const auto* scene_high = literal(event, 1);
        const auto* scene_low = literal(event, 2);
        if (!scene_high || *scene_high < 0) {
            return 0;
        }
        const int scene = seasonal_background_scene(
            *scene_high * 10 + (scene_low ? std::max(0, *scene_low) : 0));
        // The tone and the weather are engine state rather than arguments,
        // so this is the name the load would use if it happened now.  A
        // script that changes either first only costs a wasted request.
        return request(
            backgrounds_,
            std::format(
                "B{:03d}{}{}{}.bmp", scene / 10,
                (tone_back_ < 0 ? tone_ : tone_back_) % 4, weather_,
                scene % 10));
    }
    if (name == "H" || name == "HT" || name == "V" || name == "VT") {
        const auto* visual_high = literal(event, 1);
        const auto* visual_low = literal(event, 2);
        if (!visual_high || *visual_high < 0) {
            return 0;
        }
        const int visual = *visual_high * 10
            + (visual_low && *visual_low >= 0 ? *visual_low : 0);
        const char prefix = (name == "H" || name == "HT") ? 'h' : 'v';
        return request(graphics_, std::format("{}{:06d}.tga", prefix, visual));
    }
    if (name == "SetBmpEx") {
        // Backgrounds, character sprites and CGs: by far the most of what a
        // scene change reads.
        const auto* asset = literal_text(event, 2);
        const auto* pack = literal_text(event, 6);
        if (!asset || !pack || asset->empty()) {
            return 0;
        }
        const bool background = *pack == "bak";
        note(background ? "bak:" : "grp:", *asset, background);
        return request(background ? backgrounds_ : graphics_, *asset);
    }
    if (name == "M") {
        const auto* track = literal(event, 0);
        if (!track || *track < 0) {
            return 0;
        }
        // play_bgm() prefers the single file and falls back to the intro and
        // loop pair, so ask for whichever exists.
        const auto single = std::format("BGM_{:03d}.OGG", *track);
        if (bgm_archive_.find(single)) {
            return request_audio(bgm_archive_, single);
        }
        const auto loop = literal(event, 2);
        int requests = request_audio(
            bgm_archive_, std::format("BGM_{:03d}_A.OGG", *track));
        if (!loop || *loop != 0) {
            requests += request_audio(
                bgm_archive_, std::format("BGM_{:03d}_B.OGG", *track));
        }
        return requests;
    }
    if (name == "SE" || name == "SEP") {
        const auto* sound = literal(event, name == "SE" ? 0 : 1);
        if (!sound || *sound < 0) {
            return 0;
        }
        return request_audio(
            se_archive_, std::format("SE_{:04d}.WAV", *sound));
    }
    if (name == "VV" || name == "VA" || name == "VB" || name == "VC") {
        const auto* character_argument = literal(event, 0);
        const auto* voice = literal(event, 3);
        if (!character_argument || !voice || *voice < 0) {
            return 0;
        }
        int character = *character_argument;
        if (character >= 10 && character != 28) {
            character = 99;
        }
        // play_voice() resolves the scenario the same way, from the script
        // name and the VI overrides that are already in effect.
        int scenario = th2app::scenario_number(script_name);
        if (vi_event_voice_no_all_ >= 0) {
            scenario = vi_event_voice_no_all_;
        } else if (vi_event_voice_no_ >= 0) {
            scenario = scenario / 100 * 100 + vi_event_voice_no_;
        }
        const auto standard = std::format(
            "K{:09d}_{:03d}{:03d}.OGG", scenario, *voice, character);
        if (runtime_.flag(5) == 0) {
            const auto alternate = std::format(
                "K{:09d}_{:03d}{:03d}A.OGG", scenario, *voice, character);
            if (voice_archive_.find(alternate)) {
                return request_audio(voice_archive_, alternate);
            }
        }
        return request_audio(voice_archive_, standard);
    }
    return 0;
}

namespace {

// LoadScript and the map take a name with or without the extension, the way
// ScriptRuntime::load() does.
std::string scenario_file_name(std::string name)
{
    if (name.size() < 4
        || (name.compare(name.size() - 4, 4, ".sdt") != 0
            && name.compare(name.size() - 4, 4, ".SDT") != 0)) {
        name += ".sdt";
    }
    return name;
}

}  // namespace

void Game::preload_scripts()
{
    const auto started = std::chrono::steady_clock::now();
    std::size_t bytes = 0;
    for (const auto& entry : scripts_.entries()) {
        try {
            auto contents = scripts_.read(entry);
            // A script is a header followed by bytecode; anything that does
            // not look like one is simply not offered to the walk.
            if (contents.size() <= th2::Scenario::header_size
                || contents[0] != 'L' || contents[2] != 'F') {
                continue;
            }
            bytes += contents.size();
            script_bytecode_.emplace(entry.name, std::move(contents));
        } catch (const std::exception&) {
            // One script that will not decompress is not a reason to fail
            // the others; the walk simply will not follow into it.
        }
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    SDL_Log("scripts: %zu decompressed, %zu KB, %lld ms",
            script_bytecode_.size(), bytes / 1024,
            static_cast<long long>(took.count()));
}

std::span<const std::uint8_t> Game::script_prefix_bytecode(
    const std::string& name)
{
    const auto* entry = scripts_.find(scenario_file_name(name));
    if (!entry) {
        return {};
    }
    auto found = script_bytecode_.find(entry->name);
    if (found == script_bytecode_.end()) {
        // Not in the preload.  The two entries that startup skips are not
        // scripts at all - a stray vssver.scc and an empty record - so this
        // should never fire, but a script the walk can reach and cannot read
        // would silently prune a branch, and that is a worse failure than a
        // one-off decompression.  Whatever comes back is kept, including
        // nothing, so a bad entry is tried once rather than every scan.
        std::vector<std::uint8_t> contents;
        try {
            contents = scripts_.read(*entry);
            if (contents.size() <= th2::Scenario::header_size
                || contents[0] != 'L' || contents[2] != 'F') {
                contents.clear();
            } else {
                SDL_Log("script %s was not preloaded; read on demand",
                        entry->name.c_str());
            }
        } catch (const std::exception&) {
            contents.clear();
        }
        found = script_bytecode_.emplace(entry->name, std::move(contents))
                    .first;
    }
    if (found->second.size() <= th2::Scenario::header_size) {
        return {};
    }
    // A script is a header followed by bytecode; it cannot go through
    // Scenario, which checks the file against its declared size, so the
    // header is stepped over here instead.
    return std::span<const std::uint8_t>(found->second)
        .subspan(th2::Scenario::header_size);
}

void Game::explore(
    std::span<const std::uint8_t> bytecode,
    std::span<const std::int32_t> registers, const std::string& script,
    std::size_t offset, int depth, int& requests, int request_budget,
    std::unordered_set<std::string>& visited)
{
    while (depth < scan_depth_limit && offset < bytecode.size()
           && requests < request_budget) {
        th2::Instruction instruction{};
        try {
            instruction = th2::decode_instruction(bytecode, offset);
        } catch (const std::exception&) {
            return;
        }
        if (instruction.size == 0
            || instruction.offset + instruction.size > bytecode.size()) {
            return;
        }
        ++depth;
        if (interesting_opcode(instruction.name)) {
            scanning_depth_ = depth;
            try {
                const auto event = th2::decode_event(
                    instruction,
                    bytecode.subspan(instruction.offset, instruction.size),
                    registers);
                requests += prefetch_event_assets(
                    event, script);

                // Follow where the script can go.  Depth carries across the
                // jump, so what is just inside a branch three instructions
                // away outranks what is two hundred instructions down this
                // one - which is the whole point of counting steps rather
                // than asking which script something lives in.
                const auto& opcode = event.instruction.name;
                const std::string* target = nullptr;
                if (opcode == "LoadScript") {
                    target = literal_text(event, 0);
                } else if (opcode == "SetSelectMesEx") {
                    target = literal_text(event, 1);
                } else if (opcode == "SetMapEvent") {
                    target = literal_text(event, 3);
                }
                if (target && !target->empty()
                    && visited.insert(*target).second) {
                    // A script the interpreter has not entered has no
                    // register state worth guessing at, so only its literal
                    // references resolve.
                    static constexpr std::array<std::int32_t, 64> none{};
                    const auto prefix = script_prefix_bytecode(*target);
                    if (!prefix.empty()) {
                        explore(prefix, none, *target, 0, depth, requests,
                                request_budget, visited);
                    }
                }
            } catch (const std::exception&) {
                // One instruction this cannot make sense of is no reason to
                // stop looking at the ones after it.
            }
        }
        offset += instruction.size;
    }
}

void Game::submit_scan_wants()
{
    // Sorted by (depth, size, offset).  Depth is the point; size and offset
    // only make the order total, so the same script position always admits
    // the same ranges in the same sequence instead of depending on which
    // instruction the walk happened to decode first.
    std::vector<const WantedRange*> wanted;
    wanted.reserve(scan_wants_.size());
    for (const auto& [key, want] : scan_wants_) {
        wanted.push_back(&want);
    }
    std::ranges::sort(wanted, [](const WantedRange* a, const WantedRange* b) {
        return std::tie(a->depth, a->size, a->offset)
            < std::tie(b->depth, b->size, b->offset);
    });

    std::string lines;
    lines.reserve(wanted.size() * 64);
    for (const WantedRange* want : wanted) {
        lines += want->path;
        lines += '\t';
        lines += std::to_string(want->offset);
        lines += '\t';
        lines += std::to_string(want->size);
        lines += '\t';
        lines += std::to_string(want->depth);
        lines += '\n';
    }
    th2::data_prefetch_submit(lines);
}

void Game::update_predecode_queues()
{
    // Nearest first, and each asset once at its shallowest sighting.
    std::ranges::sort(scan_assets_, [](const ScanAsset& a, const ScanAsset& b) {
        return std::tie(a.depth, a.key) < std::tie(b.depth, b.key);
    });
    const auto last = std::ranges::unique(
        scan_assets_, {}, &ScanAsset::key).begin();
    scan_assets_.erase(last, scan_assets_.end());

    image_decode_queue_.clear();
    audio_decode_queue_.clear();
    std::size_t images = 0;
    std::size_t audio = 0;

    for (const ScanAsset& asset : scan_assets_) {
        if (asset.audio) {
            // Whatever is held is still wanted, so it keeps its place; what
            // is not held is work, up to the nearest twenty.
            audio_decoders_.touch(asset.name, scan_generation_, asset.depth);
            if (audio >= th2::PredecodeCache<int>::live_limit) {
                continue;
            }
            ++audio;
            if (audio_decoders_.contains(asset.name)) {
                continue;
            }
            if (!asset.archive->find(asset.name)) {
                continue;  // A script can name a track the release omits.
            }
            audio_decode_queue_.push_back(AudioDecodeRequest{
                asset.archive, asset.name, asset.depth,
                ++audio_decode_order_});
            continue;
        }
        decoded_images_.touch(asset.key, scan_generation_, asset.depth);
        if (images >= th2::PredecodeCache<int>::live_limit) {
            continue;
        }
        ++images;
        if (decoded_images_.contains(asset.key)) {
            continue;
        }
        if (th2app::trace_prefetch) {
            ++trace_.image_queued;
        }
        image_decode_queue_.push_back(asset.key);
    }
}

void Game::prefetch_upcoming_assets()
{
    image_decode_queue_.clear();
    audio_decode_queue_.clear();
    plan_.begin();
    scan_wants_.clear();
    scan_assets_.clear();
    ++scan_generation_;

    prepare_pending_transition_mask();
    prefetch_follow_pending_ = false;

    constexpr int request_budget = 200;
    int requests = 0;
    std::unordered_set<std::string> visited;
    explore(runtime_.vm_bytecode(), runtime_.vm_registers(),
            runtime_.script_name(), runtime_.vm_pc(), 0, requests,
            request_budget, visited);

    submit_scan_wants();
    update_predecode_queues();
}

}  // namespace th2app
