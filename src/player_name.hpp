#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace th2 {

struct PlayerName {
    std::string family;
    std::string given;
    std::string family_reading;
    std::string given_reading;
    std::string nickname;
    std::string nickname_reading;
};

PlayerName load_default_player_name(const std::filesystem::path& executable);

// AVG_SetName substitutes a compiled-in character name for the *h2 token -
// a surname, or a given name when flag 213 is set.  The English release
// romanised both, which is right for play and wrong for a trace: the
// reference is built from the GPL source and can only ever have the original
// two-character names, so the same line is six counts wide here and two
// there.  Trace runs pin these back; see Game::configure_trace, which pins
// player_name_ for exactly the same reason.
void set_h2_character_name(std::string family, std::string given);
bool uses_default_voice_name(
    const PlayerName& name, const PlayerName& default_name);
std::string substitute_player_name(
    std::string_view source, const PlayerName& name,
    bool use_komaki_given_name = false);

}  // namespace th2
