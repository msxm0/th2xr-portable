#pragma once
#include "archive.hpp"
#include "gl_readback.hpp"
#include "text_count.hpp"
#include "audio.hpp"
#include "character.hpp"
#include "config.hpp"
#include "font.hpp"
#include "asset_plan.hpp"
#include "frame_budget.hpp"
#include "image.hpp"
#include "texture.hpp"
#include "dsp.hpp"
#include "avg_back.hpp"
#include "trace.hpp"
#include "avg_msg.hpp"
#include "avg_char.hpp"
#include "gl_transition.hpp"
#include "gamepad_input.hpp"
#include "imgui_layer.hpp"
#include "message.hpp"
#include "persistent_state.hpp"
#include "predecode_cache.hpp"
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
#include <cstdio>
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

// What an ESC_WAIT opcode re-asks every frame until it can retire.  The
// engine gives each one exactly one question - a C waits on AVG_WaitChar and
// on nothing else - so a background fade cannot hold a character instruction
// and no instruction has to be resumed by whoever finished its wait.
enum class WaitKind {
    none,           // ESC_NOWAIT: the machine runs straight on to the next
    frame,          // ESC_WAIT with no predicate: one frame, then on
    character,      // !AVG_WaitChar( EscParam[0].num )
    novel_message,  // AVG_WaitNovelMessage()
    back,           // !AVG_WaitBack()
    fade,           // !AVG_WaitFade()
    back_fade,      // !AVG_WaitBackFade()
    shake,          // !AVG_WaitShake()
    back_scroll,    // !AVG_WaitBackScroll()
    key,            // AVG_WaitKey()
    bgm,            // AVG_WaitBGM()
    se,             // !AVG_WaitSe( EscParam[0].num )
    voice,          // AVG_WaitVoice( EscParam[0].num )
    movie,          // !AVG_WaitMovie()
    select,         // AVG_WaitSelect() != -1
    wait_frame,     // AVG_WaitFrame()
    clock,          // AVG_ViewClock() / AVG_SetCalender()
    title,          // AVG_SetGotoTitle's fade, then the release frame
};

// WAIT_STRUCT, the counter behind AVG_SetWaitFrame / AVG_WaitFrame.  A frame
// count, not a duration: the original never looks at a clock for this.
struct WaitFrameState {
    int flag = 0;
    int type = 0;
    int count = 0;
    int max = 0;
};

// Set by --cpu-transitions.  Forces the CPU blend even where the shader
// would work, so the two paths can be compared without rebuilding.
extern bool force_cpu_transitions;
extern int web_frame_pacing;
// --trace-prefetch: report what the lookahead guessed against what was used.
extern bool trace_prefetch;

std::filesystem::path writable_directory();
std::filesystem::path profile_directory();
// `may_prompt` is false for a run with no one in front of it - a trace, a
// soak - where the file picker is not a fallback but a hang: the dialog is
// modal, nothing answers it, and a sweep that takes an hour sits on it until
// someone notices.  Such a run fails with the path it looked in instead.
std::optional<std::filesystem::path> discover_game_data_path(
    const std::filesystem::path& default_path, bool explicit_path,
    bool may_prompt = true);
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
// One engine tick.  MAIN_Loop steps next_time by 1000/60 - integer divided,
// so 16 ms - and the original therefore ran at 62.5 ticks a second, not 60.
// Every wait the scripts count in ticks was timed against that.
inline constexpr int engine_tick_ms = 1000 / 60;
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
    void run_tick();
private:
    // 27: the engine's NovelBuf replaces the port's backlog.
    static constexpr std::uint32_t save_version_ = 27;
    static constexpr std::uint32_t oldest_supported_save_version_ = 27;
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
    // The tick a trace run's audio wait began on, or 0 for none.  A real
    // channel stops when the samples run out, which is a wall clock: the
    // same SEW would cost a different number of ticks on a fast machine
    // than on a slow one.  The reference has exactly the same problem and
    // solves it the same way - see th2ref_pcm_play - so both sides agree
    // that a sound lasts trace_sound_ticks and nothing looks at a clock.
    static constexpr std::uint64_t trace_sound_ticks = 50;
    // Keyed the same way AudioWait::channel is, and stamped when the sound
    // *starts* rather than when something waits for it.  That distinction is
    // the whole point: a script plays an effect with SEP and waits for it
    // several opcodes later with SEW, by which time most of its length has
    // already gone by.  Timing the wait from the SEW instead made ours hold
    // for the full fifty ticks where the reference held for fourteen.
    std::unordered_map<std::size_t, std::uint64_t> trace_sound_started_;
    void note_sound_started(std::size_t channel);
    // Audio as events.  The reference decodes nothing, so there is no
    // waveform to compare against - what is comparable is which sound it
    // asked for, on which channel, on which tick.  Argument order matches
    // the reference's AVG_Play* probes so the two logs diff directly.
    void trace_note_audio(const char* kind, int a, int b, int c, int d,
                          const char* name = nullptr);
    // Is a traced effect still running on this channel?  AVG_WaitSe reads
    // SeStruct[sno], which on the other side is fed by the stub in
    // reference/shim - a tick count, not a decoder.  Asking our real decoder
    // whether it is playing is a different question with a different answer.
    bool trace_se_playing(std::size_t channel) const;
    // A traced movie, which is not decoded on either side.  See
    // Game::start_movie for why, and for what TH2_MOVIE_TICKS does and does
    // not change.
    bool trace_movie_live_ = false;
    std::uint64_t trace_movie_started_ = 0;
    static std::uint64_t trace_movie_ticks();
    struct CharacterTexture {
        int pose = -1;
        Texture texture;
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
        int nuki = -1;
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

    // Render targets from finished transitions, for the next one.
    std::vector<Texture> spare_frame_targets_;
    void end_transition();
    void renew_wipe_backdrop(SDL_Texture* target);
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
        // AVG_SetBack takes its snapshot only `if(back_max)`, and back_max is
        // AVG_EffCnt(fd_max) as it stands at that moment - zero under a held
        // skip key.  See begin_transition.
        bool no_snapshot = false;
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
        // SHAKE_ZOOM alone sets DRW_BLD(128) on the background while it
        // runs, so the zoomed picture is composited at half strength.
        bool half_blend = false;
        // The engine's own units, for the graph setters.  DSP_SetGraphZoom2
        // takes 256ths above one-to-one and DSP_SetGraphRoll takes a turn in
        // 256 steps, so carrying degrees and a scale factor alongside them
        // would mean converting back and losing the integer arithmetic.
        int zoom_256 = 0;
        int roll_rate = -1;   // -1 when this shake does not roll
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
    // One SPRITE_OPR: code (0 NULL, 1 DRAW, 2 LOOP, 3 REPEAT), data1..3.
    struct SpriteOperation {
        int code = 0;
        int data1 = 0;
        int data2 = 0;
        int data3 = 0;
    };
    struct MapCharacter {
        Texture texture;
        std::vector<std::vector<MapSpritePart>> frames;
        std::vector<MapSpriteStep> steps;
        // Sprite 0's program, NULL-terminated, and ANIME_STRUCT.frame.
        std::vector<SpriteOperation> program;
        int rate = 60;
    };
    // The chip SPR_RenewSprite has sprite 0 of `animation` on after
    // `renews` calls, from SPR_SetSprite's fresh ANIME_CONTROL.
    static int sprite_frame_after(const MapCharacter& animation, int renews);
    // A frame count, not a duration.  AVG_ViewClock counts its own frames -
    // sixteen fading in, one per six minutes of travel, sixteen fading out -
    // and never asks what time it is.  Deriving the frame from engine_now()
    // instead put it 4% slow, because that clock steps in integer 1000/60 ms,
    // so sixty of its milliseconds-worth is 0.96 frames: the clock at pc 3748
    // of 010301110.sdt held the script 71 frames where the engine holds it 66.
    struct ClockState {
        int target = 0;
        int start_minutes = 0;
        int target_minutes = 0;
        int travel_frames = 0;
        int frame = 0;
    };
    struct CalendarState {
        int month = 0;
        int day = 0;
        int weekday = 0;
        int holiday = -1;
        bool dismissing = false;
        int frame = 0;
        // AVG_CALENDER: the day-change calendar AVG_ToHertDaySinkou puts up
        // runs as a step of AVG_Main of its own, which calls nothing but
        // AVG_SetCalender - not AVG_ControlSystem2, so Avg.msg_cut stays
        // what it was.  ViewCalender, the opcode, shows the same page from
        // inside AVG_GAME and is not one.
        bool step = false;
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
    SDL_AudioDeviceID audio_keepalive_ = 0;  // see the constructor's end
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
    // BMP_BACK is the background as loaded plus whatever characters have
    // been composited into it; BMP_BACK2 is the clean copy taken before any
    // of them were, and BMP_BACKHALF the darkened copy of the plate.  All
    // three belong to the display layer now - the game asks it for them.
    bool has_background() const;
    void load_background_bitmap(Texture texture);
    // BackStruct.bno under another name.  AVG_SetBack stores the scene
    // number in the struct and AVG_ResetBackHalfTone's early-out compares
    // against it, so the two must not drift; set_bg_scene() is the only
    // place either is written.
    int bg_scene_ = -1;
    void set_bg_scene(int scene);
    BackgroundKind background_kind_ = BackgroundKind::background;
    BackgroundView background_view_;
    std::optional<BackgroundScroll> background_scroll_;
    std::array<OverlayState, 32> overlay_states_{};
    // The original's display layer and character machine.  display_ owns
    // the graph table; avg_char_ is CharStruct[MAX_CHAR] and AVG_ControlChar.
    // Both need the renderer, so they are built in the constructor body.
    std::optional<th2::Display> display_;
    std::optional<th2::AvgChar> avg_char_;
    // BackStruct and the AVG_ControlBack* chain.  Every background effect
    // counts in whole frames here, and AVG_EffCnt is re-asked on each pass,
    // which is what lets the skip key collapse one mid-flight.
    std::optional<th2::AvgBack> avg_back_;
    // NovelMessage and HalfTone, and AVG_ControlNovelMessage.
    std::optional<th2::AvgMsg> avg_msg_;
    th2::Display& display() { return *display_; }
    const th2::Display& display() const { return *display_; }
    th2::AvgChar& chars() { return *avg_char_; }
    const th2::AvgChar& chars() const { return *avg_char_; }
    th2::AvgBack& avgback() { return *avg_back_; }
    const th2::AvgBack& avgback() const { return *avg_back_; }
    th2::BackStruct& back() { return avg_back_->back(); }
    const th2::BackStruct& back() const { return avg_back_->back(); }
    th2::AvgMsg& msg() { return *avg_msg_; }
    const th2::AvgMsg& msg() const { return *avg_msg_; }
    void build_display();
    // AVG_ControlChar's view of BackStruct, and the calls it makes outward.
    th2::AvgChar::Hooks character_hooks();
    th2::AvgBack::Hooks background_hooks();
    th2::AvgMsg::Hooks message_hooks();
    // AVG_GetGameKey's output, sampled once at the top of the frame.
    th2::GameKey game_key_{};
    th2::KeyCond key_cond_{};
    void get_game_key();          // AVG_GetGameKey
    void control_system2();       // AVG_ControlSystem2
    // AVG_ControlChar runs at sixty frames a second in the original and
    // counts in whole ones.  On a faster display the AVG_Control* pass has
    // to be stepped by elapsed time instead, and this is the accumulator
    // that does it for the whole chain rather than for characters alone.
    int control_ticks_due();
    // AVG_ControlBack, stepped the same number of sixtieths as the rest of
    // the chain.
    void update_avg_back(int steps);
    // Points BMP_BACK / BMP_BACK2 at the plates the game still owns, so a
    // character can bake into them before the background is a graph itself.
    // AVG_SetBackPos and the cases of AVG_ControlShake that transform
    // GRP_BACK, applied to the background graph and the darkened copy that
    // has to travel with it.
    // True while a wipe that the display layer can express as graph
    // parameters is running.  AVG_ControlBackChange parks the outgoing
    // snapshot in GRP_BACK+1 at LAY_BACK, lifts GRP_BACK to LAY_BACK+1 and
    // then only ever calls DSP_SetGraphParam, DSP_SetGraphZoom2 or
    // DSP_SetGraphMove on the two of them - no blending of its own.  The
    // pattern wipes are the exception: they need their mask.
    bool transition_drives_graphs() const;
    // AVG_ControlBackChange's per-frame half, for the types above.
    void control_back_change();
    // How far through the running wipe we are, 0..1.  Zero when none is.
    float transition_progress() const;
    void setup_background_graphs(
        const ShakeSample& shake, bool shake_background,
        bool shake_characters);

    std::array<CharacterTexture, 32> character_textures_{};
    bool background_baked_dirty_ = true;
    void rebuild_baked_background();
    // AVG_CopyBack(OFF): BMP_BACK <- BMP_BACK2, creating the plate if it is
    // not there yet.  Returns false when there is no background to copy.
    bool copy_back_plate();
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
    void warm_renderer_programs();
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
    void update_image_decode();
    void report_prefetch_trace();
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
    SDL_Point sakura_large_pos_{};
    SDL_Point sakura_small_pos_{};
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
    // Every script, decompressed once at startup and kept.
    //
    // The walk follows jumps, so it needs whatever script a branch leads to,
    // and decompressing one on demand put an LZS pass on the frame that was
    // drawing - repeatedly, because the cache that held them was eight
    // entries cleared wholesale on overflow, and a scene with nine reachable
    // destinations re-decompressed all of them every scan.  All 1275 of them
    // come to 7.1MB, against a container that is already held whole at
    // 3.1MB, so there is nothing to gain by being clever about which to keep.
    std::unordered_map<std::string, std::vector<std::uint8_t>>
        script_bytecode_;
    void preload_scripts();
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
    th2::PredecodeCache<Surface> decoded_images_;
    // The nearest few the latest scan found, in order.  Work happens on the
    // front one until it is finished, then the next.
    std::deque<std::string> image_decode_queue_;
    // Five backgrounds and five sprites: enough for the next screen whichever
    // branch is taken, and no more.  An unbounded sprite lookahead decoded
    // nine times as much as it used, most of it discarded.
    // Opening a Vorbis stream builds its VLC tables - about six milliseconds,
    // atomic, and three times the whole frame allowance.  The budget cannot
    // interrupt it, so the only control is how often it is allowed to happen.
    static constexpr int audio_opens_per_frame = 1;
    int audio_opens_this_frame_ = 0;

    // What the lookahead asked for against what was actually used.  A guess
    // that is never used costs a decode and a cache slot, and at the scale
    // this runs at a wrong guess repeated every line is invisible in a
    // profile but ruinous in aggregate - so it is counted rather than
    // reasoned about.
    struct PrefetchTrace {
        int audio_created = 0;
        int audio_recreated = 0;
        int image_queued = 0;
        int image_decoded = 0;
        int image_used = 0;
        int image_evicted_unused = 0;
        // An asset the engine asked for that no recent scan had named.
        // This is the explorer's coverage, which is the thing to look at if
        // the prefetcher keeps missing: fetch counts say how much it
        // fetched, this says how much of what was wanted it saw coming.
        int unpredicted_uses = 0;
    };
    PrefetchTrace trace_{};

    // What the script will want next, graded by how soon.  Rebuilt by each
    // scan; everything not in it has been passed and is first to go.
    th2::AssetPlan plan_;
    // How far ahead the walk goes, in instructions, along any one path.
    static constexpr int scan_depth_limit = 200;
    // Depth of the instruction being looked at, so assets it names are graded
    // by how far the walk had to come to reach them.
    int scanning_depth_ = 0;
    // The byte-store key for each planned asset, captured when it is
    // requested.  data_prefetch() ignores a repeat request, so this is the
    // only way the store can be told what a range is worth now rather than
    // what it was worth when it was fetched.
    // Everything one scan decided it wants, keyed by store key so the same
    // range named twice keeps its nearest depth.
    struct WantedRange {
        std::string path;
        std::uint64_t offset = 0;
        std::size_t size = 0;
        int depth = 0;
    };
    std::unordered_map<std::string, WantedRange> scan_wants_;
    void submit_scan_wants();

    // Everything one scan found, whatever layer it is already sitting in.
    // The predecoders work from this rather than from what still needs
    // fetching: an asset already in the byte store is exactly the one worth
    // decoding early, and leaving it out was how they ended up working on
    // whatever happened to miss instead of on what comes next.
    struct ScanAsset {
        std::string key;                 // cache key, prefixed for images
        std::string name;                // the asset's own name
        const th2::Archive* archive = nullptr;   // audio only
        bool background = false;         // images only
        bool audio = false;
        int depth = 0;
    };
    std::vector<ScanAsset> scan_assets_;
    // Bumped once per scan.  An entry carries the generation that last named
    // it, so how stale it is falls straight out of the difference.
    int scan_generation_ = 0;
    void update_predecode_queues();
    // Depth-first from the interpreter, following branches and script loads,
    // stopping any path at scan_depth_limit instructions.
    void explore(std::span<const std::uint8_t> bytecode,
                 std::span<const std::int32_t> registers,
                 const std::string& script, std::size_t offset, int depth,
                 int& requests, int request_budget,
                 std::unordered_set<std::string>& visited);
    std::span<const std::uint8_t> script_prefix_bytecode(
        const std::string& name);
    // Names seen before, so re-creating a decoder for one already decoded and
    // dropped can be told apart from meeting it for the first time.
    std::unordered_map<std::string, int> audio_seen_;
    std::chrono::steady_clock::time_point last_trace_report_{};

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
    float half_tone_count_ = 0.0f;
    // AVG_SetHalfTone() is called when a message is set, not every frame the
    // message happens to be up - so the wash is armed by a line arriving and
    // stays off until one does.  A transition disarms it: the engine hands
    // GRP_BACK+1, the graph the darkened copy lives in, to the outgoing
    // background for the duration, and resets it when the wipe ends.
    bool half_tone_armed_ = false;
    bool half_tone_fading_ = false;
    std::chrono::steady_clock::time_point half_tone_updated_{};
    std::array<float, 3> background_brightness_{128.0f, 128.0f, 128.0f};
    std::chrono::steady_clock::time_point skip_next_time_{};
    std::string current_line_key_;
    std::size_t text_reveal_start_ = 0;
    // The counter has reached NovelMessage.max (count + 8): the page is
    // "done" as far as input and the click indicator are concerned.
    bool text_reveal_complete_ = true;
    // The per-glyph ramp has finished too, sixteen counts after the last
    // glyph started fading in.  Later than the above, and only this one may
    // switch the fade off - conflating them either snapped the tail to solid
    // or held the indicator back, depending on which threshold was used.
    bool text_fade_complete_ = true;
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
    // SelectWindow.cnt.  A count of characters, stepped once per control
    // pass by AVG_MsgCnt() - not a span of milliseconds.  It used to be a
    // timestamp that choice_reveal_count divided into engine_now(), which
    // at text_speed_ms=20 advanced 16/20 of a character a tick where the
    // engine advanced a whole one, so every choice took a quarter longer to
    // become answerable than the reference's did.
    int choice_reveal_cnt_ = 0;
    // The glyph string as it stood when the frame was DRAWN, kept only for
    // the frame a choice is answered on.  The reference records glyph alpha
    // from inside TXT_DrawTextEx - what actually went to the screen - while
    // ours recomputes it at dump time, after the control pass.  Normally
    // those agree; on the frame AVG_ControlSelectWindow answers a choice it
    // sets NovelMessage.disp to 0 *after* the draw, so the reference reports
    // disp 0 with the glyphs still on screen and ours reported no glyphs at
    // all.  The draw really did happen on both sides - only the dump differed.
    std::string trace_glyph_drawn_;
    // This tick's DSP_GetTextDispPos( TXT_WINDOW ): see trace_dump_state.
    std::string trace_glyph_measured_;
    // This frame was not drawn: see th2::engine_draw_flag.
    bool frame_undrawn_ = false;
    // A tick ran this frame.  Without one the art target is left as the
    // last tick drew it (see draw_frame).
    bool tick_this_frame_ = true;
    // Desktop: SDL_RenderPresent waits for the display (no sleep pacing).
    bool vsync_paced_ = false;

    // --- drawing between ticks (src/game_subtick.cpp) ------------------
    //
    // How far past the last tick this frame is drawn, 0..1; 0 on a tick, in
    // a trace, and with the option off.
    double presentation_phase() const;
    // Whether the next tick steps the scene's ramps, or the menu's - the
    // only things a frame between ticks may draw further along.
    bool subtick_scene_steps() const;
    bool subtick_menu_steps() const;
    // Display's presenter: the in-between values for one graph's copy.
    void present_graph(int gno, th2::Graph& graph) const;
    bool present_engine_config_graph(
        int gno, double phase, th2::Graph& graph) const;
    // Set only while a frame between ticks is being drawn; everything the
    // engine asks sees 0.
    double presentation_phase_ = 0.0;
    double scroll_present_extra_ = 0.0;
    double choice_present_extra_ = 0.0;
    bool present_scene_ = false;
    bool present_menu_ = false;
    bool subtick_drawn_ = false;   // the copy was drawn this frame
    // The art target's twin a frame between ticks is drawn into, so the art
    // target keeps exactly what the last tick drew.
    Texture subtick_art_;
    SDL_Texture* ensure_subtick_art();
    void end_presentation();
    void trace_dump_subtick_frame();
    std::string trace_glyph_string() const;
    bool trace_choice_answered_ = false;
    int choice_highlight_ = 0;
    int choice_selected_ = -1;
    int choice_result_register_ = -1;
    bool choice_ex_ = false;
    // --- UI State ---
    // Where a new game starts.  The title screen's scan walks it too, so the
    // first scene is fetched and decoded before the click that starts it.
    static constexpr const char* new_game_script = "EV_0301MORNING.SDT";
    bool title_scanned_ = false;
    enum class UiMode {
        title,
        cg_gallery,
        music_room,
        replay_gallery,
        game,
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
    // First visible line when a page of text is taller than the screen.
    int message_scroll_ = 0;
    bool message_scroll_follow_ = true;
    bool message_scroll_dragging_ = false;
    // NovelMessage.disp: the engine's own switch for the text and the bar,
    // which it turns off for a background fade or a character animation and
    // back on with the next message.  Only the engine writes it.
    bool message_visible_ = true;
    // Hiding the window is the engine's too now - GameKey.diswin and the
    // bar's button, MSG_SYSTEM - so this is the whole of it.
    bool message_shown() const { return message_visible_; }
    // Message.wstep: whether the message *window* is up, which is a
    // different thing from whether the text is shown.  AVG_GetWindowCond
    // reads this, and AVG_ControlChar uses it to know the window was open
    // before an animation started so it can put it back afterwards.
    bool message_window_open_ = true;
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
    // long DefaultCharName = 1, mirrored into ESC_FlagBuf[_DEFAULT_NAME]
    // (flag 5).  The engine does not work this out by comparing names: it
    // starts at 1, the name dialog writes it when the player presses OK, and
    // a load takes it back out of flag 5.  Deriving it instead from
    // player_name_ == default_player_name_ broke on the English patch, where
    // load_default_player_name reads romanised names out of the executable
    // while a loaded save still holds the Japanese ones - so every line that
    // names a character took the "this guy" branch the reference never took.
    // Found at pc 564 of 070000501.sdt, a GetFlag 5 feeding an IfV.
    int default_char_name_ = 1;
    th2::PlayerName player_name_;
    std::array<char, 64> name_family_{};
    std::array<char, 64> name_given_{};
    std::array<char, 64> name_family_reading_{};
    std::array<char, 64> name_given_reading_{};
    std::array<char, 64> name_nickname_{};
    bool auto_mode_ = false;
    bool skip_mode_ = false;   // Avg.msg_cut_mode, the toggle
    bool skip_held_ = false;   // Avg.msg_cut, the key
    // EOprFlag[] from Escript.cpp, indexed by opcode the way the original
    // indexes it by ESC_ id.  Non-zero means this instruction has already run
    // its set-up and is only re-asking its wait; the four background opcodes
    // count to two, because they clear the old half tone a frame before they
    // set the new picture.
    std::array<std::uint8_t, 256> eopr_flag_{};
    // Which of those phases handle() is being called for.
    int opcode_phase_ = 1;
    bool demo_mode_ = false;
    // AVG_SetGotoTitle stopped the script (MAIN_SetScriptFlag(OFF)) and left
    // the fade to run; the title itself comes once that fade is done.  Ours
    // used to switch straight to the title and walk on to End, so the route
    // ended thirty frames early and never went back to the title at all.
    bool goto_title_pending_ = false;
    // The frame between the title fade ending and the script being let go.
    // Release is its own frame here as it is for the map and the calendar.
    bool goto_title_released_ = false;
    bool replay_mode_ = false;
    int demo_delay_frames_ = 0;
    std::vector<MapEvent> map_events_;
    int map_field_ = 1;
    int map_previous_field_ = 1;
    int map_hover_ = -1;
    int map_slide_ticks_ = 0;
    // |EventFieldRoolCount| before this frame's roll step, 0 if none ran.
    int map_marker_roll_ = 0;
    // AVG_ControlMapEvent's select_back (a function static, so it outlives
    // each map) and the pointer it reads select from every frame.
    int map_select_back_ = 0;
    // The arrow clicked this frame or rolling since (-2 left, -3 right).
    int map_arrow_pressed_ = 0;
    // The frame's text-shake offset, re-applied after each target change.
    SDL_Point text_shake_{};
    // MouseCheck[10][0..15].flag: every destination's rect is switched on
    // when the map is set up, and step 3 narrows them to the page shown -
    // after it has read select for the frame.
    std::array<bool, 16> map_rect_on_{};
    int map_anim_frames_ = 0;
    // map_anim_frames_ on the frame the characters' sprites were set.
    int map_sprite_start_ = -1;
    float map_pointer_x_ = 0.0f;
    float map_pointer_y_ = 0.0f;
    int map_fade_ticks_ = 0;
    int map_selected_ = -1;
    // Where the trace script's pointer is, and the map driven from it.  The
    // map is the one screen a trace cannot answer with a key: it is picked by
    // the pointer being inside a destination's rect when a click lands, and
    // our map only ever listened to SDL events, which a trace does not
    // produce.
    // AVG_ControlMapEvent's step 2: the map fades in over sixteen frames
    // before it will take input.  Ours drew a fade off steady_clock and
    // gated nothing, so the screen was answerable on its first frame.
    // No script running, as opposed to one parked at its last offset.  The
    // engine's state dump is
    //     const DWORD pc = EXEC_LangInfo ? EXEC_LangInfo->pc : 0;
    // so a zero there means there is no script at all - which is what a
    // finished script waiting on the map screen is.  Ours kept the VM on the
    // ended script's last offset and reported that, so every tick of a map
    // read as a divergence.
    // The map's step 5: releasing the map and loading what was chosen is its
    // own frame, after the sixteen the fade takes.  Ours did both on the
    // fade's last frame and so left the screen early.
    bool map_finish_pending_ = false;
    // AVG_SetCalender's step 4: releasing the page is its own frame, after
    // the sixteen step 3 spends fading it out.  Same shape as the map's
    // step 5 - the engine consistently separates "the animation ended" from
    // "the script may move on".
    bool calendar_finish_pending_ = false;
    // A frame wait, counted in frames.  It used to be turned into
    // milliseconds (wait_value * 1000 / 60) and compared against
    // engine_now(), which itself steps by 1000/60 == 16 integer-divided
    // milliseconds a tick - so a twelve frame wait needed 200ms against
    // 16ms ticks and took thirteen.  Every WaitFrame in the game was one
    // tick long, which is what held pc 148 of 070000200.sdt a tick past the
    // engine.  The sixth wall-clock bug of its kind here: audio fades, the
    // choice reveal, the clock, the map's animations, its entry fade, this.
    int wake_frames_ = 0;
    // Sound start ticks read out of a checkpoint, held until trace_tick_ has
    // been restored: they are stored as ages, so they mean nothing until the
    // tick they are relative to is back.
    std::vector<std::pair<int, int>> pending_sound_ages_;
    bool script_ended_ = false;
    int map_enter_ticks_ = 0;
    // True on the frame the entry fade spent its last tick.  The map ignores
    // input while it is fading in, and the frame that finishes the fade is
    // one of those frames - the counter is already zero by the time the
    // input gate reads it, so without this the gate opens a frame early.
    // Same mistake as the calendar's dismiss check, and it costs the same:
    // a click landing on that frame is one the reference cannot see, and
    // with input arriving every thirty ticks that is thirty ticks of drift,
    // measured at the April 20th map where ours picked at tick 610320.
    bool map_enter_finished_this_frame_ = false;
    int trace_mouse_x_ = 0;
    int trace_mouse_y_ = 0;
    // Live input with no name in a script, held for the next tick's sample:
    // wheel notches (KeyCond.wheel), the middle button's edge, and a bar
    // button a gesture stands in for (-1 for none).
    int live_wheel_ = 0;
    bool live_middle_ = false;
    int live_bar_button_ = -1;
    bool half_tone_unsaved_ = false;
    // MUS_SetMousePos during a replay: where the engine put the pointer, and
    // the script position it replaced (the warp lasts until that changes).
    struct PointerWarp {
        int x = 0, y = 0;
        int from_x = 0, from_y = 0;
    };
    std::optional<PointerWarp> pointer_warp_;
    int script_mouse_x_ = 0;
    int script_mouse_y_ = 0;
    void warp_pointer(int x, int y);
    // Whether the pointer last came from a finger (the touch-synthesised
    // mouse), and which shape the choice rects were last given for it.
    bool pointer_from_touch_ = false;
    // The finger on the touch-synthesised mouse is holding the engine bar's
    // button down (see begin_touch_drag).
    bool touch_bar_press_ = false;
    bool choice_rects_for_touch_ = false;
    void register_choice_rects(bool touch);
    void trace_drive_map(bool pick);
    void trace_map_follow_pointer();
    void trace_peek_map_pointer();
    void trace_map_pick_position(int& x, int& y) const;
    std::chrono::steady_clock::time_point map_tick_{};
    std::chrono::steady_clock::time_point map_started_{};
    std::optional<th2::ReadMarker> current_read_marker() const;
    bool current_text_is_read() const;
    void mark_current_text_read();
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
    void begin_clock(int requested, int first_frame = 1);
    void begin_calendar(int month, int day);
    void update_clock_calendar();
    void control_calendar_key();
    static int sprite_blend_mode(SDL_Texture* texture);
    void draw_sprite_frame(
        const MapCharacter& animation, int frame, float x, float y,
        int level = 256);
    // `mode` -1 picks from the texture (3 for a premultiplied 32 bit
    // bitmap, else 0); `bright` is a BrightTable level, 128 neutral.
    void draw_engine_blit(SDL_Texture* texture, const SDL_FRect& source,
                          const SDL_FRect& destination, int level,
                          int mode = -1, int bright = 128);
    void draw_clock_calendar();
    Texture load_sakura_texture(std::string_view name);
    void start_sakura(int amount, bool no_reset);
    void stop_sakura(bool force);
    int seasonal_background_scene(int scene) const;
    void update_background_sakura(int scene, bool background);
    std::uint32_t next_sakura_random();
    void spawn_sakura_petals();
    void update_sakura(int steps);
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
    std::string choice_engine_text(int index) const;
    th2::TextCount choice_layout(int index) const;
    float choice_row_height(int index) const;
    float choice_row_y(int index) const;
    std::vector<std::string> choice_lines(
        const Choice& choice, int index) const;
    float message_text_width() const;
    // Scales an effect's frame count by the effect speed setting; 0 makes
    // it instant, which is what the original does while skipping too.
    // AVG_GetMesCut(): the message is being skipped, so every effect the
    // engine measures with AVG_EffCnt collapses to nothing.
    bool message_cut() const;
    // Escr.h's EScroptOpr[].ret: true for an opcode the engine's virtual
    // machine stops on for the rest of the frame.
    static bool opcode_yields_frame(std::string_view name);
    // The one question that opcode re-asks each frame.
    static WaitKind opcode_wait_kind(std::string_view name);
    // How many frames of setting-up it does before it starts asking.
    static int opcode_phases(std::string_view name);
    // The predicate itself: true while the instruction must stay put.
    // Not const: AVG_WaitFrame() counts a frame every time it is asked,
    // which is the whole of how it measures one.
    bool opcode_waiting(WaitKind kind, const th2::Event& event);
    // BOOL AVG_WaitFrame( void ).  True on the frame the wait retires.
    bool avg_wait_frame();
    WaitFrameState wait_frame_{};
    int effect_frames(int frames) const;
    // AVG_EffCnt3: 30fps units, and the one that ignores the skip key.
    int effect_frames3(int frames) const;
    // GlobalCount / GlobalCount2: free-running frame counters the engine
    // phases the laster wipe and the click indicator off.  Stepped once per
    // sixtieth in the AVG_Control* pass, like everything else.
    int global_count_ = 0;
    int control_steps_ = 0;
    // --trace: the engine as a pure function of a tick counter and a script.
    // Nothing may read a clock, exactly one control tick happens per frame,
    // and the 800x600 art layer is written out for comparison against the
    // reference build's dump of the same tick.
    bool trace_mode_ = false;
    std::filesystem::path trace_dir_;
    TraceScript trace_script_;
    // --record: live input, captured as a script while it is played.  Until
    // record_from_ the base script (--trace-input) drives, unpaced; from it
    // on the player does, at sixty ticks a second.
    TraceRecorder recorder_;
    std::uint64_t record_from_ = 0;
    bool record_live() const
    {
        return recorder_.recording() && trace_tick_ >= record_from_;
    }
    // The player's hands drive the engine: normal play, and a recording
    // past its lead-in.  Only a scripted replay does without them.
    bool live_input() const { return !trace_mode_ || record_live(); }
    // Whether a press may become engine input now.  Not while one of the
    // port's own screens is up over the scene - the save and load screens,
    // the ImGui panels, a movie - which take their input from SDL directly.
    bool engine_input_open() const;
    void record_input_event(const SDL_Event& event);
    bool handle_host_key(const SDL_Event& event);
    bool handle_text_scroll_drag(const SDL_Event& event);
    std::uint64_t trace_tick_ = 0;
    std::uint64_t trace_last_tick_ = 0;
    // Frames before this tick are computed but not written.  A frame is
    // 1.37MB and the replay to a divergence deep in a scenario is thousands
    // of ticks, so dumping the whole run to look at thirty of its frames is
    // most of the cost of a fix-and-retest cycle.  The state trace is not
    // windowed: it is one short line per tick and it is what the alignment
    // is read off.
    std::uint64_t trace_first_tick_ = 0;
    // --trace-hold: stop dead on this tick for a few seconds, so the window
    // can be photographed at a known point in the run.  A trace otherwise
    // goes past in a second and there is nothing to look at.
    std::uint64_t trace_hold_tick_ = 0;
    int trace_hold_seconds_ = 0;
    // Checkpoints, so a comparison window deep in the game does not cost a
    // replay of everything before it.  A segment saves at one tick and the
    // next segment resumes from that file, which makes a sweep linear in the
    // length of the game instead of quadratic in the number of windows.
    //
    // The resume waits for the lead-in to put the engine in the scenario,
    // exactly as the reference's does, and then moves the trace clock to the
    // tick the save was taken at - the scripted input and the dump window
    // are keyed to absolute ticks, so a resumed run has to answer to the
    // same numbers as the straight-through run it stands in for.
    std::filesystem::path trace_save_file_;
    std::uint64_t trace_save_tick_ = 0;
    std::filesystem::path trace_resume_file_;
    std::uint64_t trace_resume_trigger_ = 0;
    bool trace_resume_pending_ = false;
    void trace_checkpoint_save();
    void trace_checkpoint_resume();
    void trace_apply_input();
    void trace_dump_state();
    std::string trace_glyph_alpha() const;
public:
    // The clock the script machinery waits on.  Real time normally; in a
    // trace run, tick * (1000/60) from the epoch - the same integer step the
    // reference's th2ref_time() hands the engine, so a wait of N
    // milliseconds costs both sides the same whole number of ticks.
    //
    // This exists because a trace tick is a unit of engine progress rather
    // than of time.  With a real clock the run was only accidentally
    // deterministic: it worked while the loop happened to be paced at sixty
    // ticks a second, and the moment that sleep came out, a WaitFrame 90 sat
    // there for fifteen times as many ticks.
    std::chrono::steady_clock::time_point engine_now() const;
    void set_trace_hold(std::uint64_t tick, int seconds);
    void enable_recording(const std::filesystem::path& path,
                          std::uint64_t from);
    // Write a checkpoint at the end of `tick`, and resume from `file` once
    // the lead-in has reached `trigger`.  Either may be left unset.
    void set_trace_checkpoint(const std::filesystem::path& save_file,
                              std::uint64_t save_tick,
                              const std::filesystem::path& resume_file,
                              std::uint64_t resume_trigger);
private:
    std::FILE* trace_state_ = nullptr;
public:
    // Called before run() when --trace is given.  Throws if the script
    // cannot be read or names a key the reference does not know.
    // `lead` is how many ticks the reference spends before the point our
    // trace starts at.  Free-running counters - GlobalCount above all, which
    // drives the click indicator's thirty frame spin - count from process
    // start, not from the scenario's, so without it the two runs are in
    // phase on everything the script drives and out of phase on everything
    // it does not.
    void enable_trace(const std::filesystem::path& dir,
                      const std::optional<std::filesystem::path>& input,
                      std::uint64_t ticks, std::uint64_t first = 0,
                      std::uint64_t lead = 0);
private:
    void trace_dump_frame();
    // The menu cross-fades use begin_transition's machinery but are not
    // AVG_SetBack, so they keep their own clock rather than BackStruct's.
    int menu_transition_frames_ = 0;
    std::chrono::steady_clock::time_point menu_transition_started_{};
    // AVG_WaitSe cuts the sound when the skip key is down.  It is asked from
    // a const predicate, so the stop happens on the way out of the VM.
    mutable bool se_cut_pending_ = false;
    // AVG_EffCnt4: the same count in 30fps units, unscaled by the effect
    // speed, and nothing at all while skipping.
    int effect_frames4(int frames) const;
    static std::chrono::milliseconds audio_fade_duration(int frames);
    float text_line_height() const;
    std::vector<std::string> display_lines(std::string_view source) const;
    // display_lines() measures a growing prefix per character, and a frame
    // asks for the same message's lines from several places: remembered by
    // text, width and font generation, the last few of them.
    struct WrappedText {
        std::string source;
        float width = 0.0f;
        std::uint64_t generation = 0;
        std::vector<std::string> lines;
    };
    mutable std::vector<WrappedText> wrapped_text_;
    float message_text_x() const;
    float message_text_y() const;
    static std::size_t utf8_prefix_bytes(
        std::string_view text, std::size_t characters);
    static std::size_t utf8_character_count(std::string_view text);
    // For each byte of `visible` that starts a character, the index in
    // msg().counted() of the engine glyph it is drawn as.  The two strings
    // are different renderings of one source: the engine does not count the
    // script's line breaks, among other things, so numbering the visible
    // string's own characters ran a message's tail - one character per line
    // break - past every glyph the engine reveals, and the outline text
    // stopped three or four characters short with the indicator already up.
    std::vector<std::size_t> engine_glyph_map(std::string_view visible) const;
    int choice_reveal_count(int index) const;
    bool choice_reveal_finished() const;
    // The single point at which a choice is committed.  AVG_ControlSelectWindow
    // has exactly one - `if( click && select>=0 )` - and it blanks the message
    // there; ours had four, none of which did.
    void answer_choice(int select);
    void control_select_window();
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
    // The art target, reduced to thumbnail size into thumbnail_target_
    // (made once), or null.
    SDL_Texture* draw_frame_thumbnail(int width, int height);
    // save_snapshot_, without the pipeline sync where GLES allows: the
    // reduced frame is queued for an asynchronous read, and a save made
    // before it arrives writes its thumbnail when it does
    // (finish_save_snapshot, every frame).
    void capture_save_snapshot();
    bool has_save_snapshot() const;
    void reset_save_snapshot();
    void finish_save_snapshot(bool wait);
    Texture thumbnail_target_;
    std::unique_ptr<th2::GlAsyncReadback> thumbnail_readback_;
    // Whether the pending read becomes save_snapshot_ (false once reset),
    // and the slots whose thumbnail file waits for it.
    bool snapshot_keep_ = false;
    std::vector<int> snapshot_slots_;
    static constexpr int save_thumbnail_width = 160;
    static constexpr int save_thumbnail_height = 120;
    Texture texture_from_surface(SDL_Surface* surface);
    void retire_soak_gpu_work(bool force = false);
    std::vector<std::uint8_t> load_transition_mask(
        int type, int& width, int& height);
    std::vector<std::uint8_t> transition_mask_pixels(
        SDL_Surface* surface, int type, int& width, int& height);
    // Script-driven transitions run through AVG_EffCnt(), which the
    // effect-speed setting scales; the title, save and gallery screens use
    // AVG_EffCnt4(), which it does not.
    enum class EffectTiming { script, menu };
    void begin_transition(
        int type, int frames, int vague, bool resume_script,
        EffectTiming timing = EffectTiming::script);
    void update_transition();
    void draw_pattern_transition(float progress);
    // AVG_ControlBackChange's integer rate, 0..256.
    void draw_pattern_transition_rate(int rate);
    bool draw_pattern_transition_gpu(int rate);
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
    // `extra` ticks on from sc_cnt, for a frame drawn between ticks; 0 for
    // everything the engine itself asks.
    BackgroundView current_background_view(double extra = 0.0) const;
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
    // AVG_ControlChar counts in frames and the engine runs at sixty of them
    // a second; we run at the display's rate, which is often twice that.
    // This is the leftover time between sixtieths, so the state machine is
    // stepped the number of times the original would have stepped it.
    // Set when a character has been composited into BMP_BACK, so the
    // darkened copy taken from it can be refreshed once at the end of the
    // pass rather than per character.
    bool half_tone_copy_stale_ = false;
    double character_control_debt_ = 0.0;
    std::chrono::steady_clock::time_point character_control_time_{};
    void update_character_animations(int steps);
    void play_se(int channel, int sound, bool loop, int volume, int fade = 0,
                 bool wait_for_completion = false);
    void sync_game_flags();
    // void AVG_PlayBGM( int mus_no, int fade, int loop, int vol, int change )
    // and int AVG_StopBGM( int fade ).  Every way the music starts or stops
    // goes through these two in the engine, which is also where the
    // reference's trace probe sits - so a caller that reaches the mixer some
    // other way is invisible to the comparison even when it behaves
    // correctly.  AVG_SetMovie was exactly that: it stops the music, ours
    // stopped it too, and the audio trace still showed the reference alone.
    void play_bgm(int music, bool loop, int volume, int fade = 0,
                  bool change = false);
    void stop_all_se(int fade);
    void stop_bgm(int fade);
    bool movie_bgm_stop_pending_ = false;
    void play_voice(const th2::Event& event);
    void draw_engine_text(const th2::EngineText& text);
    // The engine's system menu, for the engine bar: game_engine_config.cpp.
    struct EngineConfigState {
        int flag = 0;
        int mode = 0;
        int cnt = 0;
        int next_cnt = 0;
        int next_mode = 0;
    };
    EngineConfigState engine_config_{};
    int engine_config_select_back_ = 0;   // AVG_ControlConfigWindow's static
    int engine_config_open_mode_ = 0;     // ConfigOpenMode
    int engine_config_mouse_ = 0;         // Config.mouse
    // AvgStep[0] == AVG_CONFIG, and AVG_ChangeSetp's pending change, which
    // AVG_RenewSetp applies at the end of the frame.
    bool engine_config_step_ = false;
    std::optional<bool> engine_config_step_next_;
    // AVG_CloseBack has put the half-tone plate away; AVG_OpenBack brings it
    // back.  setup_background_graphs re-asserts the plate every frame and
    // has to know.
    bool engine_back_closed_ = false;
    // GRP_WEATHER's display: AVG_CloseBack hides every petal, and the next
    // AVG_ControlWeather puts them back.
    bool weather_disp_ = true;
    bool engine_config_check() const;
    void engine_go_config(int mode);
    // The port's screen standing in for the engine's save (1), load (2) or
    // side-bar settings (3), and the way back from it.
    void engine_open_port_screen(int page);
    void engine_port_screen_closed();
    void engine_config_loaded();
    void engine_close_back();
    void engine_open_back();
    void engine_set_config_window();
    void engine_reset_config_window();
    void engine_end_config();
    void engine_close_start_config_window(int cmax);
    void control_engine_config();
    void play_log_voice(int character, int scenario, int voice, bool a_cut);
    void update_audio();
    void refresh_audio_wait(bool before_script);
    void set_background(const th2::Event& event, bool keep_characters);
    void set_cg(
        const th2::Event& event, BackgroundKind kind, char prefix);
    void restore_background();
    std::optional<std::size_t> overlay_index(int requested) const;
    void reset_overlays();
    void restore_overlay(std::size_t slot, const OverlayState& held);
    void load_overlay(
        std::size_t slot, std::string name, std::string archive,
        int tone_type, int layer, int nuki);
    bool handle(const th2::Event& event);
    std::filesystem::path dump_engine_error(
        const th2::ScriptStep& step, std::string_view error);
    std::filesystem::path dump_runtime_error(std::string_view error);
    void advance(bool skipping = false);
    // The virtual machine itself, run once a frame from EXEC_ControlLang's
    // slot.  advance() is the player's advance and calls into it; this is
    // what keeps a parked instruction asking.
    void exec_control_lang(bool skipping = false);
    // MAIN_SetScriptFlag / MAIN_GetScriptFlag: the one gate on the machine,
    // turned off by the modes that take the screen over.
    bool script_flag_ = true;
    // EXEC_ControlLang's slot: retires a parked ESC_WAIT and runs whatever
    // resume_script() asked for, before any of the update_ pass.
    void pump_script();
    void save(int slot);
    bool load(int slot);
    std::filesystem::path save_path(int slot) const;
    std::filesystem::path thumbnail_path(int slot) const;
    std::filesystem::path metadata_path(int slot) const;
    std::filesystem::path anime4k_shader_dir() const;
    std::filesystem::path blend_shader_dir() const;
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
    void reset_play_state();
    void initialize_scenario_flags();
    void start_new_game();
    void open_name_input();
    void begin_title_exit(bool start_game);
    void begin_title_menu_transition(bool extras);
    void update_title();
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
    // GRP_KEYWAIT's state, set by AVG_ControlNovelMessage and read by
    // draw_click_indicator().
    bool keywait_visible_ = false;
    bool keywait_page_end_ = false;
    // AVG_PlaySE3( no, volume ), for the two sounds the message machine makes.
    void play_system_se(int number, int volume);
    // AVG_MsgCnt(): how far NovelMessage.count moves in one frame.
    int message_count_step() const;
    // AVG_EffCntPuls(): how far HalfTone.tcount moves in one frame.
    int effect_count_pulse() const;
    // Avg.msg_wait, the text-speed setting, as the engine numbers it: 0 is
    // instant, 3 the slowest.  Ours is a millisecond-per-character slider,
    // so it is folded back into the four the original has.
    int message_wait_setting() const;
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
    std::vector<AudioDecodeRequest> audio_decode_queue_;
    // Twenty live and ten stale, the same policy the pictures and the byte
    // store use.  Counted in objects rather than bytes because read-ahead
    // decodes a second of a clip, not the whole of it.
    th2::PredecodeCache<std::shared_ptr<th2::AudioDecoder>> audio_decoders_;
    std::uint64_t audio_decode_order_ = 0;

    // A track started from its first 64KB, waiting for the rest of its
    // bytes.  Held weakly: if the decoder is dropped before the tail lands -
    // evicted, or the scene moved on - there is nothing left to finish.
    struct PendingAudioRest {
        const th2::Archive* archive = nullptr;
        const th2::ArchiveEntry* entry = nullptr;
        std::weak_ptr<th2::AudioDecoder> decoder;
        std::size_t ready = 0;
    };
    std::vector<PendingAudioRest> pending_audio_rest_;
    // Key to the generation of the last scan that named it.  The plan itself
    // is rebuilt every scan and an asset in use is usually behind the
    // interpreter by then, so asking the plan whether something was
    // predicted always answers no.  This remembers.
    std::unordered_map<std::string, int> named_recently_;
    // Scans run up to twenty times a second, so this is about ten seconds
    // of exploration - long enough that an asset named a moment before it is
    // used still counts as predicted, which is the question being asked.
    static constexpr int named_recently_generations = 200;
    // True when no recent scan saw this coming; counts and logs it once.
    bool note_asset_use(const std::string& key);
    void collect_audio_rests();
    // Two and a half seconds of Vorbis, and one round trip.  Larger buys
    // nothing - the playback window only needs one second in front of the
    // device - and smaller risks a track whose first page is unusually big.
    static constexpr std::size_t audio_head_bytes = 64 * 1024;

    // `keep` says whether this is read-ahead, and so whether the decoder
    // belongs in the explorer-governed cache at all.
    std::shared_ptr<th2::AudioDecoder> audio_decoder(
        const th2::Archive& archive, std::string_view name, int rank,
        bool keep);
    std::shared_ptr<th2::AudioDecoder> ready_audio_decoder(
        const th2::Archive& archive, std::string_view name);
    void update_audio_decode();

    int prefetch_event_assets(
        const th2::Event& event, std::string_view script_name);
    // Prefetches what a script loads when it starts, for the scripts the
    // current one can reach.  Returns how many transfers it started.

    void handle_touch_actions();
    // Turns one finger's events into the mouse press, motion and release the
    // game's own UI is written against.
    void push_touch_mouse_event(
        const SDL_Event& event, int window_width, int window_height);
    // Starts a drag if the touch landed on something draggable.
    bool begin_touch_drag(float logical_x, float logical_y);
    // Drops the hover highlights a lifted finger left behind.
    void clear_pointer_highlights();
    void open_save_load(UiMode mode);
    void close_save_load();
    void draw_save_digit_sheet_text(
        float x, float y, std::string_view text,
        std::uint8_t red = 255, std::uint8_t green = 255,
        std::uint8_t blue = 255);
    void draw_save_digit_number(float x, float y, int number, int digits);
    int map_marker_level(int field) const;
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
    void change_map_field(int direction);
    void update_map_hover(float x, float y);
    bool map_hover_on_page() const;
    void update_map();
    void draw_sidebar();
    // Handles a press on the sidebar.  With activate_buttons false the
    // buttons are left alone and only the draggable parts respond, which is
    // what a touch press does: the buttons wait for the release.
    void draw_click_indicator();
    // GRP_KEYWAIT's sprite frame; see the definition.
    bool draw_keywait_sprite(
        SDL_Texture* texture, const SDL_FRect& source,
        const SDL_FRect& destination);
    int keywait_frame() const;
    std::size_t message_visible_lines() const;
    int message_scroll_limit(std::size_t total_lines) const;
    // Scroll text that overflows its box by `direction` lines; false when
    // nothing here overflows.
    bool scroll_overflowing_text(int direction);
    // The engine's log entry in the outline font (see draw_log_modern).
    struct LogDisplay {
        std::string text;
        std::vector<std::size_t> raw_of;
    };
    LogDisplay log_display(const std::string& raw) const;
    std::pair<std::size_t, std::size_t> log_voiced_span(
        const std::string& raw, std::size_t start) const;
    std::vector<SDL_FRect> log_span_rects(
        const std::string& raw, std::size_t start, std::size_t end) const;
    void draw_log_modern();
    bool log_entry_shown() const;
    int log_scroll_ = 0;
    std::string log_scroll_text_;
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
    // AVG_ResetHalfTone(): the wash goes away outright.  Setting a character
    // does this, which is what keeps BMP_BACKHALF from ever being a copy of
    // a plate that has since changed.
    void reset_half_tone();
    void update_half_tone();
    bool handle_message_scroll_press(float x, float y);
    void set_message_scroll_from_y(float y);
    void select_overlay();
    void begin_overlay();
    void select_sidebar();
    void clear_sidebar();
    void clear_authentic_text();
    void begin_authentic_text();
    void apply_text_shake();
    float imgui_display_scale() const;
    void present_frame();
    void reset_render_state();
    // Hides the message window for a character animation, remembering to
    // put it back when the animation ends.
    // AVG_ControlBackFade() is DSP_SetGraphBright() on the background and on
    // every script bitmap - a property of each object, not a wash over the
    // screen.  Applied per texture it follows whatever transform that object
    // has and respects its alpha, so a faded scene cannot tint the black
    // behind a shaken background or the transparent parts of an overlay.
    //
    // Returns how much has to be added back on a second pass: colour
    // modulation can only darken, and the fade also brightens.
    // What the half tone multiplies the picture by while the message window
    // is up: BackStruct.r * Avg.half_tone / 128, ramped over the sixteen
    // steps of TONE_FADEOUT.  One, when there is no wash.
    //
    // It is a property of the picture, not a sheet laid over the scene.  The
    // original builds BMP_BACKHALF by copying BMP_BACK at this factor and
    // displays that instead - and since AVG_SetBackChar composites the
    // characters into BMP_BACK first, they are inside the copy and darken
    // with it.  Overlays are GRP_SCRIPT objects, never in that bitmap, so
    // they keep full brightness.
    float half_tone_factor() const;
    // True once TONE_FADEOUT has run its sixteen steps and the engine swaps
    // GRP_BACK off for GRP_BACK+1.  Before that it dims GRP_BACK in place
    // with DSP_SetGraphBright and the copy is not shown at all.
    bool half_tone_settled() const;
    // BMP_BACKHALF: the background copied at the half-tone factor and shown
    // *instead of* the background, not over it.  Being an opaque layer at
    // LAY_BACK+2 it also hides anything at layers 1 and 2, which a
    // modulation on the background alone cannot do.
    //
    // Built once, where AVG_SetHalfTone() builds it - the TONE_NODISP branch,
    // when the wash appears from nothing.  Called again while the ramp is
    // running it only switches the copy on; it does not copy again.  So the
    // copy is of the background as it was when the line arrived, and a
    // background that changes underneath it without a new message is not
    // reflected.  That is the engine's behaviour, not an oversight here.
    void build_half_tone_background();
    SDL_Texture* half_tone_background() const;
    // The full darkness the copy is made at: half_tone/128, with no ramp.
    // The ramp is done live on the background instead, which is why the two
    // are separate.
    float half_tone_target_factor() const;
    void ensure_shake_target();
    // The engine never clears its framebuffer, so whatever a transform fails
    // to cover keeps the pixels from the frame before.  That is visible in
    // the original - the corners a rotation leaves behind hold the previous
    // picture - so it is reproduced rather than papered over with black.
    //
    // Done as an explicit copy rather than by simply not clearing: a render
    // target surviving between frames is not contractual, and an FBO
    // attachment can be invalidated on the web build.  Both blits stay on
    // the GPU; there is no readback.
    // The art target that has had its first clear; any other is new.
    SDL_Texture* art_cleared_for_ = nullptr;
    // Whether the authentic-text and sidebar layers were drawn into since
    // their last clear: an empty layer is neither cleared nor composited.
    bool text_layer_used_ = true;
    bool sidebar_layer_used_ = true;
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
