#include "data_source.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstring>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#else
#include <fstream>
#include <iterator>
#include <system_error>
#endif

namespace th2 {

#ifdef __EMSCRIPTEN__

namespace {

// The engine addresses game data by relative path ("game-data/SDT.PAK"), which
// doubles as a URL relative to the page.  Both helpers suspend the wasm stack
// on the await and resume when the response arrives, which requires JSPI.

EM_ASYNC_JS(double, fetch_size, (const char* url), {
    try {
        const response = await fetch(UTF8ToString(url), {method: "HEAD"});
        if (!response.ok) {
            return -1;
        }
        const length = response.headers.get("Content-Length");
        return length === null ? -1 : Number(length);
    } catch (error) {
        return -1;
    }
});

EM_ASYNC_JS(int, fetch_range,
    (const char* url, double offset, double size, void* destination), {
    try {
        const last = offset + size - 1;
        const response = await fetch(UTF8ToString(url), {
            headers: {Range: `bytes=${offset}-${last}`},
        });
        if (!response.ok) {
            return 0;
        }
        // A server that ignores Range answers 200 with the whole file, which
        // for these archives would mean buffering a gigabyte to satisfy a
        // small read.  Refuse before touching the body.
        if (response.status !== 206) {
            const length = Number(response.headers.get("Content-Length"));
            if (length && length !== size) {
                console.error("server ignored Range for " + UTF8ToString(url));
                return 0;
            }
        }
        const body = new Uint8Array(await response.arrayBuffer());
        if (body.length !== size) {
            return 0;
        }
        // HEAPU8 is re-read here rather than before the await, so a heap that
        // grew while the request was in flight still resolves to a live view.
        HEAPU8.set(body, destination);
        return 1;
    } catch (error) {
        return 0;
    }
});

// Fetches a range without waiting for it.  The bytes land in a JS-side map
// that take_prefetched() drains, so the engine can ask for what a script is
// about to need while the player is still reading the current line.  The
// budget is generous but bounded: prefetches are guesses, and a wrong guess
// must not be able to push the tab out of memory.
EM_JS(void, start_prefetch,
    (const char* url, double offset, double size, int keep, int rank), {
    const path = UTF8ToString(url);
    const key = path + ":" + offset + ":" + size;
    Module.th2Prefetch ||= {entries: new Map(), bytes: 0};
    const store = Module.th2Prefetch;
    if (store.entries.has(key)) {
        return;
    }
    const budget = 256 * 1024 * 1024;
    if (store.bytes + size > budget) {
        // Make room by dropping the most speculative entries first, and
        // within a rank the most recently added - those sit furthest ahead
        // of the player, because the scan walks the script in order.  What
        // the next few lines need was requested earliest at the lowest rank,
        // so it is the last thing to go.  Only entries that have arrived can
        // be dropped; a request still in flight has nothing to reclaim.
        const droppable = [...store.entries]
            .filter(([, entry]) => entry.body && !entry.keep)
            .sort((a, b) => (b[1].rank - a[1].rank) || (b[1].order - a[1].order));
        for (const [oldKey, entry] of droppable) {
            if (store.bytes + size <= budget) {
                break;
            }
            store.bytes -= entry.body.length;
            store.entries.delete(oldKey);
        }
        if (store.bytes + size > budget) {
            return;
        }
    }
    // A "kept" entry is a chunk covering many later reads - an archive's
    // directory, say - so it stays after it has been read from.
    store.counter = (store.counter || 0) + 1;
    const entry = {body: null, path: path, offset: offset, size: size,
                   keep: keep != 0, rank: rank, order: store.counter};
    const last = offset + size - 1;
    entry.promise = fetch(path, {headers: {Range: `bytes=${offset}-${last}`}})
        .then(async (response) => {
            if (!response.ok) {
                return null;
            }
            if (response.status !== 206) {
                const length = Number(response.headers.get("Content-Length"));
                if (length && length !== size) {
                    return null;
                }
            }
            const body = new Uint8Array(await response.arrayBuffer());
            if (body.length !== size) {
                return null;
            }
            entry.body = body;
            store.bytes += body.length;
            return body;
        })
        .catch(() => null);
    store.entries.set(key, entry);
});

// Hands over a prefetched range, waiting for it when it is still in flight -
// which is still cheaper than a second request.  Returns 1 when destination
// was filled, 0 when this range was never prefetched (the caller then fetches
// it the usual way) and -1 when the prefetch itself failed.
EM_ASYNC_JS(int, take_prefetched,
    (const char* url, double offset, double size, void* destination), {
    const store = Module.th2Prefetch;
    if (!store) {
        return 0;
    }
    const path = UTF8ToString(url);
    let entry = store.entries.get(path + ":" + offset + ":" + size);
    let start = 0;
    if (!entry) {
        // No exact match: a kept chunk that spans this range serves it just
        // as well, which is how one request covers a whole archive
        // directory.
        for (const candidate of store.entries.values()) {
            if (candidate.keep && candidate.path === path
                && candidate.offset <= offset
                && candidate.offset + candidate.size >= offset + size) {
                entry = candidate;
                start = offset - candidate.offset;
                break;
            }
        }
    }
    if (!entry) {
        return 0;
    }
    const body = entry.body ?? await entry.promise;
    if (!entry.keep) {
        if (entry.body) {
            store.bytes -= entry.body.length;
        }
        store.entries.delete(path + ":" + offset + ":" + size);
    }
    if (!body) {
        return -1;
    }
    // HEAPU8 is re-read after the await so a heap that grew meanwhile still
    // resolves to a live view.
    HEAPU8.set(body.subarray(start, start + size), destination);
    return 1;
});

EM_JS(int, prefetch_pending, (const char* url, double offset, double size), {
    const store = Module.th2Prefetch;
    if (!store) {
        return 0;
    }
    const path = UTF8ToString(url);
    if (store.entries.has(path + ":" + offset + ":" + size)) {
        return 1;
    }
    for (const candidate of store.entries.values()) {
        if (candidate.keep && candidate.path === path
            && candidate.offset <= offset
            && candidate.offset + candidate.size >= offset + size) {
            return 1;
        }
    }
    return 0;
});

// Every read that misses costs a network round trip, which on anything
// slower than a LAN is long enough to drop a frame.  Three caches keep that
// off the hot path.

// Sizes never change, so one HEAD per file is enough for the whole session.
std::unordered_map<std::string, std::uint64_t>& size_cache()
{
    static std::unordered_map<std::string, std::uint64_t> cache;
    return cache;
}

// Files at or below this are downloaded whole on first touch and served from
// memory afterwards.  It covers SDT.PAK (scripts), FNT.PAK (fonts) and
// TOHEART2.EXE, which are small, read constantly, and worth the one-off cost.
constexpr std::uint64_t pinned_file_limit = 16ull << 20;

// Larger archives keep an LRU of the ranges already read, so a background or
// CG that comes back on screen does not go to the network again.  Single
// reads above the entry limit (movies) bypass it rather than evict it.
constexpr std::size_t range_cache_budget = 64ull << 20;
constexpr std::size_t range_cache_entry_limit = 4ull << 20;

std::unordered_map<std::string, std::vector<std::uint8_t>>& pinned_files()
{
    static std::unordered_map<std::string, std::vector<std::uint8_t>> files;
    return files;
}

struct RangeEntry {
    std::string key;
    std::vector<std::uint8_t> bytes;
};

std::list<RangeEntry>& range_cache()
{
    static std::list<RangeEntry> entries;
    return entries;
}

std::unordered_map<std::string, std::list<RangeEntry>::iterator>&
range_index()
{
    static std::unordered_map<std::string, std::list<RangeEntry>::iterator>
        index;
    return index;
}

// Ranges already asked for.  Checking this in C++ keeps data_is_resident()
// off the JS side, which the lookahead calls dozens of times per scan.
std::unordered_set<std::string>& requested_ranges()
{
    static std::unordered_set<std::string> keys;
    return keys;
}

std::size_t& range_cache_bytes()
{
    static std::size_t bytes = 0;
    return bytes;
}

std::string range_key(
    const std::string& path, std::uint64_t offset, std::size_t size)
{
    return path + ':' + std::to_string(offset) + ':' + std::to_string(size);
}

void remember_range(
    const std::string& key, std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > range_cache_entry_limit) {
        return;
    }
    auto& entries = range_cache();
    auto& index = range_index();
    entries.push_front(RangeEntry{key, {bytes.begin(), bytes.end()}});
    index[key] = entries.begin();
    range_cache_bytes() += bytes.size();
    while (range_cache_bytes() > range_cache_budget && !entries.empty()) {
        const auto& oldest = entries.back();
        range_cache_bytes() -= oldest.bytes.size();
        index.erase(oldest.key);
        entries.pop_back();
    }
}

}  // namespace

bool data_exists(const std::filesystem::path& path)
{
    return data_size(path) > 0;
}

std::uint64_t data_size(const std::filesystem::path& path)
{
    const std::string key = path.string();
    if (const auto found = size_cache().find(key);
        found != size_cache().end()) {
        return found->second;
    }
    const double size = fetch_size(key.c_str());
    const auto result = size < 0 ? 0 : static_cast<std::uint64_t>(size);
    if (result > 0) {
        size_cache().emplace(key, result);
    }
    return result;
}

bool data_read(
    const std::filesystem::path& path, std::uint64_t offset,
    std::span<std::uint8_t> destination)
{
    if (destination.empty()) {
        return true;
    }
    const std::string key = path.string();

    if (const auto pinned = pinned_files().find(key);
        pinned != pinned_files().end()) {
        const auto& contents = pinned->second;
        if (offset + destination.size() > contents.size()) {
            return false;
        }
        std::memcpy(
            destination.data(), contents.data() + offset, destination.size());
        return true;
    }

    const std::uint64_t total = data_size(path);
    if (total > 0 && total <= pinned_file_limit) {
        std::vector<std::uint8_t> contents(static_cast<std::size_t>(total));
        if (fetch_range(
                key.c_str(), 0.0, static_cast<double>(total),
                contents.data()) == 0) {
            return false;
        }
        const auto& stored =
            pinned_files().emplace(key, std::move(contents)).first->second;
        if (offset + destination.size() > stored.size()) {
            return false;
        }
        std::memcpy(
            destination.data(), stored.data() + offset, destination.size());
        return true;
    }

    const std::string cache_key = range_key(key, offset, destination.size());
    auto& index = range_index();
    if (const auto found = index.find(cache_key); found != index.end()) {
        auto& entries = range_cache();
        entries.splice(entries.begin(), entries, found->second);
        std::memcpy(
            destination.data(), found->second->bytes.data(),
            destination.size());
        return true;
    }

    // Something asked for this range ahead of time; take it rather than
    // opening a second request for the same bytes.
    const int prefetched = take_prefetched(
        key.c_str(), static_cast<double>(offset),
        static_cast<double>(destination.size()), destination.data());
    if (prefetched == 1) {
        remember_range(cache_key, destination);
        return true;
    }

    if (fetch_range(
            key.c_str(), static_cast<double>(offset),
            static_cast<double>(destination.size()), destination.data())
        == 0) {
        return false;
    }
    remember_range(cache_key, destination);
    return true;
}

void data_prefetch(
    const std::filesystem::path& path, std::uint64_t offset, std::size_t size,
    PrefetchRank rank)
{
    if (size == 0 || size > range_cache_entry_limit) {
        return;  // Movie-sized reads are streamed, not cached.
    }
    const std::string key = path.string();
    if (pinned_files().contains(key)) {
        return;
    }
    const auto cache_key = range_key(key, offset, size);
    if (range_index().contains(cache_key)
        || !requested_ranges().insert(cache_key).second) {
        return;
    }
    start_prefetch(
        key.c_str(), static_cast<double>(offset), static_cast<double>(size),
        0, static_cast<int>(rank));
}

void data_prefetch_chunk(
    const std::filesystem::path& path, std::uint64_t offset, std::size_t size)
{
    const std::string key = path.string();
    if (pinned_files().contains(key)) {
        return;
    }
    start_prefetch(
        key.c_str(), static_cast<double>(offset), static_cast<double>(size),
        1, static_cast<int>(PrefetchRank::imminent));
}

bool data_is_resident(
    const std::filesystem::path& path, std::uint64_t offset,
    std::size_t size)
{
    const std::string key = path.string();
    if (pinned_files().contains(key)) {
        return true;
    }
    const auto cache_key = range_key(key, offset, size);
    if (range_index().contains(cache_key)
        || requested_ranges().contains(cache_key)) {
        return true;
    }
    // Not asked for directly, but a chunk fetched for something else may
    // still cover it; that check has to go to the store itself.
    return prefetch_pending(
        key.c_str(), static_cast<double>(offset),
        static_cast<double>(size)) != 0;
}

#else

bool data_exists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

void data_prefetch(
    const std::filesystem::path&, std::uint64_t, std::size_t, PrefetchRank)
{
    // Reads come off the disk here; the OS cache is the whole story.
}

void data_prefetch_chunk(
    const std::filesystem::path&, std::uint64_t, std::size_t)
{
}

bool data_is_resident(
    const std::filesystem::path&, std::uint64_t, std::size_t)
{
    return true;
}

std::uint64_t data_size(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

bool data_read(
    const std::filesystem::path& path, std::uint64_t offset,
    std::span<std::uint8_t> destination)
{
    if (destination.empty()) {
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(
        reinterpret_cast<char*>(destination.data()),
        static_cast<std::streamsize>(destination.size()));
    return static_cast<bool>(input);
}

#endif

std::vector<std::uint8_t> read_data_file(const std::filesystem::path& path)
{
    const auto size = data_size(path);
    if (size == 0) {
        return {};
    }
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
    if (!data_read(path, 0, contents)) {
        return {};
    }
    return contents;
}

}  // namespace th2
