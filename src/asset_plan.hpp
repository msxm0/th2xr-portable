#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>

namespace th2 {

// What the script is going to want, and how soon.
//
// Depth is instructions from the interpreter along the path that reaches the
// asset: the next thing it will touch is a handful of steps away, something
// past a choice is however far the choice is plus its own distance.  Branches
// and script loads are followed, so two ways out of a choice are graded by
// how far into each the asset sits rather than by which script it lives in.
//
// Rebuilt from the interpreter on every scan.  Walking a few hundred
// instructions costs nothing worth measuring, and doing it afresh means a
// priority can never be stale and nothing has to be invalidated when the
// story moves - which is what went wrong when priorities were recorded once
// and refreshed by announcing an asset again, because announcing was a no-op
// for anything already requested.
//
// Anything the walk does not reach is `passed`: the lowest priority there is,
// and the first thing dropped when something else needs the room.
class AssetPlan {
public:
    static constexpr int passed = 1'000'000;

    void begin() { depths_.clear(); }

    // The shallowest sighting wins: an asset used soon and again later, or
    // reachable down two branches, is as urgent as its nearest use.
    void note(const std::string& key, int depth)
    {
        const auto found = depths_.find(key);
        if (found == depths_.end()) {
            depths_.emplace(key, depth);
        } else {
            found->second = std::min(found->second, depth);
        }
    }

    int depth_of(const std::string& key) const
    {
        const auto found = depths_.find(key);
        return found == depths_.end() ? passed : found->second;
    }

    std::size_t size() const { return depths_.size(); }

private:
    std::unordered_map<std::string, int> depths_;
};

}  // namespace th2
