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
    const th2::Event& event, std::string_view script_name,
    th2::PrefetchRank rank)
{
    // Depth is recorded for every asset this event names, whether or not it
    // still needs fetching: the plan describes what the script wants, and a
    // resident asset is still wanted - it is what stops the caches treating
    // it as passed and evicting it moments before it is used.
    const auto note = [this](const char* prefix, std::string_view asset) {
        plan_.note(std::string(prefix) + std::string(asset), scan_depth_);
    };
    // The depth is read at the moment of the request, not captured when the
    // event was decoded: each asset in an event advances it, so a background
    // and the voice line after it are not treated as equally urgent.  A
    // resident asset is still re-announced, which is how the store learns it
    // is still on a reachable path.
    const auto request = [this](const th2::Archive& archive,
                                std::string_view asset) {
        const auto* entry = archive.find(asset);
        if (!entry) {
            return 0;
        }
        archive.prefetch(*entry, scan_depth_);
        return archive.resident(*entry) ? 0 : 1;
    };
    (void)rank;

    // Audio needs decoding as well as fetching, and decoding reads the bytes,
    // so the decode is only worth queuing once they have landed.  Scans
    // repeat while the player reads, so an asset fetched by one scan gets its
    // decode queued by the next.
    const auto request_audio = [&](const th2::Archive& archive,
                                   std::string_view asset) {
        plan_.note(std::string(asset), scan_depth_);
        if (&archive == &voice_archive_) {
            ++scan_.voices;
        }
        ++scan_depth_;
        const int requested = request(archive, asset);
        if (const auto* entry = archive.find(asset);
            entry && archive.resident(*entry)) {
            request_audio_decode(archive, asset, rank);
        }
        return requested;
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
        note("grp:", asset);
        ++scan_.sprites;
        ++scan_depth_;
        const int requested = request(graphics_, asset);
        // Capped at five, unlike the first attempt at this: the lookahead
        // sees every pose across every branch, and decoding all of them cost
        // nine times what it used.
        request_image_decode(false, asset);
        return requested;
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
        note(background ? "bak:" : "grp:", *asset);
        if (background) {
            ++scan_.backgrounds;
        } else {
            ++scan_.sprites;
        }
        ++scan_depth_;
        const int requested =
            request(background ? backgrounds_ : graphics_, *asset);
        // Queue the decode too.  request_image_decode() only takes it up once
        // the bytes have arrived, so a miss here simply means the next scan
        // catches it.
        request_image_decode(background, *asset);
        return requested;
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

int Game::prefetch_script_start(const std::string& name, int budget)
{
    // Scripts come out of SDT.PAK, which is small enough that the browser
    // build holds it whole, so opening one to look at it costs nothing.
    const auto* entry = scripts_.find(scenario_file_name(name));
    if (!entry) {
        return 0;
    }
    constexpr std::size_t opening_instructions = 128;
    // Only the opening of the script is scanned, so only that much of it is
    // decompressed: whole scripts run to hundreds of kilobytes, and this
    // runs on the thread that draws.
    constexpr std::size_t prefix_bytes = th2::Scenario::header_size + 32768;
    int requests = 0;
    try {
        // The same handful of destinations recur as the player moves between
        // scenes, so the last few prefixes are kept.
        auto cached = script_prefix_cache_.find(entry->name);
        if (cached == script_prefix_cache_.end()) {
            constexpr std::size_t cache_limit = 8;
            if (script_prefix_cache_.size() >= cache_limit) {
                script_prefix_cache_.clear();
            }
            cached = script_prefix_cache_
                         .emplace(
                             entry->name,
                             std::make_shared<const std::vector<std::uint8_t>>(
                                 scripts_.read_prefix(*entry, prefix_bytes)))
                         .first;
        }
        const auto& prefix = *cached->second;
        // A prefix cannot go through Scenario, which checks the file against
        // its declared size, so the header is stepped over here instead.
        if (prefix.size() <= th2::Scenario::header_size
            || prefix[0] != 'L' || prefix[2] != 'F') {
            return 0;
        }
        const auto bytecode = std::span<const std::uint8_t>(prefix).subspan(
            th2::Scenario::header_size);
        // A script the interpreter has not entered yet has no register
        // state worth guessing at, so only its literal references resolve.
        static constexpr std::array<std::int32_t, 64> registers{};
        std::size_t offset = 0;
        for (std::size_t step = 0;
             step < opening_instructions && offset < bytecode.size()
             && requests < budget;
             ++step) {
            const auto instruction = th2::decode_instruction(bytecode, offset);
            if (instruction.size == 0
                || instruction.offset + instruction.size > bytecode.size()) {
                break;
            }
            if (interesting_opcode(instruction.name)) {
                try {
                    const auto event = th2::decode_event(
                        instruction,
                        bytecode.subspan(instruction.offset, instruction.size),
                        registers);
                    requests += prefetch_event_assets(
                        event, entry->name, th2::PrefetchRank::branch);
                } catch (const std::exception&) {
                }
            }
            offset += instruction.size;
        }
    } catch (const std::exception&) {
        return requests;
    }
    return requests;
}

void Game::prefetch_upcoming_assets()
{
    // The queues are rebuilt rather than appended to, so they always hold the
    // nearest few rather than whatever an earlier scan happened to leave
    // behind.  Work already under way is held elsewhere - a part-decoded
    // picture in pending_image_, an open decoder in audio_decoders_ - so
    // nothing in progress is lost.
    image_decode_queue_.clear();
    audio_decode_queue_.clear();
    // A fresh plan each scan.  Anything the walk below does not reach has
    // been passed, and the caches will treat it as such.
    plan_.begin();
    scan_ = {};
    scan_depth_ = 0;
    // Everything this round does not ask for again has fallen off every path
    // the script can still take.
    th2::data_prefetch_new_round();



    // Every archive read that misses costs a network round trip in the
    // browser build, and a scene change makes a dozen of them back to back,
    // each one freezing the frame it happens on.  The script says what is
    // coming, so walk it ahead of the interpreter and start those transfers
    // while the player is still reading the current line.  A guess that turns
    // out wrong only costs a request; the bytes land in the same cache the
    // reads use either way.
    // Superseded by scan_limits_, which stops each path at whichever of its
    // counts runs out first; this only bounds the raw decode loop.
    const std::size_t instruction_budget =
        static_cast<std::size_t>(scan_limits_.instructions);
    // A scene change is a dozen or so entries; the follow into the next
    // script needs room on top of that.  They travel in parallel, so the
    // ceiling is about how much speculative traffic is acceptable, not
    // about latency.
    constexpr int request_budget = 20;
    constexpr std::size_t target_budget = 4;
    std::vector<std::string> targets;

    const auto bytecode = runtime_.vm_bytecode();
    const auto registers = runtime_.vm_registers();
    std::size_t offset = runtime_.vm_pc();
    int requests = 0;
    for (std::size_t step = 0;
         step < instruction_budget && offset < bytecode.size()
         && requests < request_budget;
         ++step) {
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
        ++scan_.instructions;
        if (scan_.exhausted(scan_limits_)) {
            break;  // This path has been looked at far enough.
        }
        if (!interesting_opcode(instruction.name)) {
            offset += instruction.size;
            continue;
        }
        try {
            const auto event = th2::decode_event(
                instruction,
                bytecode.subspan(instruction.offset, instruction.size),
                registers);
            requests += prefetch_event_assets(
                event, runtime_.script_name(),
                th2::PrefetchRank::imminent);
            // Where the script can go next: the branch it takes and the
            // choices it offers all start a new script, and the first thing
            // a script does is usually to load a background and play a
            // sound.
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
                && targets.size() < target_budget) {
                targets.push_back(*target);
            }
        } catch (const std::exception&) {
            // An instruction this scan cannot make sense of is not a reason
            // to stop looking at the ones after it.
        }
        offset += instruction.size;
    }

    if (scanned_from_ != runtime_.script_name()) {
        scanned_from_ = runtime_.script_name();
        scanned_scripts_.clear();
    }
    const bool anything_to_follow = std::ranges::any_of(
        targets, [this](const std::string& target) {
            return !scanned_scripts_.contains(target);
        });
    // Following into another script is the expensive half of this - it
    // decompresses and parses one - so it waits for a frame with nothing
    // else going on.  Doing it during a transition would trade a download
    // stall for a rendering one, which is exactly the thing being fixed.
    const bool idle = waiting_for_input_ && !transition_ && !background_fade_
        && !screen_flash_ && !background_scroll_ && !movie_
        && !character_animation_active();
    if (!idle) {
        prefetch_follow_pending_ = anything_to_follow;
        return;
    }
    prefetch_follow_pending_ = false;
    prepare_pending_transition_mask();
    // Each branch is walked from the depth of the branch point, with its own
    // fresh per-path counts.  Two ways out of a choice are equally likely to
    // be the next thing needed, so the first asset down each carries the same
    // depth rather than one of them being demoted for being "another script".
    const int branch_depth = scan_depth_;
    for (const auto& target : targets) {
        if (requests >= request_budget) {
            break;
        }
        if (!scanned_scripts_.insert(target).second) {
            continue;  // Already looked at while in this script.
        }
        scan_depth_ = branch_depth;
        scan_ = {};
        prefetch_script_start(target, request_budget - requests);
        // One script per scan: opening one costs a decompression and a few
        // hundred instruction decodes, and this runs on the thread that
        // draws.  The rest are picked up by the scans of the lines that
        // follow, which is still long before the player can reach them.
        break;
    }
}

}  // namespace th2app
