#include "data_source.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstring>
#include <list>
#include <string>
#include <unordered_map>
#include <limits>
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
        // This is a read the engine is blocked on, so it never waits behind
        // a speculative one: it bypasses the prefetch queue entirely and
        // asks the browser to schedule it ahead of the low-priority
        // prefetches (ignored by engines without Fetch Priority).
        const response = await fetch(UTF8ToString(url), {
            headers: {Range: `bytes=${offset}-${last}`},
            priority: "high",
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

// Set up once, on first use.  Kept out of the admission pass so that pass
// reads as the algorithm it is rather than as initialisation with an
// algorithm attached.
EM_JS(void, th2InitPrefetchStore, (), {
    if (Module.th2Prefetch) {
        return;
    }
        const store = {entries: new Map(), bytes: 0, queue: [], active: 0,
                       counter: 0};
        // Prefetches are guesses, and a wrong guess must not be able to push
        // the tab out of memory.
        store.budget = 256 * 1024 * 1024;
        // Speculative requests in flight at once.  A browser keeps six
        // connections per origin on HTTP/1.1 and queues the rest itself, in
        // an order nothing can change afterwards; holding the guesses here
        // instead leaves slots free for the read the player is actually
        // waiting on, and keeps the ordering ours to rearrange.
        store.limit = 4;
        // One place that forgets a range: drop it from the map and give back
        // whatever space it was holding or had reserved.  Getting this wrong
        // in one path out of four is how a budget quietly becomes unusable,
        // so there is only one path.
        store.release = (entry) => {
            const key = entry.path + ":" + entry.offset + ":" + entry.size;
            if (!store.entries.delete(key)) {
                return;  // Already gone; do not refund twice.
            }
            store.bytes -= entry.size;
            // Also out of the queue.  Every caller happens to splice before
            // calling this, but one that forgot would leave the queue holding
            // a range the store no longer knows about, and pump() would fetch
            // it into nowhere.
            const queued = store.queue.indexOf(entry);
            if (queued >= 0) {
                store.queue.splice(queued, 1);
            }
            if (entry.abort && !entry.body) {
                // Still on the wire and nobody wants it any more.
                try {
                    entry.abort.abort();
                } catch (error) {
                    // An abort that cannot be delivered is not worth failing
                    // the eviction for.
                }
            }
        };
        store.begin = (entry) => {
            entry.started = true;
            store.active++;
            entry.abort = typeof AbortController === "function"
                ? new AbortController() : null;
            const last = entry.offset + entry.size - 1;
            fetch(entry.path, {
                headers: {Range: `bytes=${entry.offset}-${last}`},
                priority: "low",
                signal: entry.abort ? entry.abort.signal : undefined,
            })
                .then(async (response) => {
                    if (!response.ok) {
                        return null;
                    }
                    if (response.status !== 206) {
                        const length =
                            Number(response.headers.get("Content-Length"));
                        if (length && length !== entry.size) {
                            return null;
                        }
                    }
                    const body = new Uint8Array(await response.arrayBuffer());
                    if (body.length !== entry.size) {
                        return null;
                    }
                    entry.body = body;
                    return body;
                })
                .catch(() => null)
                .then((body) => {
                    store.active--;
                    if (!body) {
                        // Failed, or aborted because the range left the
                        // graph.  Either way the space set aside for it goes
                        // back, or the budget would shrink with every miss.
                        store.release(entry);
                    }
                    entry.settle(body);
                    store.pump();
                });
        };
        // Queued entries go out in need order rather than call order:
        // nearest in the script first, and among equals the one asked for
        // earliest.  A later scan's imminent range therefore overtakes an
        // earlier scan's guesses rather than queueing behind them.
        // Issuing a fetch costs about a tenth of a millisecond on the
        // thread that draws.  The concurrency limit alone does not bound
        // that per frame: over a fast link a request can complete in the
        // same frame it was issued, and each completion pumps the queue
        // again, so a burst drains in one go.  Capping the speculative
        // starts per frame keeps that off the frame budget; promotions
        // bypass this the way they bypass the concurrency limit, because
        // the engine is already blocked on them.
        store.perFrameLimit = 6;
        store.issuedThisFrame = 0;
        store.pump = () => {
            // Sorting only when a slot is free keeps this off the hot path:
            // a scan enqueues twenty ranges and can start at most four, so
            // the other sixteen calls do nothing but append.
            if (store.active >= store.limit || !store.queue.length
                || store.issuedThisFrame >= store.perFrameLimit) {
                return;
            }
            if (store.queue.length > 1) {
                store.queue.sort(
                    (a, b) => (store.priority(a) - store.priority(b))
                           || (a.order - b.order));
            }
            while (store.active < store.limit && store.queue.length
                   && store.issuedThisFrame < store.perFrameLimit) {
                store.issuedThisFrame++;
                store.begin(store.queue.shift());
            }
        };
        // A hidden tab stops painting, and stops prefetching with it; a read
        // the engine blocks on still goes out, through promote().
        const schedule = typeof requestAnimationFrame === "function"
            ? requestAnimationFrame
            : (fn) => setTimeout(fn, 16);
        const frameTick = () => {
            store.issuedThisFrame = 0;
            store.pump();
            schedule(frameTick);
        };
        schedule(frameTick);
        // A queue that outruns the player is stale guesses competing for
        // bandwidth with the ones that still matter, so it is bounded the
        // way the byte budget is: the most speculative go first.  Anything
        // dropped is simply a miss later, which data_read() refetches.
        store.queue_limit = 64;
        store.trim = () => {
            while (store.queue.length > store.queue_limit) {
                // The queue is sorted least-urgent-last only after pump()
                // runs, so pick the worst explicitly rather than trusting
                // position.
                let worst = 0;
                for (let i = 1; i < store.queue.length; ++i) {
                    const here = store.priority(store.queue[i]);
                    const best = store.priority(store.queue[worst]);
                    if (here > best
                        || (here === best
                            && store.queue[i].round
                                < store.queue[worst].round)) {
                        worst = i;
                    }
                }
                const dropped = store.queue.splice(worst, 1)[0];
                store.release(dropped);
                dropped.settle(null);
            }
        };
        store.round = 0;
        store.PASSED = 1000000;
        // Each scan is a generation.  A range carries the generation of the
        // scan that last named it, so "how long since the script could still
        // reach this" is just how far that number has fallen behind.
        //
        // No grace period: the scan names every reachable range every time,
        // so going unnamed for even one generation means the range has left
        // the graph.
        store.priority = (entry) => (
            entry.round === store.round ? entry.rank : store.PASSED);
        // A range stops being a guess the moment the engine blocks on it, so
        // it leaves the queue and goes out immediately, over both limits.
        store.promote = (entry) => {
            if (entry.started) {
                return;
            }
            const at = store.queue.indexOf(entry);
            if (at >= 0) {
                store.queue.splice(at, 1);
            }
            store.begin(entry);
        };
        Module.th2Prefetch = store;
});

// One admission pass over the scan's whole list.
//
// The list arrives sorted by (priority, size, offset) - deterministic, so the
// same script position always admits the same ranges in the same order - and
// each line is `path\toffset\tsize\tdepth`.  Ranges already held are repriced
// and skipped; the rest are admitted in order, evicting only entries the
// script wants later, until one will not fit even after that.  The pass stops
// there: everything after it in the list is at least as far off, so nothing
// after it would fit either.
EM_JS(void, submit_prefetch, (const char* text), {
    if (!Module.th2Prefetch) {
        th2InitPrefetchStore();
    }
    const store = Module.th2Prefetch;
    const budget = store.budget;
    store.round = (store.round || 0) + 1;
    const lines = UTF8ToString(text);
    const named = new Set();

    let at = 0;
    while (at < lines.length) {
        const stop = lines.indexOf("\n", at);
        const line = lines.slice(at, stop < 0 ? lines.length : stop);
        at = (stop < 0 ? lines.length : stop) + 1;
        if (!line) {
            continue;
        }
        const parts = line.split("\t");
        if (parts.length < 4) {
            continue;
        }
        const path = parts[0];
        const offset = Number(parts[1]);
        const size = Number(parts[2]);
        const depth = parseInt(parts[3], 10);
            const key = path + ":" + offset + ":" + size;
        named.add(key);

        const held = store.entries.get(key);
        if (held) {
            // Already here or on its way: this is how it learns what the
            // range is worth now rather than when it was fetched.
            held.rank = depth;
            held.round = store.round;
            continue;
        }

        // Make room, but only from ranges the script wants later than this
        // one.  Least urgent first; among equals the one whose generation has
        // fallen furthest behind, which for the passed ones - all tied at the
        // same worthless priority - means the longest since the script could
        // reach it at all.  Insertion order settles the rest.
        //
        // Only ranges that have arrived can be dropped: one still in flight
        // has nothing to reclaim and its space is already reserved.
        if (store.bytes + size > budget) {
            const droppable = [...store.entries]
                .filter(([, e]) => !e.keep && e.body)
                .sort((a, b) => (store.priority(b[1]) - store.priority(a[1]))
                             || (a[1].round - b[1].round)
                             || (a[1].order - b[1].order));
            for (const [oldKey, entry] of droppable) {
                if (store.bytes + size <= budget) {
                    break;
                }
                if (store.priority(entry) <= depth) {
                    break;  // Nothing left that is worth less than this.
                }
                store.release(entry);
            }
        }
        if (store.bytes + size > budget) {
            break;  // Everything further down the list is at least as far off.
        }

        // Reserved now, not when the bytes land, so two admissions cannot be
        // let in against the same free space.
        store.bytes += size;
        store.counter = (store.counter || 0) + 1;
        const entry = {body: null, path: path, offset: offset, size: size,
                       keep: false, rank: depth, round: store.round,
                       order: store.counter, started: false};
        entry.promise = new Promise((resolve) => { entry.settle = resolve; });
        store.entries.set(key, entry);
        store.queue.push(entry);
    }
    // A queued range that the scan did not name has left the graph before it
    // ever went out.  Nothing has been spent on it, so drop it rather than
    // let it sit ahead of ranges that are still wanted.  Ones already in
    // flight are left alone: the bandwidth is spent either way, and the bytes
    // may still be useful if the player goes back.
    for (let i = store.queue.length - 1; i >= 0; --i) {
        const entry = store.queue[i];
        const key = entry.path + ":" + entry.offset + ":" + entry.size;
        if (entry.keep || entry.started || named.has(key)) {
            continue;
        }
        store.queue.splice(i, 1);
        store.release(entry);
        entry.settle(null);
    }
    store.pump();
    store.trim();
});

// A range the engine is about to block on, admitted whatever the budget says:
// it is not a guess, and refusing it would only mean fetching it twice.
EM_JS(void, pin_prefetch,
    (const char* url, double offset, double size, int keep), {
    if (!Module.th2Prefetch) {
        th2InitPrefetchStore();
    }
    const store = Module.th2Prefetch;
    const path = UTF8ToString(url);
    const key = path + ":" + offset + ":" + size;
    if (store.entries.has(key)) {
        return;
    }
    store.bytes += size;
    store.counter = (store.counter || 0) + 1;
    const entry = {body: null, path: path, offset: offset, size: size,
                   keep: keep != 0, rank: 0, round: store.round,
                   order: store.counter, started: false};
    entry.promise = new Promise((resolve) => { entry.settle = resolve; });
    store.entries.set(key, entry);
    store.queue.push(entry);
    store.pump();
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
    if (!entry.body) {
        store.promote(entry);
    }
    const body = entry.body ?? await entry.promise;
    if (!body) {
        return -1;  // begin() has already released it.
    }
    // Deliberately kept.  This region is a cache, not a queue of things not
    // yet read: dropping a range the moment it is used meant the 256MB was
    // never more than a few megabytes of in-flight requests, and a second
    // read of the same range - after the layer above it had let go - went
    // back to the network.  It stays until something the script wants sooner
    // needs the room.
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

}  // namespace

std::string data_range_key(
    const std::filesystem::path& path, std::uint64_t offset, std::size_t size)
{
    return range_key(path.string(), offset, size);
}

namespace {

void remember_range(
    const std::string& key, std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > range_cache_entry_limit) {
        return;  // Movie-sized reads are streamed, not cached.
    }
    auto& entries = range_cache();
    auto& index = range_index();
    // Already here.  Two reads of one range can be in flight at once, because
    // both the prefetch store and the network suspend the wasm stack and a
    // second read can begin while the first is parked.  Pushing a duplicate
    // would leave the older copy unreachable through the index but still
    // counted against the budget, and evicting it later would erase the
    // index entry belonging to the newer one.
    if (const auto found = index.find(key); found != index.end()) {
        entries.splice(entries.begin(), entries, found->second);
        return;
    }
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

void data_prefetch_submit(const std::string& sorted_lines)
{
    submit_prefetch(sorted_lines.c_str());
}

void data_prefetch_pin(
    const std::filesystem::path& path, std::uint64_t offset, std::size_t size,
    bool keep)
{
    if (size == 0 || pinned_files().contains(path.string())) {
        return;
    }
    pin_prefetch(
        path.string().c_str(), static_cast<double>(offset),
        static_cast<double>(size), keep ? 1 : 0);
}

void data_prefetch_chunk(
    const std::filesystem::path& path, std::uint64_t offset, std::size_t size)
{
    const std::string key = path.string();
    if (pinned_files().contains(key)) {
        return;
    }
    pin_prefetch(
        key.c_str(), static_cast<double>(offset), static_cast<double>(size),
        1);
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
    if (range_index().contains(cache_key)) {
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

void data_prefetch_submit(const std::string&) {}

void data_prefetch_pin(
    const std::filesystem::path&, std::uint64_t, std::size_t, bool)
{
}

void data_prefetch(
    const std::filesystem::path&, std::uint64_t, std::size_t, int)
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
