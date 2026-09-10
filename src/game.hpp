#pragma once
#include "archive.hpp"
#include "audio.hpp"
#include "character.hpp"
#include "config.hpp"
#include "font.hpp"
#include "frame_budget.hpp"
#include "image.hpp"
#include "gl_transition.hpp"
#include "gamepad_input.hpp"
#include "imgui_layer.hpp"
#include "message.hpp"
#include "persistent_state.hpp"
#include "player_name.hpp"
#include "script_runtime.hpp"
#include "soak.hpp"
#include "soak_game.hpp"
#include "touch_input.hpp"
#include "upscaler.hpp"
#include "video.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_iostream.h>
#include <array>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <ostream>
#include <istream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
namespace th2app {

// Set by --cpu-transitions.  Forces the CPU blend even where the shader
// would work, so the two paths can be compared without rebuilding.
extern bool force_cpu_transitions;

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const;
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;
struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const;
};
using Surface = std::unique_ptr<SDL_Surface, SurfaceDeleter>;
struct WindowDeleter {
    void operator()(SDL_Window* window) const;
};
struct RendererDeleter {
    void operator()(SDL_Renderer* renderer) const;
};
struct IoDeleter {
    void operator()(SDL_IOStream* stream) const;
};
using WindowPtr = std::unique_ptr<SDL_Window, WindowDeleter>;
using RendererPtr = std::unique_ptr<SDL_Renderer, RendererDeleter>;
using IoPtr = std::unique_ptr<SDL_IOStream, IoDeleter>;
std::filesystem::path writable_directory();
std::filesystem::path profile_directory();
std::optional<std::filesystem::path> discover_game_data_path(
    const std::filesystem::path& default_path, bool explicit_path);
std::pair<float, float> logical_coordinates(
    float x, float y, int window_width, int window_height);
void convert_event_to_logical_coordinates(
    SDL_Event& event, int window_width, int window_height);
std::int32_t number(const th2::Event& event, std::size_t index);
const std::string& text(const th2::Event& event, std::size_t index);
Texture load_texture(
    SDL_Renderer* renderer, const th2::Archive& archive, std::string_view name);
struct ToneCurveSpec {
    std::string name;
    int vividness = 256;
};
// Decodes an image without touching the renderer, so it can be done on a
// frame that has time rather than on the one that needs the picture.
Surface decode_image(
    const th2::Archive& image_archive, std::string_view image_name);
Texture load_toned_texture(
    SDL_Renderer* renderer,
    const th2::Archive& image_archive,
    std::string_view image_name,
    const th2::Archive& curve_archive,
    const std::vector<ToneCurveSpec>& curves,
    Surface* pixels = nullptr,
    Surface predecoded = {});
th2::AudioClip load_audio(const th2::Archive& archive, std::string_view name);
int scenario_number(std::string_view name);
// Left edge of the message sidebar, from HistorySystemRectX[] in the
// original's GM_AvgMsg.cpp.
inline constexpr float sidebar_left_x = 776.0f;

// Last line of message text starts above this.
inline constexpr float message_bottom_y = 535.0f;

// Slim scrollbar drawn in the left margin when the text does not fit.
inline constexpr float message_scroll_x = 9.0f;
inline constexpr float message_scroll_width = 8.0f;

std::vector<std::string> display_lines(
    std::string_view source, float max_width,
    const std::function<float(std::string_view)>& measure);
std::string interpret_newlines(std::string text);
bool clip_texture_source(
    SDL_Texture* texture, SDL_FRect& source, SDL_FRect& destination);
class Game {
public:
    template <typename>
    friend class th2::SoakGameDriver;
    explicit Game(
        const std::filesystem::path& data,
        const std::optional<std::filesystem::path>& scenario,
        const std::optional<std::filesystem::path>& soak_directory,
        std::size_t soak_runs);
    ~Game();
#ifdef __ANDROID__
    static void handle_android_save_bundle_export_result(
        const char* uri, const char* error);
#endif
    int run();
    int run_loop();
    void iterate();
private:
    static constexpr std::uint32_t save_version_ = 26;
    static constexpr std::uint32_t first_backlog_voice_save_version_ = 25;
    static constexpr std::uint32_t oldest_supported_save_version_ = 26;
    static bool is_confirm_key(SDL_Keycode key);
    static bool is_alt_enter(const SDL_KeyboardEvent& key);
    enum class AudioWaitKind {
        bgm,
        sound_effect,
        voice,
    };
    struct AudioWait {
        AudioWaitKind kind;
        std::size_t channel;
    };
    struct CharacterTexture {
        int pose = -1;
        Texture texture;
    };
    enum class CharacterAnimationKind {
        none,
        enter,
        leave,
        pose,
        locate,
        brightness,
        alpha,
    };
    struct CharacterAnimation {
        CharacterAnimationKind kind = CharacterAnimationKind::none;
        int type = 0;
        int frames = 1;
        int from_locate = 1;
        int to_locate = 1;
        int from_brightness = 128;
        int to_brightness = 128;
        int from_alpha = 256;
        int to_alpha = 256;
        bool blocking = false;
        Texture previous;
        std::chrono::steady_clock::time_point started;
    };
    struct OverlayState {
        std::string name;
        std::string archive;
        bool visible = true;
        int layer = 0;
        int tone_type = 0;
        int parameter = 0;
        int parameter_value = 0;
        int reverse = 0;
        int red = 128;
        int green = 128;
        int blue = 128;
        int destination_x = 0;
        int destination_y = 0;
        int destination_width = 0;
        int destination_height = 0;
        int source_x = 0;
        int source_y = 0;
        int source_width = 0;
        int source_height = 0;
        int zoom_center_x = 0;
        int zoom_center_y = 0;
        int zoom = 0;
    };
    // Transition masks are a hundred-odd fixed files reused across sixteen
    // hundred pattern wipes, and preparing one costs an archive read, an LZS
    // decompress, a BMP parse and a walk over every pixel.  Doing that once
    // per wipe showed up as a fifth of the frames that overran.
    struct TransitionMask {
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
        Texture texture;  // Only built when the shader path can use it.
    };
    std::unordered_map<int, TransitionMask> transition_masks_;
    std::set<int> pending_transition_masks_;
    // Best-effort: null or unavailable means every wipe takes the CPU blend.
    std::unique_ptr<th2::GlPatternTransition> gl_transition_;
    bool gl_transition_usable() const;
    SDL_Texture* transition_mask_texture(int type);
    bool transition_needs_pixels(int type) const;
    const TransitionMask& transition_mask(int type);
    void prepare_pending_transition_mask();

    struct Transition {
        Texture previous;
        Surface previous_pixels;
        Surface next_pixels;
        Texture composite;
        std::vector<std::uint8_t> mask;
        int mask_width = 0;
        int mask_height = 0;
        int vague = 128;
        int frames = 1;
        int type = 1;
        bool resume_script = false;
        std::chrono::steady_clock::time_point started;
        std::uint64_t debug_id = 0;
        int last_dumped_frame = -1;
        bool debug_metadata_written = false;
    };
    struct BackgroundFade {
        std::array<float, 3> from{128.0f, 128.0f, 128.0f};
        std::array<float, 3> to{128.0f, 128.0f, 128.0f};
        std::chrono::steady_clock::time_point started;
        std::chrono::milliseconds duration;
    };
    struct ScreenFlash {
        int red = 255;
        int green = 255;
        int blue = 255;
        int fade_in_frames = 1;
        int fade_out_frames = 1;
        std::chrono::steady_clock::time_point started;
    };
    struct ShakeState {
        int type = 0;
        int pitch = 0;
        int frames = 0;
        int direction = 0;
        int swing = 256;
        int sampled_frame = 0;
        std::chrono::steady_clock::time_point started;
    };
    struct ShakeSample {
        float x = 0.0f;
        float y = 0.0f;
        float scale = 1.0f;
        double angle = 0.0;
        bool text_only = false;
        bool includes_text = false;
    };
    struct BackgroundView {
        float x = 0.0f;
        float y = 0.0f;
        float width = 800.0f;
        float height = 600.0f;
    };
    enum class BackgroundKind : std::int32_t {
        background,
        visual,
        hcg,
    };
    struct BackgroundScroll {
        BackgroundView from;
        BackgroundView to;
        int frames = 1;
        int easing = 0;
        bool zoom = false;
        std::chrono::steady_clock::time_point started;
    };
    struct MapEvent {
        int character = 0;
        int position = 0;
        int type = 0;
        std::string script;
    };
    struct MapPosition {
        int field;
        int x;
        int y;
        int overlap;
    };
    struct MapSpritePart {
        SDL_FRect source;
        float x;
        float y;
    };
    struct MapSpriteStep {
        int frame;
        int ticks;
    };
    struct MapCharacter {
        Texture texture;
        std::vector<std::vector<MapSpritePart>> frames;
        std::vector<MapSpriteStep> steps;
    };
    struct ClockState {
        int target = 0;
        int start_minutes = 0;
        int target_minutes = 0;
        int travel_frames = 0;
        std::chrono::steady_clock::time_point started;
    };
    struct CalendarState {
        int month = 0;
        int day = 0;
        int weekday = 0;
        int holiday = -1;
        bool dismissing = false;
        std::chrono::steady_clock::time_point started;
    };
    struct SakuraPetal {
        bool active = false;
        int type = 0;
        float x = 0.0f;
        float y = 0.0f;
        float axis_x = 0.0f;
        float axis_y = 0.0f;
        std::uint32_t counter = 0;
    };
    struct SakuraState {
        std::array<SakuraPetal, 200> petals;
        int amount = 0;
        int target_amount = 0;
        float wind = 1.0f;
        int speed = 10;
        int tick = 0;
        int reset_frames = -1;
        bool no_reset = false;
        std::chrono::steady_clock::time_point updated;
    };
    static constexpr std::array<MapPosition, 22> map_positions_{{
        {0, 280, 340, 0}, {2, 488, 210, 1}, {2, 326, 356, 2},
        {3, 262, 304, 3}, {3, 464, 220, 4}, {4, 330, 216, 5},
        {4, 490, 294, 6}, {1, 356, 412, 7}, {1, 288, 210, 8},
        {1, 288, 210, 8}, {1, 288, 210, 8}, {1, 567, 326, 9},
        {1, 288, 210, 8}, {1, 288, 210, 8}, {1, 288, 210, 8},
        {1, 288, 210, 8}, {1, 288, 210, 8}, {1, 288, 210, 8},
        {1, 288, 210, 8}, {1, 288, 210, 8}, {1, 288, 210, 8},
        {1, 288, 210, 8},
    }};
    th2::Archive scripts_;
    th2::Archive graphics_;
    th2::Archive backgrounds_;
    th2::Archive fonts_;
    th2::Archive bgm_archive_;
    th2::Archive se_archive_;
    th2::Archive voice_archive_;
    th2::Archive movie_archive_;
    th2::ScriptRuntime runtime_;
    const std::filesystem::path config_path_;
    const std::filesystem::path state_path_;
    th2::GameConfig config_;
    th2::PersistentState persistent_state_;
    std::array<std::int32_t, 1024> persistent_game_flags_{};
    std::unordered_set<int> unlocked_visual_cgs_;
    std::unordered_set<int> unlocked_h_cgs_;
    std::unordered_set<int> unlocked_replays_;
    const bool suppress_audio_output_;
    std::unique_ptr<th2::SoakGameDriver<Game>> soak_;
    std::size_t soak_renderer_ticks_ = 0;
    // Window/renderer holders are declared before every other SDL-dependent
    // member so that textures, audio streams, etc. are destroyed while the
    // renderer still exists. SDL itself is initialized in main().
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    WindowPtr window_holder_;
    RendererPtr renderer_holder_;
    std::unique_ptr<th2::ImGuiLayer> imgui_;
    std::unique_ptr<th2::Upscaler> upscaler_;
    th2::TouchInput touch_input_;
    GamepadInput gamepad_input_;
    th2::GameFont font_;
    bool anime4k_available_ = false;
    bool last_anime4k_wanted_ = false;
    Texture background_;
    int bg_scene_ = -1;
    BackgroundKind background_kind_ = BackgroundKind::background;
    BackgroundView background_view_;
    std::optional<BackgroundScroll> background_scroll_;
    std::array<Texture, 32> overlays_{};
    std::array<Surface, 32> overlay_pixels_{};
    std::array<OverlayState, 32> overlay_states_{};
    th2::Characters characters_;
    std::array<CharacterTexture, 32> character_textures_{};
    std::array<CharacterAnimation, 32> character_animations_{};
    std::array<bool, 32> character_staged_{};
    std::array<bool, 32> character_pending_removal_{};
    th2::AudioChannel bgm_;
    int bgm_track_ = -1;
    bool bgm_loop_ = false;
    int bgm_volume_ = 255;
    std::array<th2::AudioChannel, 8> transient_se_{};
    std::array<int, 8> transient_se_volume_{};
    std::array<th2::AudioChannel, 16> se_channels_{};
    std::array<int, 16> se_sound_{};  // sound number per channel, -1 = none
    std::array<bool, 16> se_loop_{};
    std::array<int, 16> se_volume_{};
    std::array<th2::AudioChannel, 8> voice_channels_{};
    std::array<int, 8> voice_sound_{};  // voice number per channel, -1 = none
    std::array<int, 8> voice_character_{};  // character for this voice
    std::array<int, 8> voice_scenario_{};  // scenario for voice filename
    std::array<int, 8> voice_volume_{};
    std::array<bool, 8> voice_loop_{};
    int vi_event_voice_no_ = -1;
    int vi_event_voice_no_all_ = -1;
    std::vector<std::uint8_t> movie_bytes_;
    std::unique_ptr<th2::VideoPlayer> movie_;
    bool movie_resume_script_ = false;
    int movie_mode_ = -1;
    float bgm_gain(int volume) const;
    float se_gain(int volume) const;
    void ensure_upscaler();
    std::size_t voice_character_index(int character) const;
    float voice_gain(int volume, int character) const;
    void apply_audio_gains();
    void sync_window_config();
    void toggle_fullscreen();
    // Keeps SDL's window in step with the page's canvas (browser only).
    void sync_web_viewport();
    void sync_web_canvas_buffer();
    // Decoding an upcoming background before the script asks for it.  The
    // bytes are already being prefetched; this is the other half, and it is
    // the half that lands on the frame where the scene changes.
    void request_image_decode(bool background, std::string_view name);
    void update_image_decode();
    Surface take_predecoded_image(bool background, std::string_view name);
#ifdef __EMSCRIPTEN__
    void web_published_sizes(int* box_width, int* box_height,
                             int* buffer_width,
                             int* buffer_height) const;
#endif
    // True when the ImGui panels should use the phone layout: full-screen
    // window with a scrollable, full-width body.
    bool compact_ui() const;
    void start_movie(int mode, int number, bool resume_script);
    void update_movie();
    void complete_movie();
    // UI textures (from GRP.PAK)
    Texture ui_sys_menu_bg_;       // sys0100.tga
    Texture ui_sys_menu_btns_;     // sys0110.tga
    Texture ui_sys_cancel_;        // sys0111.tga
    Texture ui_sidebar_track_;     // sys0000.tga
    Texture ui_sidebar_btns_;      // sys0001.tga
    Texture ui_keywait_;           // sys0011.tga (mid-page cursor)
    Texture ui_pageend_;           // sys0010.tga (end-of-page cursor)
    Texture ui_save_bg_;
    Texture ui_load_bg_;
    Texture ui_save_rows_;
    Texture ui_save_rows_hover_;
    Texture ui_save_new_;
    Texture ui_save_digits_;
    Texture ui_save_prompt_;
    Texture ui_load_prompt_;
    Texture ui_confirm_buttons_;
    Texture ui_save_controls_;
    Texture title_background_;
    Texture title_menu_;
    Texture omake_cg_background_;
    Texture omake_cg_locked_;
    Texture omake_music_background_;
    Texture omake_music_selection_;
    Texture omake_music_labels_;
    Texture omake_music_title_;
    Texture omake_music_artist_;
    Texture omake_music_playing_;
    Texture omake_replay_background_;
    Texture map_frame_;
    Texture map_arrows_;
    Texture map_markers_;
    std::array<Texture, 5> map_fields_;
    std::vector<MapCharacter> map_characters_;
    Texture clock_background_;
    std::optional<MapCharacter> clock_animation_;
    std::optional<ClockState> clock_state_;
    Texture calendar_background_;
    Texture calendar_labels_;
    Texture calendar_days_;
    std::optional<CalendarState> calendar_state_;
    int skipped_month_ = 0;
    int skipped_day_ = 0;
    Texture sakura_large_;
    Texture sakura_small_;
    std::optional<SakuraState> sakura_;
    std::uint32_t sakura_random_ = 0x13579bdfu;
    Surface title_foreground_pixels_;
    Texture title_masked_;
    std::vector<std::uint8_t> title_mask_;
    int title_mask_width_ = 0;
    int title_mask_height_ = 0;
    th2::Message message_;
    bool message_ends_block_ = true;
    int tone_ = 0;
    int tone_back_ = -1;
    int tone_char_ = -1;
    int weather_ = 0;
    std::string background_tone_curve_;
    bool running_ = true;
    bool app_active_ = true;
    std::chrono::steady_clock::time_point next_frame_{};
    // Last device pixel ratio pushed into SDL (browser only).
    double web_pixel_ratio_ = 0.0;
    // The finger currently standing in for the mouse, and where it was.
    SDL_FingerID touch_mouse_finger_ = 0;
    bool touch_mouse_active_ = false;
    bool touch_mouse_dragging_ = false;
    // Scripts already looked into while running the current one, and the
    // last few parsed, so following the same destination again is free.
    std::unordered_set<std::string> scanned_scripts_;
    std::string scanned_from_;
    std::unordered_map<
        std::string, std::shared_ptr<const std::vector<std::uint8_t>>>
        script_prefix_cache_;
    // Set when a scan is due; it runs on the next frame rather than on the
    // one the player's click landed on.
    bool prefetch_scan_pending_ = false;
    // A follow that was put off because the engine was busy animating.
    bool prefetch_follow_pending_ = false;
    // Scratch for the CPU transition blends, kept so a wipe allocates once
    // rather than once a frame.
    std::vector<std::uint8_t> transition_pixels_;
    // Keyed by archive and name.  Small: a couple of backgrounds in flight,
    // not a history of everything seen.
    std::unordered_map<std::string, Surface> decoded_images_;
    std::deque<std::string> image_decode_queue_;
    static constexpr std::size_t decoded_image_limit = 6;

    // A picture part-way through being decoded.  Held across frames, because
    // a large one is twenty milliseconds of work and no frame can take that
    // in one go.
    struct PendingImage {
        std::string key;
        std::optional<th2::LzsStream> unpacking;
        std::optional<th2::TgaStream> decoding;
        std::vector<std::uint8_t> bytes;
        bool streamable = false;
    };
    std::optional<PendingImage> pending_image_;

    // The frame's whole allowance for work nobody is waiting on, shared by
    // the prefetch scan, audio decoding and image decoding alike.
    //
    // Two milliseconds, not four.  These consumers spend whatever they are
    // given - the audio read-ahead in particular will decode as far forward
    // as the clock allows - so the allowance sets how much speculative work
    // gets done, not just how much is permitted.  At four, a playthrough
    // spent 40 seconds more in the Vorbis decoder than the same playthrough
    // at two, most of it on clips the script had already moved past, and
    // frames over 10ms went from 805 to 6167.  Two matches what audio used
    // to have to itself, so the shared pool is no larger than the largest
    // private one it replaced - the difference is that everything else now
    // has to fit inside it rather than being added on top.
    th2::FrameBudget background_budget_{std::chrono::milliseconds(2)};
    std::chrono::steady_clock::time_point last_metrics_publish_{};
    std::chrono::steady_clock::time_point last_viewport_poll_{};
    int web_viewport_generation_ = 0;
    bool viewport_bridge_installed_ = false;
    std::chrono::steady_clock::time_point last_prefetch_scan_{};
    // Event id that tells the loop to drop the hover highlights, queued
    // behind the click a lifted finger produced.
    Uint32 touch_clear_event_ = 0;
    float touch_mouse_x_ = 0.0f;
    float touch_mouse_y_ = 0.0f;
    bool waiting_for_input_ = false;
    std::optional<std::chrono::steady_clock::time_point> wake_time_;
    std::optional<AudioWait> audio_wait_;
    std::optional<Transition> transition_;
    std::optional<BackgroundFade> background_fade_;
    std::optional<ScreenFlash> screen_flash_;
    std::optional<ShakeState> shake_;
    Texture shake_target_;
    Texture pose_blend_target_;
    // Set while a character animation has the message window hidden.
    bool message_restore_after_animation_ = false;
    float half_tone_count_ = 0.0f;
    bool half_tone_fading_ = false;
    std::chrono::steady_clock::time_point half_tone_updated_{};
    std::array<float, 3> background_brightness_{128.0f, 128.0f, 128.0f};
    std::chrono::steady_clock::time_point skip_next_time_{};
    std::optional<std::chrono::steady_clock::time_point> auto_next_time_;
    std::string current_line_key_;
    std::chrono::steady_clock::time_point text_reveal_started_{};
    std::size_t text_reveal_start_ = 0;
    bool text_reveal_complete_ = true;
    std::uint64_t next_transition_debug_id_ = 1;
    bool direct_scenario_ = false;
    th2::AudioChannel& waited_audio_channel();
    struct Choice {
        std::string text;
        int flag_no = -1;
        int flag_value = 0;
        std::string sno;
    };
    bool choosing_ = false;
    std::vector<Choice> choices_;
    std::optional<std::chrono::steady_clock::time_point>
        choice_reveal_started_;
    int choice_highlight_ = 0;
    int choice_selected_ = -1;
    int choice_result_register_ = -1;
    bool choice_ex_ = false;
    // --- UI State ---
    enum class UiMode {
        title,
        cg_gallery,
        music_room,
        replay_gallery,
        game,
        system_menu,
        backlog,
        save,
        load,
        map,
    };
    UiMode ui_mode_ = UiMode::title;
    UiMode save_return_mode_ = UiMode::game;
    // Starts on the first item so the menu can be driven from the keyboard
    // alone; a mouse moving off the items clears it, as the original does.
    int title_highlight_ = 0;
    bool title_extras_ = false;
    bool title_extras_transition_from_ = false;
    std::optional<std::chrono::steady_clock::time_point>
        title_menu_transition_started_;
    int omake_highlight_ = 0;
    int omake_page_ = 0;
    int omake_music_playing_slot_ = -1;
    std::optional<int> omake_cg_view_;
    struct OmakeCgEntry {
        bool hcg = false;
        std::vector<int> variants;
    };
    std::vector<OmakeCgEntry> omake_cg_entries_;
    std::vector<Texture> omake_cg_thumbnails_;
    Texture omake_cg_full_;
    Texture omake_cg_previous_full_;
    int omake_cg_variant_ = 0;
    bool omake_cg_tall_scrolled_ = false;
    enum class OmakeCgPhase {
        viewing,
        opening,
        scrolling,
        changing,
        closing,
    };
    OmakeCgPhase omake_cg_phase_ = OmakeCgPhase::viewing;
    std::chrono::steady_clock::time_point omake_cg_phase_started_{};
    std::array<Texture, 9> omake_replay_thumbnails_;
    std::chrono::steady_clock::time_point title_started_{};
    std::optional<std::chrono::steady_clock::time_point> title_exit_started_;
    bool title_exit_game_ = false;
    int menu_highlight_ = 0;
    struct BacklogVoice {
        std::size_t start = 0;
        std::size_t end = 0;
        int scenario = 0;
        int voice = 0;
        int character = 0;
        int volume = 255;
        bool alternate = false;
    };
    struct BacklogEntry {
        std::string text;
        std::vector<BacklogVoice> voices;
    };
    std::vector<BacklogEntry> backlog_;
    std::vector<BacklogVoice> current_backlog_voices_;
    std::optional<BacklogVoice> pending_backlog_voice_;
    int backlog_depth_ = 0;
    int backlog_voice_hover_ = -1;
    bool backlog_handle_dragging_ = false;
    // First visible line when a page of text is taller than the screen.
    int message_scroll_ = 0;
    bool message_scroll_follow_ = true;
    bool message_scroll_dragging_ = false;
    // The backlog scrolls the same way, but starts at the top of an entry
    // rather than following its end.
    int backlog_scroll_ = 0;
    bool backlog_scroll_dragging_ = false;
    bool opacity_handle_hover_ = false;
    bool opacity_handle_dragging_ = false;
    bool backlog_handle_hover_ = false;
    int sidebar_hover_ = -1;
    float sidebar_alpha_ = 0.0f;
    std::chrono::steady_clock::time_point sidebar_alpha_updated_{};
    bool sidebar_mouse_near_ = false;
    bool suppress_sidebar_mouse_up_ = false;
    bool message_visible_ = true;
    int save_page_ = 0;
    int save_hover_ = -1;
    int save_confirm_slot_ = -1;
    int newest_save_slot_ = -1;
    Surface save_snapshot_;
    std::array<Texture, 10> save_thumbnails_{};
    std::chrono::steady_clock::time_point last_save_time_{};
    bool just_advanced_past_block_end_ = false;
    bool config_open_ = false;
    bool config_gamepad_focus_requested_ = false;
    bool confirm_return_title_ = false;
    enum class SaveBundleAction {
        none,
        export_bundle,
        import_bundle,
    };
    struct SaveBundleDialog {
        std::mutex mutex;
        SaveBundleAction action = SaveBundleAction::none;
        bool active = false;
        bool done = false;
        std::optional<std::string> path;
        std::string error;
    };
    SaveBundleDialog save_bundle_dialog_;
#ifdef __ANDROID__
    static SaveBundleDialog* android_save_bundle_export_dialog_;
#endif
    std::string save_bundle_status_;
    bool name_input_open_ = false;
    std::string name_error_;
    std::string load_error_;
    th2::PlayerName default_player_name_;
    th2::PlayerName player_name_;
    std::array<char, 64> name_family_{};
    std::array<char, 64> name_given_{};
    std::array<char, 64> name_family_reading_{};
    std::array<char, 64> name_given_reading_{};
    std::array<char, 64> name_nickname_{};
    bool auto_mode_ = false;
    bool skip_mode_ = false;
    bool demo_mode_ = false;
    bool replay_mode_ = false;
    int demo_delay_frames_ = 0;
    std::vector<MapEvent> map_events_;
    int map_field_ = 1;
    int map_previous_field_ = 1;
    int map_hover_ = -1;
    int map_slide_ticks_ = 0;
    int map_fade_ticks_ = 0;
    int map_selected_ = -1;
    std::chrono::steady_clock::time_point map_tick_{};
    std::chrono::steady_clock::time_point map_started_{};
    std::optional<th2::ReadMarker> current_read_marker() const;
    bool current_text_is_read() const;
    void mark_current_text_read();
    void manual_advance();
    int map_sakura_type() const;
    static std::uint16_t map_u16(
        std::span<const std::uint8_t> bytes, std::size_t offset);
    static std::uint32_t map_u32(
        std::span<const std::uint8_t> bytes, std::size_t offset);
    MapCharacter load_sprite_animation(const std::string& stem);
    MapCharacter load_map_character(const MapEvent& event);
    static constexpr std::array<int, 20> clock_minutes_{
        8 * 60 + 43, 9 * 60 + 5, 9 * 60 + 25, 9 * 60 + 35,
        10 * 60, 10 * 60 + 20, 10 * 60 + 30, 10 * 60 + 55,
        11 * 60 + 15, 11 * 60 + 25, 11 * 60 + 50, 12 * 60 + 10,
        12 * 60 + 35, 13 * 60, 13 * 60 + 25, 13 * 60 + 45,
        13 * 60 + 55, 14 * 60 + 20, 14 * 60 + 40, 14 * 60 + 50,
    };
    int weekday(int month, int day) const;
    int calendar_holiday(int month, int day) const;
    void begin_clock(int requested);
    void begin_calendar(int month, int day);
    void update_clock_calendar();
    void draw_sprite_frame(
        const MapCharacter& animation, int frame, float x, float y);
    void draw_clock_calendar();
    Texture load_sakura_texture(std::string_view name);
    void start_sakura(int amount, bool no_reset);
    void stop_sakura(bool force);
    int seasonal_background_scene(int scene) const;
    void update_background_sakura(int scene, bool background);
    std::uint32_t next_sakura_random();
    void spawn_sakura_petals();
    void update_sakura();
    void draw_sakura();
    std::string map_field_name(int field) const;
    void begin_map();
    void finish_map_selection(int selected);
    void complete_map_selection();
    void load_script(std::string name);
    bool load_scheduled_script();
    bool voice_playing() const;
    void update_playback_modes();
    struct SaveMetadata {
        bool exists = false;
        std::time_t timestamp = 0;
        int game_month = 0;
        int game_day = 0;
        int game_time = 0;
        std::string message;
    };
    std::array<SaveMetadata, 10> visible_saves_{};
    float choice_y_start() const;
    float choice_height(const Choice& choice) const;
    std::vector<std::string> choice_lines(
        const Choice& choice, int index) const;
    float message_text_width() const;
    // Scales an effect's frame count by the effect speed setting; 0 makes
    // it instant, which is what the original does while skipping too.
    int effect_frames(int frames) const;
    static std::chrono::milliseconds audio_fade_duration(int frames);
    float text_line_height() const;
    std::vector<std::string> display_lines(std::string_view source) const;
    float message_text_x() const;
    float message_text_y() const;
    static std::size_t utf8_prefix_bytes(
        std::string_view text, std::size_t characters);
    static std::size_t utf8_character_count(std::string_view text);
    int choice_reveal_count(int index) const;
    void start_text_reveal(std::size_t start);
    bool finish_text_reveal();
    void skip(bool force_unread = false);
    CharacterTexture& character_texture(int number);
    Surface capture_frame_pixels(bool art_only = false);
    // A GPU-side copy of the art target.  Transitions that only ever draw
    // the old frame as a texture do not need it on the CPU, and
    // SDL_RenderReadPixels is a pipeline sync: it waits for everything
    // queued to finish, which at high DPI cost 20-40ms a wipe.
    Texture capture_frame_texture();
    // The save file only keeps a small thumbnail, so scaling on the GPU and
    // reading that back moves a fraction of the pixels through what is still
    // a pipeline sync.
    Surface capture_frame_thumbnail(int width, int height);
    static constexpr int save_thumbnail_width = 160;
    static constexpr int save_thumbnail_height = 120;
    Texture texture_from_surface(SDL_Surface* surface);
    void retire_soak_gpu_work(bool force = false);
    std::vector<std::uint8_t> load_transition_mask(
        int type, int& width, int& height);
    // Script-driven transitions run through AVG_EffCnt(), which the
    // effect-speed setting scales; the title, save and gallery screens use
    // AVG_EffCnt4(), which it does not.
    enum class EffectTiming { script, menu };
    void begin_transition(
        int type, int frames, int vague, bool resume_script,
        EffectTiming timing = EffectTiming::script);
    void update_transition();
    void draw_pattern_transition(float progress);
    bool draw_pattern_transition_gpu(float progress);
    void ensure_transition_target();
    void draw_pixel_transition(float progress);
    void draw_geometric_transition(float progress);
    void dump_transition_frame(float progress);
    void draw_active_transition();
    void draw_script_position();
    void begin_background_fade(int red, int green, int blue, int frames);
    void update_background_fade();
    void update_screen_flash();
    float screen_flash_alpha() const;
    void update_shake();
    ShakeSample shake_sample();
    BackgroundView current_background_view() const;
    void update_background_scroll();
    void begin_background_scroll(
        float x, float y, float width, float height, int frames, int type);
    void load_character_texture(const th2::CharacterState& character);
    void reload_character_textures();
    void apply_staged_characters();
    void clear_characters();
    std::size_t character_index(int character_number) const;
    static std::vector<ToneCurveSpec> effect_tone_curves(
        int tone, bool character);
    static std::string base_tone_curve(int tone);
    std::vector<ToneCurveSpec> background_tone_curves() const;
    std::vector<ToneCurveSpec> character_tone_curves() const;
    int character_effect_frames(int frames) const;
    bool character_animation_active() const;
    void start_character_animation(
        int character_number, CharacterAnimation animation);
    void update_character_animations();
    void set_character(const th2::Event& event);
    void play_se(int channel, int sound, bool loop, int volume, int fade = 0,
                 bool wait_for_completion = false);
    void sync_game_flags();
    void play_bgm(int music, bool loop, int volume);
    void play_voice(const th2::Event& event);
    void replay_backlog_voice(const BacklogVoice& voice);
    void update_audio();
    void set_background(const th2::Event& event, bool keep_characters);
    void set_cg(
        const th2::Event& event, BackgroundKind kind, char prefix);
    void restore_background();
    std::optional<std::size_t> overlay_index(int requested) const;
    void load_overlay(
        std::size_t slot, std::string name, std::string archive,
        int tone_type = 0);
    void apply_overlay_brightness(std::size_t slot);
    bool handle(const th2::Event& event);
    std::filesystem::path dump_engine_error(
        const th2::ScriptStep& step, std::string_view error);
    std::filesystem::path dump_runtime_error(std::string_view error);
    void advance(bool skipping = false);
    void save(int slot);
    bool load(int slot);
    std::filesystem::path save_path(int slot) const;
    std::filesystem::path thumbnail_path(int slot) const;
    std::filesystem::path metadata_path(int slot) const;
    std::filesystem::path anime4k_shader_dir() const;
    void save_preview(int slot);
    SaveMetadata read_save_metadata(int slot) const;
    void perform_autosave();
    void refresh_save_page();
    void save_body(std::ostream& out) const;
    bool load_body(std::istream& in);
    void write_u32(std::ostream& out, std::uint32_t value) const;
    void write_i32(std::ostream& out, std::int32_t value) const;
    void write_i64(std::ostream& out, std::int64_t value) const;
    void write_str(std::ostream& out, std::string_view str,
                   std::size_t padded_size) const;
    std::uint32_t read_u32(std::istream& in) const;
    std::int32_t read_i32(std::istream& in) const;
    std::int64_t read_i64(std::istream& in) const;
    std::string read_str(std::istream& in, std::size_t size) const;
    // --- UI Methods ---
    void push_backlog();
    void open_system_menu();
    void reset_play_state();
    void initialize_scenario_flags();
    void start_new_game();
    void open_name_input();
    void begin_title_exit(bool start_game);
    void begin_title_menu_transition(bool extras);
    void update_title();
    void close_system_menu();
    void open_config();
    void close_config();
    static void append_u16(
        std::vector<std::uint8_t>& data, std::uint16_t value);
    static void append_u32(
        std::vector<std::uint8_t>& data, std::uint32_t value);
    static void append_u64(
        std::vector<std::uint8_t>& data, std::uint64_t value);
    static std::uint16_t read_u16(
        const std::vector<std::uint8_t>& data, std::size_t& offset);
    static std::uint32_t read_u32(
        const std::vector<std::uint8_t>& data, std::size_t& offset);
    static std::uint64_t read_u64(
        const std::vector<std::uint8_t>& data, std::size_t& offset);
    static std::vector<std::uint8_t> read_local_file(
        const std::filesystem::path& path);
    static void write_local_file(
        const std::filesystem::path& path,
        const std::vector<std::uint8_t>& bytes);
    static std::vector<std::uint8_t> read_sdl_file(const std::string& path);
    static void write_sdl_file(
        const std::string& path, const std::vector<std::uint8_t>& bytes);
    static bool safe_bundle_path(std::string_view path);
    std::vector<std::pair<std::string, std::filesystem::path>>
    save_bundle_files() const;
    std::vector<std::uint8_t> build_save_bundle();
    void export_save_bundle(const std::string& selected_path);
    void import_save_bundle(const std::string& path);
    void apply_save_bundle(const std::vector<std::uint8_t>& bundle);
    static void save_bundle_dialog_callback(
        void* userdata, const char* const* filelist, int);
    void show_save_bundle_export_dialog();
    void show_save_bundle_import_dialog();
    void process_save_bundle_dialog();
    bool volume_control(const char* label, int& volume, bool& muted);
    void return_to_title();
    void draw_config();
    void draw_name_input();
    void open_backlog();
    void close_backlog();
    // Starts background transfers for the assets the script is about to
    // need, so their reads do not stall a frame (browser build; a no-op
    // where reads come off a disk).
    void prefetch_upcoming_assets();
    // Decoding audio is the other half of getting an asset ready, and the
    // expensive half: a BGM track is 100-200ms of Vorbis on the thread that
    // draws.  Requests are queued alongside the byte prefetch and worked off
    // a few milliseconds per frame, in the same need order.
    struct AudioDecodeRequest {
        const th2::Archive* archive = nullptr;
        std::string name;
        int rank = 0;
        std::uint64_t order = 0;
    };
    struct AudioDecodeEntry {
        std::shared_ptr<th2::AudioDecoder> decoder;
        int rank = 0;
        std::uint64_t order = 0;
    };
    std::vector<AudioDecodeRequest> audio_decode_queue_;
    std::unordered_map<std::string, AudioDecodeEntry> audio_decoders_;
    std::uint64_t audio_decode_order_ = 0;

    // Read-ahead only: what a channel is playing is never evicted.
    static constexpr std::size_t audio_decode_budget_bytes = 96ull << 20;
    static constexpr std::size_t audio_decode_queue_limit = 10;

    std::shared_ptr<th2::AudioDecoder> audio_decoder(
        const th2::Archive& archive, std::string_view name, int rank);
    std::shared_ptr<th2::AudioDecoder> ready_audio_decoder(
        const th2::Archive& archive, std::string_view name);
    void request_audio_decode(
        const th2::Archive& archive, std::string_view name,
        th2::PrefetchRank rank);
    void update_audio_decode();
    void evict_audio_decoders();

    int prefetch_event_assets(
        const th2::Event& event, std::string_view script_name,
        th2::PrefetchRank rank);
    // Prefetches what a script loads when it starts, for the scripts the
    // current one can reach.  Returns how many transfers it started.
    int prefetch_script_start(const std::string& name, int budget);

    void handle_touch_actions();
    // Turns one finger's events into the mouse press, motion and release the
    // game's own UI is written against.
    void push_touch_mouse_event(
        const SDL_Event& event, int window_width, int window_height);
    // Starts a drag if the touch landed on something draggable.
    bool begin_touch_drag(float logical_x, float logical_y);
    // Drops the hover highlights a lifted finger left behind.
    void clear_pointer_highlights();
    bool backlog_older();
    bool backlog_newer();
    void execute_menu_item(int index);
    void open_save_load(UiMode mode);
    void close_save_load();
    void draw_save_digit_sheet_text(
        float x, float y, std::string_view text,
        std::uint8_t red = 255, std::uint8_t green = 255,
        std::uint8_t blue = 255);
    void draw_save_digit_number(float x, float y, int number, int digits);
    void draw_system_menu();
    void draw_map_layer(
        int field, float x, float alpha,
        bool draw_field, bool draw_events);
    void draw_map(bool ui);
    bool title_extras_available() const;
    bool title_item_disabled(int item) const;
    static constexpr std::array<int, 649> cg_gallery_layout{
        1010, 1011, 0, 1020, 0, 1030, 1031, 1032, 1033, 1034, 1035, 1036, 0, 1040, 1041, 1042,
        0, 1050, 0, 1060, 0, 1070, 1071, 0, 1080, 0, 1090, 0, 1100, 1101, 0, 1110,
        0, 1120, 0, 1130, 1131, 0, 1140, 0, 1150, 1151, 1152, 1153, 0, 1160, 0, 1170,
        0, 1990, 1991, 0, 2010, 2011, 2012, 2013, 2014, 2015, 0, 2020, 2021, 2022, 2023, 2024,
        0, 2030, 2031, 2032, 0, 2040, 2041, 2042, 0, 2050, 2051, 2052, 2053, 2054, 2055, 2056,
        2057, 2058, 0, 2060, 0, 2070, 0, 2080, 2081, 2082, 0, 2090, 0, 2100, 2101, 0,
        2110, 2111, 0, 2120, 0, 2140, 2141, 0, 2150, 0, 2160, 0, 2170, 0, 2180, 0,
        2190, 0, 3010, 3011, 3012, 0, 3020, 3021, 0, 3030, 3031, 3032, 0, 3040, 0, 3050,
        3051, 0, 3060, 0, 3070, 3071, 3072, 0, 3080, 3081, 3082, 3083, 3084, 3085, 0, 3090,
        3086, 0, 3100, 3101, 3102, 3103, 3104, 3105, 0, 3110, 0, 3120, 0, 3130, 3131, 3132,
        3133, 3134, 3135, 0, 3140, 0, 3150, 3151, 0, 4010, 0, 4020, 4021, 4022, 0, 4030,
        4031, 4032, 4033, 0, 4040, 0, 4050, 4051, 4052, 0, 4060, 0, 4070, 0, 4080, 4081,
        0, 4090, 4091, 4092, 4093, 4094, 0, 4100, 4101, 4102, 4103, 0, 4110, 4111, 4112, 0,
        4120, 4121, 0, 4130, 0, 4140, 0, 4150, 0, 4160, 4161, 4162, 0, 5010, 0, 5020,
        0, 5030, 0, 5040, 0, 5050, 0, 5060, 5061, 0, 5070, 0, 5080, 0, 5090, 5091,
        0, 5100, 0, 5110, 0, 5120, 0, 5130, 0, 5140, 0, 5150, 0, 5160, 0, 7010,
        7011, 7012, 0, 7020, 0, 7030, 0, 7040, 0, 7050, 0, 7060, 0, 7070, 7071, 0,
        7080, 0, 7090, 7091, 7092, 7093, 0, 7100, 7101, 7102, 0, 7110, 7111, 7112, 0, 7120,
        0, 7130, 0, 7140, 0, 7150, 0, 7160, 0, 8010, 0, 8020, 0, 8030, 0, 8040,
        8041, 0, 8050, 0, 8060, 0, 8070, 8071, 0, 8080, 0, 8090, 0, 8100, 0, 8110,
        0, 8120, 0, 8130, 0, 8140, 0, 8150, 0, 9010, 9011, 0, 9020, 9021, 9022, 0,
        9030, 9031, 9032, 0, 9050, 9051, 0, 9060, 0, 9070, 0, 9080, 9081, 0, 9090, 9091,
        9092, 0, 9100, 0, 9110, 0, 9120, 9121, 9122, 9123, 9124, 0, 10010, 10011, 0, 15000,
        0, 28010, 0, 28020, 28021, 28022, 0, 28030, 28031, 28032, 0, 28040, 0, 28050, 0, 28060,
        28061, 28062, 28063, 28064, 28065, 28066, 28067, 0, 28070, 28071, 0, 28080, 28081, 0, 28090, 0,
        28100, 0, 28110, 28111, 0, 28120, 0, 28130, 28131, 0, 101000, 101001, 0, 101010, 101011, 101012,
        101013, 101014, 101015, 0, 101020, 101021, 101022, 101023, 101024, 0, 101030, 101031, 101032, 101033, 101034, 101035,
        0, 101040, 101041, 101042, 101043, 101044, 0, 102000, 102001, 0, 102010, 0, 102020, 102021, 102022, 0,
        102030, 102031, 0, 102040, 102041, 0, 103000, 103001, 103002, 103003, 103004, 103005, 103006, 103007, 103008, 0,
        103010, 103011, 103012, 103013, 103014, 0, 103020, 103021, 103022, 103023, 103024, 103025, 103026, 103027, 103028, 103029,
        0, 103030, 103031, 103032, 103033, 0, 103040, 103041, 103042, 103043, 103044, 0, 103120, 103121, 103122, 0,
        103103, 103104, 103105, 103106, 103107, 103108, 0, 103130, 103131, 103132, 103133, 0, 103140, 103141, 103142, 103143,
        103144, 0, 103203, 103204, 103205, 103206, 103207, 103208, 0, 103220, 103221, 103222, 0, 103230, 103231, 103232,
        103233, 0, 103240, 103241, 103242, 103243, 103244, 0, 104000, 104001, 104002, 104003, 0, 104010, 104011, 104012,
        104013, 0, 104020, 104021, 0, 104030, 104031, 104032, 104033, 104034, 104035, 0, 105000, 0, 105010, 105011,
        105012, 105013, 0, 105020, 105021, 105022, 0, 105030, 105031, 105032, 105033, 105034, 105035, 0, 105040, 105041,
        0, 107000, 107001, 107002, 107003, 0, 107010, 0, 107020, 107021, 107022, 0, 107030, 107031, 0, 107040,
        107041, 0, 108000, 0, 108010, 108011, 108012, 108013, 0, 108020, 108021, 108022, 0, 108030, 0, 109000,
        109001, 109002, 109003, 109004, 109005, 109006, 109007, 0, 109010, 109011, 109012, 109013, 0, 109020, 109021, 109022,
        109023, 109024, 109025, 109026, 109027, 0, 109030, 109031, 109032, 0, 128000, 128001, 0, 128010, 128011, 0,
        128020, 128021, 128022, 128023, 0, 0, 0, 0, 0,
    };
    void draw_omake_cancel(int highlight);
    void open_cg_gallery();
    void open_music_room();
    void open_replay_gallery();
    void close_omake_screen();
    void draw_cg_gallery_page();
    void draw_cg_full(
        SDL_Texture* texture, float source_y, const SDL_FRect& destination,
        float alpha = 1.0f);
    float omake_cg_phase_progress(int frames) const;
    void draw_cg_gallery();
    void draw_music_room();
    static constexpr std::array<int, 40> music_room_tracks{
        0, 10, 29, 11, 12, 13, 14, 30, 27, 1,
        2, 4, 3, 5, 6, 8, 7, 9, 18, 37,
        38, 41, 42, 39, 40, 15, 16, 17, 19, 20,
        22, 32, 21, 23, 26, 31, 25, 24, 28, 50,
    };
    static constexpr std::array<std::array<int, 2>, 40> music_room_artists{{
        {3, 5}, {3, 3}, {4, 4}, {2, 2}, {1, 1},
        {2, 2}, {1, 1}, {0, 0}, {2, 2}, {2, 2},
        {0, 0}, {3, 3}, {0, 0}, {4, 4}, {4, 4},
        {2, 2}, {1, 1}, {1, 1}, {2, 2}, {6, 4},
        {6, 4}, {1, 1}, {0, 0}, {5, 5}, {0, 0},
        {3, 3}, {3, 3}, {3, 3}, {2, 2}, {2, 2},
        {1, 1}, {4, 4}, {2, 2}, {1, 1}, {2, 2},
        {3, 3}, {1, 1}, {1, 1}, {3, 3}, {2, 5},
    }};
    static constexpr std::array<int, 9> replay_flags{
        1, 2, 3, 4, 5, 7, 8, 9, 11,
    };
    static constexpr std::array<int, 9> replay_scripts{
        10, 20, 30, 40, 50, 70, 80, 90, 110,
    };
    static constexpr std::array<int, 9> replay_thumbnails{
        1000, 2010, 3000, 4000, 5000, 7010, 8000, 9000, 28000,
    };
    void draw_replay_gallery();
    void draw_title();
    void activate_title_item();
    void handle_title_input(const SDL_Event& event);
    void activate_cg_gallery_item();
    void handle_cg_gallery_input(const SDL_Event& event);
    void activate_music_room_item();
    void handle_music_room_input(const SDL_Event& event);
    void start_replay(int slot);
    void activate_replay_gallery_item();
    void handle_replay_gallery_input(const SDL_Event& event);
    void draw_save_load();
    int save_load_hit(float x, float y) const;
    bool save_load_item_enabled(int item) const;
    void ensure_save_load_focus();
    void move_save_load_focus(SDL_Keycode key);
    void activate_save_load_item(int item);
    void handle_save_load_input(const SDL_Event& event);
    void handle_system_menu_input(const SDL_Event& event);
    void change_map_field(int direction);
    void update_map_hover(float x, float y);
    void handle_map_input(const SDL_Event& event);
    void update_map();
    void draw_backlog();
    std::vector<SDL_FRect> backlog_voice_rects(
        const BacklogEntry& entry, int voice_index) const;
    void draw_sidebar();
    void update_sidebar_hover(float x, float y);
    // Handles a press on the sidebar.  With activate_buttons false the
    // buttons are left alone and only the draggable parts respond, which is
    // what a touch press does: the buttons wait for the release.
    bool handle_sidebar_click(float x, float y, bool activate_buttons = true);
    void set_backlog_from_sidebar_y(float y);
    void set_message_alpha_from_sidebar_y(float y);
    void finish_sidebar_drag();
    void handle_backlog_input(const SDL_Event& event);
    void draw_click_indicator();
    std::size_t message_visible_lines() const;
    int message_scroll_limit(std::size_t total_lines) const;
    void draw_scrollbar(
        std::size_t total_lines, int scroll, bool dragging);
    int scroll_from_y(float y, std::size_t total_lines) const;
    bool on_scrollbar(float x, float y) const;
    // Alpha of the black wash under the message text, from the configured
    // half tone.  The backlog shares it: the original shows the log in the
    // same window over the same darkened background.
    Uint8 message_backdrop_alpha() const;
    // The dimming behind the message window ramps in over sixteen steps and
    // is dropped in one go, the way AVG_ControlHalfTone() does it.
    static constexpr float half_tone_steps = 16.0f;
    float half_tone_pulse() const;
    void raise_half_tone();
    void update_half_tone();
    bool handle_message_scroll_press(float x, float y);
    void set_message_scroll_from_y(float y);
    bool handle_backlog_scroll_press(float x, float y);
    void set_backlog_scroll_from_y(float y);
    std::string_view backlog_view_text() const;
    std::vector<std::string> backlog_view_lines() const;
    void select_overlay();
    void begin_overlay();
    void select_sidebar();
    void clear_sidebar();
    void clear_authentic_text();
    void begin_authentic_text();
    void draw_overlay(std::size_t slot);
    float imgui_display_scale() const;
    void present_frame();
    void reset_render_state();
    // Hides the message window for a character animation, remembering to
    // put it back when the animation ends.
    void hide_message_for_animation();
    void ensure_shake_target();
    // Cross-dissolves two character poses through a scratch target, the way
    // the original blends the pair inside one sprite.  False when the
    // renderer cannot do it, so the caller falls back.
    bool ensure_pose_blend_target();
    bool draw_pose_dissolve(
        SDL_Texture* next, SDL_Texture* previous, float progress,
        Uint8 brightness, int alpha, const SDL_FRect& destination);
    void draw_frame();
    void draw();
};
}  // namespace th2app
