#include "game.hpp"

#include "data_source.hpp"
#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <functional>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace th2app {


// Directory for writable files (config, saves, logs). On Android the current
// working directory is not writable, so use the app-internal storage path.
// On desktop platforms use the system-preferred user data directory so the
// game works regardless of where the binary is launched from.
std::filesystem::path writable_directory()
{
#ifdef __ANDROID__
    return std::filesystem::path(SDL_GetAndroidInternalStoragePath());
#else
    char* path = SDL_GetPrefPath("ripdog", "ToHeart2XR");
    if (!path) {
        return std::filesystem::path(".");
    }
    std::filesystem::path result(path);
    SDL_free(path);
    return result;
#endif
}

std::filesystem::path profile_directory()
{
    return writable_directory() / "profile";
}

std::filesystem::path app_config_directory()
{
#ifdef __ANDROID__
    return std::filesystem::path(SDL_GetAndroidInternalStoragePath());
#else
    char* path = SDL_GetPrefPath("ripdog", "ToHeart2XR");
    if (!path) {
        return std::filesystem::path(".");
    }
    std::filesystem::path result(path);
    SDL_free(path);
    return result;
#endif
}

std::filesystem::path remembered_data_path_file()
{
    return app_config_directory() / "game-data-path.txt";
}

bool valid_game_data_directory(const std::filesystem::path& path)
{
#ifndef __EMSCRIPTEN__
    // The browser build streams the archives from the server, so there is no
    // local directory to inspect; th2::data_exists probes them over HTTP.
    if (!std::filesystem::is_directory(path)) {
        return false;
    }
#endif
    return th2::data_exists(path / "TOHEART2.EXE")
        && th2::data_exists(path / "SDT.PAK")
        && th2::data_exists(path / "GRP.PAK");
}

std::optional<std::filesystem::path> load_remembered_data_path()
{
    std::ifstream input(remembered_data_path_file());
    std::string line;
    if (!std::getline(input, line) || line.empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(line);
}

void save_remembered_data_path(const std::filesystem::path& path)
{
    std::filesystem::create_directories(app_config_directory());
    std::ofstream output(remembered_data_path_file());
    output << path.string() << '\n';
}

std::optional<std::filesystem::path> pick_game_executable()
{
    struct DialogState {
        std::mutex mutex;
        bool done = false;
        std::optional<std::filesystem::path> path;
    } state;

    SDL_Window* window = SDL_CreateWindow(
        "Select TOHEART2.EXE", 640, 120, SDL_WINDOW_HIDDEN);
    WindowPtr window_holder(window);
    const SDL_DialogFileFilter filters[] = {
        {"ToHeart2 executable", "exe"},
        {"All files", "*"},
    };
    auto callback = [](void* userdata, const char* const* filelist, int) {
        auto* dialog = static_cast<DialogState*>(userdata);
        std::lock_guard lock(dialog->mutex);
        if (filelist && filelist[0]) {
            dialog->path = std::filesystem::path(filelist[0]);
        }
        dialog->done = true;
    };
    SDL_ShowOpenFileDialog(
        callback, &state, window, filters, std::size(filters), nullptr, false);

    for (;;) {
        {
            std::lock_guard lock(state.mutex);
            if (state.done) {
                return state.path;
            }
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                return std::nullopt;
            }
        }
        SDL_Delay(16);
    }
}

std::optional<std::filesystem::path> discover_game_data_path(
    const std::filesystem::path& default_path, bool explicit_path)
{
    if (valid_game_data_directory(default_path)) {
        if (explicit_path) {
            save_remembered_data_path(default_path);
        }
        return default_path;
    }
    if (!explicit_path) {
        if (const auto remembered = load_remembered_data_path();
            remembered && valid_game_data_directory(*remembered)) {
            return *remembered;
        }
#ifdef __EMSCRIPTEN__
        // The browser build serves its data from the preloaded /game-data
        // package; there is no native file picker to fall back on.
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "No game data at %s. Rebuild with -DTH2_WEB_GAME_DATA=<dir>.",
            default_path.string().c_str());
        return std::nullopt;
#else
        const auto executable = pick_game_executable();
        if (!executable) {
            return std::nullopt;
        }
        const auto directory = executable->parent_path();
        if (valid_game_data_directory(directory)) {
            save_remembered_data_path(directory);
            return directory;
        }
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "Selected executable is not in a valid game data directory: %s",
            executable->string().c_str());
#endif
    }
    return std::nullopt;
}

// Convert window-coordinate mouse events to the fixed 800x600 logical
// coordinate system used by the core game, applying 4:3 letterboxing.
std::pair<float, float> logical_coordinates(
    float x, float y, int window_width, int window_height)
{
    const float scale = std::min(
        window_width / 800.0f, window_height / 600.0f);
    const float logical_width = 800.0f * scale;
    const float logical_height = 600.0f * scale;
    const float offset_x = (window_width - logical_width) / 2.0f;
    const float offset_y = (window_height - logical_height) / 2.0f;
    return {(x - offset_x) / scale, (y - offset_y) / scale};
}

void convert_event_to_logical_coordinates(
    SDL_Event& event, int window_width, int window_height)
{
    const float scale = std::min(
        window_width / 800.0f, window_height / 600.0f);

    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const auto [x, y] = logical_coordinates(
            event.motion.x, event.motion.y, window_width, window_height);
        event.motion.x = x;
        event.motion.y = y;
        event.motion.xrel /= scale;
        event.motion.yrel /= scale;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const auto [x, y] = logical_coordinates(
            event.button.x, event.button.y, window_width, window_height);
        event.button.x = x;
        event.button.y = y;
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        const auto [x, y] = logical_coordinates(
            event.wheel.mouse_x, event.wheel.mouse_y, window_width, window_height);
        event.wheel.mouse_x = x;
        event.wheel.mouse_y = y;
        break;
    }
    default:
        break;
    }
}

std::int32_t number(const th2::Event& event, std::size_t index)
{
    return std::get<std::int32_t>(event.arguments.at(index));
}

const std::string& text(const th2::Event& event, std::size_t index)
{
    return std::get<std::string>(event.arguments.at(index));
}

Texture load_texture(SDL_Renderer* renderer, const th2::Archive& archive,
                     std::string_view name)
{
    const auto* entry = archive.find(name);
    if (!entry) {
        throw std::runtime_error("image not found: " + std::string(name));
    }
    SDL_Surface* surface = th2::load_image(archive.read(*entry), entry->name);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (!texture) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return Texture(texture);
}

Surface decode_image(
    const th2::Archive& image_archive, std::string_view image_name)
{
    const auto* image = image_archive.find(image_name);
    if (!image) {
        throw std::runtime_error(
            "image not found: " + std::string(image_name));
    }
    return Surface(th2::load_image(image_archive.read(*image), image->name));
}

Texture load_toned_texture(
    SDL_Renderer* renderer,
    const th2::Archive& image_archive,
    std::string_view image_name,
    const th2::Archive& curve_archive,
    const std::vector<ToneCurveSpec>& curves,
    Surface* pixels,
    Surface predecoded)
{
    // Decoding is the expensive half - LZS, then TGA, then a conversion per
    // pixel - and it is what lands on the frame where the scene changes.  A
    // caller that decoded it earlier hands the surface in; the tone curves
    // below rewrite it, so what arrives must already be a private copy.
    if (!predecoded && th2app::trace_prefetch) {
        // Not decoded ahead.  Whether that also means a wait depends on
        // whether the bytes arrived, which is the distinction worth logging:
        // a decode we did not do in advance is cheap, a read we did not
        // fetch in advance stalls the frame.
        const auto* entry = image_archive.find(image_name);
        SDL_Log("image not predecoded: %.*s%s",
                static_cast<int>(image_name.size()), image_name.data(),
                (entry && !image_archive.resident(*entry))
                    ? "  AND NOT FETCHED" : "");
    }
    Surface surface = predecoded
        ? std::move(predecoded)
        : decode_image(image_archive, image_name);
    for (const auto& curve : curves) {
        if (curve.name.empty()) {
            th2::apply_tone_curve(surface.get(), {}, curve.vividness);
            continue;
        }
        const auto* entry = curve_archive.find(curve.name);
        if (!entry) {
            throw std::runtime_error(
                "tone curve not found: " + curve.name);
        }
        th2::apply_tone_curve(
            surface.get(), curve_archive.read(*entry), curve.vividness);
    }
    SDL_Surface* texture_surface = surface.get();
    if (pixels) {
        pixels->reset(
            SDL_ConvertSurface(surface.get(), SDL_PIXELFORMAT_RGBA32));
        if (!*pixels) {
            throw std::runtime_error(SDL_GetError());
        }
        texture_surface = pixels->get();
    }
    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(renderer, texture_surface);
    if (!texture) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return Texture(texture);
}

th2::AudioClip load_audio(const th2::Archive& archive, std::string_view name)
{
    const auto* entry = archive.find(name);
    if (!entry) {
        throw std::runtime_error("audio not found: " + std::string(name));
    }
    return th2::decode_audio(archive.read(*entry));
}

int scenario_number(std::string_view name)
{
    int result = 0;
    for (const auto byte : name) {
        if (byte >= '0' && byte <= '9') {
            result = result * 10 + byte - '0';
        } else if (result != 0) {
            break;
        }
    }
    return result;
}

std::vector<std::string> display_lines(
    std::string_view source, float max_width,
    const std::function<float(std::string_view)>& measure)
{
    // Appends one whole UTF-8 character, so a half-written multi-byte
    // sequence is never measured.
    const auto append_character = [&](std::string& line, std::size_t& at) {
        line.push_back(source[at++]);
        while (at < source.size()
               && (static_cast<unsigned char>(source[at]) & 0xc0) == 0x80) {
            line.push_back(source[at++]);
        }
    };

    std::vector<std::string> lines;
    std::string line;
    bool just_wrapped = false;
    for (std::size_t position = 0; position < source.size();) {
        if (source[position] == '\n') {
            if (!line.empty() || !just_wrapped) {
                lines.push_back(line);
            }
            line.clear();
            just_wrapped = false;
            ++position;
            continue;
        }
        const auto previous_length = line.size();
        append_character(line, position);
        just_wrapped = false;
        // A trailing separator is allowed to overhang: the original renderer
        // does not wrap until the next printable glyph is over width.
        if (line.back() == ' ' || measure(line) <= max_width) {
            continue;
        }
        if (const auto space = line.find_last_of(' ');
            space != std::string::npos && space > 0) {
            lines.push_back(line.substr(0, space));
            line.erase(0, space + 1);
        } else {
            // One word wider than the whole line; break it mid-word.
            std::string overflow = line.substr(previous_length);
            line.resize(previous_length);
            lines.push_back(line);
            line = std::move(overflow);
        }
        just_wrapped = line.empty();
    }
    if (!line.empty() || lines.empty()) {
        lines.push_back(line);
    }
    return lines;
}

std::string interpret_newlines(std::string text)
{
    for (std::size_t position = 0;
         (position = text.find("\\n", position)) != std::string::npos;) {
        text.replace(position, 2, "\n");
        ++position;
    }
    return text;
}

bool clip_texture_source(
    SDL_Texture* texture, SDL_FRect& source, SDL_FRect& destination)
{
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    if (!SDL_GetTextureSize(texture, &texture_width, &texture_height)
        || source.w <= 0.0f || source.h <= 0.0f) {
        return false;
    }
    const SDL_FRect original = source;
    const float left = std::clamp(source.x, 0.0f, texture_width);
    const float top = std::clamp(source.y, 0.0f, texture_height);
    const float right = std::clamp(source.x + source.w, 0.0f, texture_width);
    const float bottom = std::clamp(source.y + source.h, 0.0f, texture_height);
    if (right <= left || bottom <= top) {
        return false;
    }
    destination.x += (left - original.x) / original.w * destination.w;
    destination.y += (top - original.y) / original.h * destination.h;
    destination.w *= (right - left) / original.w;
    destination.h *= (bottom - top) / original.h;
    source = {left, top, right - left, bottom - top};
    return true;
}


}  // namespace th2app
