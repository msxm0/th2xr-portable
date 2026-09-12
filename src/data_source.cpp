#include "data_source.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

#include <cstring>
#include <list>
#include <string>
#include <unordered_map>
#include <limits>
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
                // Still on the wire and nobody wants it any more - but a run
                // shares one request, so it only dies once every range in it
                // has been let go.  Cancelling on the first would throw away
                // bytes the others are still waiting for.
                const run = entry.run || [entry];
                const wanted = run.some((member) => store.entries.has(
                    member.path + ":" + member.offset + ":" + member.size));
                if (!wanted) {
                    try {
                        entry.abort.abort();
                    } catch (error) {
                        // An abort that cannot be delivered is not worth
                        // failing the eviction for.
                    }
                }
            }
        };
        // One request for a run of ranges that touch.  A scene's voice lines
        // sit next to each other in the archive in script order, so the walk
        // names forty-odd ranges that are one contiguous stretch of bytes and
        // the store asked for each of them separately.  Measured over a
        // 5000-advance run: 1713 requests where 306 would do, voice.pak alone
        // 1321 where 111 would do.  Each of those is a fetch, a Response, an
        // ArrayBuffer and a copy, and the frame-drop bursts line up with them.
        //
        // Only ranges that actually touch are joined.  Allowing a gap would
        // fold the 111 down to 23, but the bytes in the gaps belong to lines
        // the walk did not name, and they would be fetched and thrown away.
        store.joinSlack = 0;
        // No single request larger than this, however long the run is: one
        // huge fetch would hold a slot for its whole duration and delay
        // everything behind it.
        store.joinLimit = 8 << 20;

        // Takes the queued, unstarted neighbours of `lead` that continue its
        // stretch of the file, in both directions, and removes them from the
        // queue.  Returns the run in offset order, `lead` always included.
        store.collect = (lead) => {
            const near = [lead];
            for (const other of store.queue) {
                if (other.started || other.path !== lead.path) {
                    continue;
                }
                near.push(other);
            }
            if (near.length === 1) {
                return near;
            }
            near.sort((a, b) => a.offset - b.offset);
            const at = near.indexOf(lead);
            const run = [lead];
            let first = lead.offset;
            let last = lead.offset + lead.size;
            for (let i = at + 1; i < near.length; ++i) {
                const next = near[i];
                if (next.offset - last > store.joinSlack) {
                    break;
                }
                const end = Math.max(last, next.offset + next.size);
                if (end - first > store.joinLimit) {
                    break;
                }
                last = end;
                run.push(next);
            }
            for (let i = at - 1; i >= 0; --i) {
                const prev = near[i];
                const end = prev.offset + prev.size;
                if (first - end > store.joinSlack) {
                    break;
                }
                if (last - Math.min(first, prev.offset) > store.joinLimit) {
                    break;
                }
                first = Math.min(first, prev.offset);
                run.unshift(prev);
            }
            for (const entry of run) {
                if (entry === lead) {
                    continue;
                }
                const queued = store.queue.indexOf(entry);
                if (queued >= 0) {
                    store.queue.splice(queued, 1);
                }
            }
            return run;
        };

        store.begin = (run) => {
            const entries = Array.isArray(run) ? run : [run];
            const lead = entries[0];
            let first = lead.offset;
            let last = lead.offset + lead.size;
            for (const entry of entries) {
                first = Math.min(first, entry.offset);
                last = Math.max(last, entry.offset + entry.size);
            }
            const span = last - first;
            const abort = typeof AbortController === "function"
                ? new AbortController() : null;
            for (const entry of entries) {
                entry.started = true;
                entry.abort = abort;
                // Releasing one member of a run must not cancel the bytes
                // the others are still waiting on, so the request is only
                // aborted once every member has let go.
                entry.run = entries;
            }
            store.active++;
            fetch(lead.path, {
                headers: {Range: `bytes=${first}-${last - 1}`},
                priority: "low",
                signal: abort ? abort.signal : undefined,
            })
                .then(async (response) => {
                    if (!response.ok) {
                        return null;
                    }
                    if (response.status !== 206) {
                        const length =
                            Number(response.headers.get("Content-Length"));
                        if (length && length !== span) {
                            return null;
                        }
                    }
                    const body = new Uint8Array(await response.arrayBuffer());
                    if (body.length !== span) {
                        return null;
                    }
                    return body;
                })
                .catch(() => null)
                .then((body) => {
                    store.active--;
                    for (const entry of entries) {
                        // Each range keeps a view on the one arrival rather
                        // than a copy of it: the slices do not overlap, and
                        // take_prefetched copies out of them anyway.
                        const slice = body
                            ? body.subarray(entry.offset - first,
                                            entry.offset - first + entry.size)
                            : null;
                        if (slice) {
                            entry.body = slice;
                        } else {
                            // Failed, or aborted because the range left the
                            // graph.  Either way the space set aside for it
                            // goes back, or the budget would shrink with
                            // every miss.
                            store.release(entry);
                        }
                        entry.settle(slice);
                    }
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
                // One issue covers the whole contiguous run, so the
                // per-frame cap counts requests rather than ranges - which
                // is what it was always meant to bound.
                store.begin(store.collect(store.queue.shift()));
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
            // Alone, not as a run: the engine is blocked on this one range,
            // and joining it to neighbours would make it wait for bytes
            // nobody is asking for yet.
            store.begin([entry]);
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
        // No entry of exactly this shape.  Any single one that spans the
        // request serves it just as well - which is how one request covers a
        // whole archive directory, and how a track fetched whole answers a
        // read of its first 64KB.  An entry still in flight counts: waiting
        // on a request already going is cheaper than opening another.
        for (const candidate of store.entries.values()) {
            if (candidate.path === path
                && candidate.offset <= offset
                && candidate.offset + candidate.size >= offset + size) {
                entry = candidate;
                start = offset - candidate.offset;
                break;
            }
        }
    }
    if (!entry) {
        // Still nothing whole, but the bytes may be spread over several
        // arrived ranges - the two halves of a split read, or neighbours
        // from a coalesced run that were admitted separately.  Only ranges
        // that have landed can contribute: stitching would otherwise mean
        // waiting on several requests at once, and a single fresh fetch
        // beats that.
        const pieces = [];
        for (const candidate of store.entries.values()) {
            if (candidate.path === path && candidate.body
                && candidate.offset < offset + size
                && candidate.offset + candidate.size > offset) {
                pieces.push(candidate);
            }
        }
        pieces.sort((a, b) => a.offset - b.offset);
        let at = offset;
        const used = [];
        for (const piece of pieces) {
            if (piece.offset > at) {
                break;  // A hole; the rest cannot help.
            }
            if (piece.offset + piece.size <= at) {
                continue;  // Entirely behind what is already covered.
            }
            used.push(piece);
            at = piece.offset + piece.size;
            if (at >= offset + size) {
                break;
            }
        }
        if (at < offset + size) {
            return 0;
        }
        let filled = 0;
        for (const piece of used) {
            const from = (offset + filled) - piece.offset;
            const count = Math.min(piece.size - from, size - filled);
            HEAPU8.set(piece.body.subarray(from, from + count),
                       destination + filled);
            filled += count;
        }
        return 1;
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
    // Must agree with take_prefetched, or a caller that asks first and reads
    // second gets a different answer than the read would have given.
    const pieces = [];
    for (const candidate of store.entries.values()) {
        if (candidate.path !== path) {
            continue;
        }
        if (candidate.offset <= offset
            && candidate.offset + candidate.size >= offset + size) {
            return 1;
        }
        if (candidate.body && candidate.offset < offset + size
            && candidate.offset + candidate.size > offset) {
            pieces.push(candidate);
        }
    }
    pieces.sort((a, b) => a.offset - b.offset);
    let at = offset;
    for (const piece of pieces) {
        if (piece.offset > at) {
            break;
        }
        at = Math.max(at, piece.offset + piece.size);
        if (at >= offset + size) {
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
//
// The entry limit is set above the largest thing worth caching rather than at
// a round number: BGM tracks run to 6.3MB, and at the old 4MB the longest of
// them were fetched, played and forgotten, so every replay went back to the
// network.  Movies are the case it exists for, and those are hundreds of
// megabytes - nowhere near this.
constexpr std::size_t range_cache_budget = 64ull << 20;
constexpr std::size_t range_cache_entry_limit = 32ull << 20;

std::unordered_map<std::string, std::vector<std::uint8_t>>& pinned_files()
{
    static std::unordered_map<std::string, std::vector<std::uint8_t>> files;
    return files;
}

struct RangeEntry {
    std::string key;
    // The extent, kept alongside the key rather than parsed back out of it:
    // serving a read from bytes that were fetched under some other range
    // means comparing extents on every candidate.
    std::string path;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> bytes;

    std::uint64_t end() const { return offset + bytes.size(); }
};

// Copies whatever part of [offset, offset+destination.size()) this entry
// holds, and returns how far the request is now satisfied from its start.
// Zero when the entry does not reach the wanted bytes at all.
std::size_t take_from(
    const RangeEntry& entry, const std::string& path, std::uint64_t offset,
    std::span<std::uint8_t> destination, std::size_t filled)
{
    if (entry.path != path) {
        return filled;
    }
    const std::uint64_t want_at = offset + filled;
    if (entry.offset > want_at || entry.end() <= want_at) {
        return filled;
    }
    const auto from = static_cast<std::size_t>(want_at - entry.offset);
    const auto count = std::min(
        entry.bytes.size() - from, destination.size() - filled);
    std::memcpy(destination.data() + filled, entry.bytes.data() + from, count);
    return filled + count;
}

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
    const std::string& key, const std::string& path, std::uint64_t offset,
    std::span<const std::uint8_t> bytes)
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
    entries.push_front(
        RangeEntry{key, path, offset, {bytes.begin(), bytes.end()}});
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

// Fills `destination` from whatever the cache already holds, from as many
// entries as it takes.  Ranges are asked for in whatever shape the caller
// wants them - a whole archive entry, the first 64KB of one, the remainder
// after that - so the bytes for a read are often here under some other
// range's name.  Matching only the exact extent meant refetching bytes that
// were already in hand a few entries away.
//
// Entries that contribute are moved to the front, because they were used.
bool serve_from_cache(
    const std::string& path, std::uint64_t offset,
    std::span<std::uint8_t> destination)
{
    auto& entries = range_cache();
    std::size_t filled = 0;
    // Repeated passes: each one takes whatever continues the run, so a read
    // split across several entries is put back together in whatever order
    // they happen to sit in the list.
    bool progressed = true;
    while (filled < destination.size() && progressed) {
        progressed = false;
        for (auto at = entries.begin(); at != entries.end(); ++at) {
            const auto grown =
                take_from(*at, path, offset, destination, filled);
            if (grown == filled) {
                continue;
            }
            filled = grown;
            entries.splice(entries.begin(), entries, at);
            progressed = true;
            break;
        }
    }
    return filled == destination.size();
}

DataCacheStats& data_cache_stats()
{
    static DataCacheStats stats;
    return stats;
}

bool data_read(
    const std::filesystem::path& path, std::uint64_t offset,
    std::span<std::uint8_t> destination)
{
    if (destination.empty()) {
        return true;
    }
    auto& stats = data_cache_stats();
    ++stats.reads;
    const std::string key = path.string();

    if (const auto pinned = pinned_files().find(key);
        pinned != pinned_files().end()) {
        const auto& contents = pinned->second;
        if (offset + destination.size() > contents.size()) {
            return false;
        }
        std::memcpy(
            destination.data(), contents.data() + offset, destination.size());
        ++stats.pinned_hits;
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
        ++stats.pinned_hits;
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
        ++stats.lru_exact;
        return true;
    }
    // No entry of exactly this shape, but the bytes may still be here under
    // one or several others.
    if (serve_from_cache(key, offset, destination)) {
        ++stats.lru_assembled;
        return true;
    }

    // Something asked for this range ahead of time; take it rather than
    // opening a second request for the same bytes.
    const int prefetched = take_prefetched(
        key.c_str(), static_cast<double>(offset),
        static_cast<double>(destination.size()), destination.data());
    if (prefetched == 1) {
        ++stats.store_hits;
        remember_range(cache_key, key, offset, destination);
        return true;
    }

    // Nothing had it: the read blocks on the network, and the frame with it.
    ++stats.blocking;
    stats.blocking_bytes += destination.size();
    if (fetch_range(
            key.c_str(), static_cast<double>(offset),
            static_cast<double>(destination.size()), destination.data())
        == 0) {
        return false;
    }
    remember_range(cache_key, key, offset, destination);
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

DataCacheStats& data_cache_stats()
{
    // Native reads come off the disk and never wait on anything, so the
    // counters exist only to keep the interface the same on both builds.
    static DataCacheStats stats;
    return stats;
}

bool data_read(
    const std::filesystem::path& path, std::uint64_t offset,
    std::span<std::uint8_t> destination)
{
    if (destination.empty()) {
        return true;
    }
    ++data_cache_stats().reads;
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
