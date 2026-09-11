#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>

namespace th2 {

// What the script is going to want, and how soon.
//
// Depth is position in the story, not distance in bytes: the next voice line
// is depth 0, whatever the script needs after that is depth 1, and so on.  A
// branch does not make its successors less urgent - if the script can go two
// ways, the first asset down each way is equally likely to be the next one
// needed, so both sit at the same depth.  That is the whole point of grading
// by depth rather than by "this script" versus "some other script": a choice
// three lines away used to rank below a background thirty lines away in the
// current script, which is backwards.
//
// Anything the script has already passed is not in the plan at all, and
// answers `passed` - the lowest priority there is, and the first thing to go
// when something else needs the room.
class AssetPlan {
public:
    // Lower is more urgent.  Far enough above any real depth that arithmetic
    // on it cannot collide with one.
    static constexpr int passed = 1'000'000;

    void begin()
    {
        previous_.swap(depths_);
        depths_.clear();
    }

    // The shallowest sighting wins: an asset reachable both two lines ahead
    // and again after a branch is as urgent as its nearest use.
    void note(std::string key, int depth)
    {
        const auto found = depths_.find(key);
        if (found == depths_.end()) {
            depths_.emplace(std::move(key), depth);
        } else {
            found->second = std::min(found->second, depth);
        }
    }

    int depth_of(const std::string& key) const
    {
        const auto found = depths_.find(key);
        return found == depths_.end() ? passed : found->second;
    }

    // True while the plan is empty - before the first scan, when treating
    // everything as passed would evict the whole cache.
    bool empty() const { return depths_.empty(); }
    std::size_t size() const { return depths_.size(); }

    // What the previous scan thought, for reporting how much the plan churns.
    int previous_depth_of(const std::string& key) const
    {
        const auto found = previous_.find(key);
        return found == previous_.end() ? passed : found->second;
    }

private:
    std::unordered_map<std::string, int> depths_;
    std::unordered_map<std::string, int> previous_;
};

// How far ahead to look along any one path.  Whichever runs out first stops
// that path: the point is to cover what the player can reach next, not to
// walk the whole script.
struct ScanLimits {
    int voices = 5;
    int backgrounds = 5;
    int sprites = 10;
    int instructions = 200;
};

}  // namespace th2
