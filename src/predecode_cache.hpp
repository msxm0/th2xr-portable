#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace th2 {

// What a predecoder holds, and how it decides what to let go of.
//
// The same shape as the byte store one layer down: the explorer names what it
// can reach, anything it names is live, and anything it stops naming becomes
// stale and is the first to go - oldest generation first, so the thing the
// script has been away from longest loses its place before the one it left a
// moment ago.
//
// Capacity is counted in objects rather than bytes because what is held is a
// second of audio or one picture, not a whole track: twenty live and ten
// stale is a working set plus a grace buffer, so stepping back over a line
// already read finds its work still done.
template <typename Value>
class PredecodeCache {
public:
    static constexpr std::size_t live_limit = 20;
    static constexpr std::size_t capacity = 30;

    struct Entry {
        Value value;
        int generation = 0;
        int depth = 0;
    };

    // Says this object is wanted again, at this distance.  Called for
    // everything the latest scan found, whether it is held yet or not.
    void touch(const std::string& key, int generation, int depth)
    {
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            found->second.generation = generation;
            found->second.depth = depth;
        }
    }

    bool contains(const std::string& key) const
    {
        return entries_.contains(key);
    }

    Value* find(const std::string& key)
    {
        const auto found = entries_.find(key);
        return found == entries_.end() ? nullptr : &found->second.value;
    }

    // Makes room if there is none, then stores.  Returns false when nothing
    // could be given up for it - which means everything held is wanted at
    // least as soon, and decoding this would only displace something needed
    // first.
    bool insert(const std::string& key, Value value, int generation,
                int depth)
    {
        if (entries_.size() >= capacity && !evict_for(generation, depth)) {
            return false;
        }
        entries_.insert_or_assign(key, Entry{std::move(value), generation,
                                             depth});
        return true;
    }

    Value take(const std::string& key)
    {
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            return Value{};
        }
        Value value = std::move(found->second.value);
        entries_.erase(found);
        return value;
    }

    void erase(const std::string& key) { entries_.erase(key); }
    void clear() { entries_.clear(); }
    std::size_t size() const { return entries_.size(); }

    // Held but no longer named by the latest scan.
    std::size_t stale(int generation) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            entries_, [&](const auto& item) {
                return item.second.generation != generation;
            }));
    }

private:
    // Stale first, oldest generation first among those; if nothing is stale,
    // whatever the script wants latest, and only if that is later than what
    // is asking for the room.
    bool evict_for(int generation, int depth)
    {
        auto worst = entries_.end();
        for (auto at = entries_.begin(); at != entries_.end(); ++at) {
            if (worst == entries_.end()) {
                worst = at;
                continue;
            }
            const bool at_stale = at->second.generation != generation;
            const bool worst_stale = worst->second.generation != generation;
            if (at_stale != worst_stale) {
                if (at_stale) {
                    worst = at;
                }
                continue;
            }
            if (at_stale) {
                if (at->second.generation < worst->second.generation) {
                    worst = at;
                }
            } else if (at->second.depth > worst->second.depth) {
                worst = at;
            }
        }
        if (worst == entries_.end()) {
            return false;
        }
        const bool worst_stale = worst->second.generation != generation;
        if (!worst_stale && worst->second.depth <= depth) {
            return false;  // Everything held is wanted at least as soon.
        }
        entries_.erase(worst);
        return true;
    }

    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace th2
