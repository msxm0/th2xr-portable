#pragma once

#include <array>
#include <filesystem>
#include <string>

namespace th2 {

struct GameConfig {
    int bgm_volume = 176;
    int se_volume = 256;
    int voice_volume = 256;
    bool bgm_muted = false;
    bool se_muted = false;
    bool voice_muted = false;
    std::array<int, 11> character_voice_volume{256, 256, 256, 256, 256, 256,
                                                256, 256, 256, 256, 256};
    std::array<bool, 11> character_voice_muted{};
    int auto_line_ms = 2000;
    int auto_page_ms = 4000;
    // 16 ms per character is the original's 60 chars/s (msg_wait 2);
    // its faster setting is 120 chars/s and 0 is instant.
    int text_speed_ms = 16;
    // Multiplies every effect duration, as Avg.wait does in the original:
    // AVG_EffCnt(n) = wait * n * frame / 60.  The options screen offers
    // 0 (instant), 1 (normal), 2 and 4.
    static constexpr std::array<int, 4> effect_speeds{0, 1, 2, 4};
    int effect_speed = 1;
    // Background half-tone while the message window is up, as in the
    // original: the background is multiplied by half_tone/128, so 128 leaves
    // it untouched and the darkest setting still shows 28% of the art.  The
    // bounds come from the sidebar bar's travel (GM_AvgMsg.cpp: half_tone =
    // 128 - clamp(y, 0, track_height - handle_height)).
    static constexpr int min_message_half_tone = 36;
    static constexpr int max_message_half_tone = 128;
    int message_half_tone = 64;
    bool auto_skip_read = false;
    bool skip_unread = false;
    bool wheel_opens_backlog = true;
    bool autosave_enabled = true;
#ifdef __ANDROID__
    int sidebar_mode = 3;  // Hidden by default — touch gestures replace it
#else
    int sidebar_mode = 0;  // Fade when away
#endif
    bool fullscreen = false;
    int window_x = -1;
    int window_y = -1;
    int window_width = 1600;
    int window_height = 1200;
    bool anime4k = true;
    bool authentic_font = false;
    std::string font_family = "Noto Sans";
    int font_size = 24;
    bool show_script_position = false;
    bool dump_transition_frames = false;
};

GameConfig load_config(const std::filesystem::path& path);
void save_config(const std::filesystem::path& path, const GameConfig& config);
int auto_delay_ms(const GameConfig& config, bool text_is_read,
                  bool has_hidden_segments, bool message_ends_block);

}  // namespace th2
