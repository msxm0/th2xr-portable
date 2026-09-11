#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace th2 {

// Access to the game data files.  Native builds read them from disk.  The
// browser build fetches byte ranges over HTTP instead: the archives total
// several gigabytes, far more than fits in the wasm heap, so they stay on the
// server and only the bytes actually needed are transferred.
bool data_exists(const std::filesystem::path& path);

// Size in bytes, or 0 when the file cannot be reached.
std::uint64_t data_size(const std::filesystem::path& path);

// Fills destination from offset, returning false on a short or failed read.
bool data_read(
    const std::filesystem::path& path, std::uint64_t offset,
    std::span<std::uint8_t> destination);

// How speculative a prefetch is.  When the store is full the guesses
// furthest from the player are dropped first, so a branch that may never be
// taken cannot displace what the next few lines are about to read.
// How soon the script will want a range, in story order: 0 is the next thing
// it will use, and larger is further off.  A branch does not make what is
// down it less urgent - two ways out of a choice are equally likely to be
// taken - so both carry the depth of the branch point plus their own
// distance, rather than one being demoted for living in another script.
//
// Anything the explorer no longer reaches is `passed`, which is the lowest
// priority there is and the first thing dropped when room is needed.
namespace prefetch_depth {
inline constexpr int imminent = 0;
inline constexpr int passed = 1'000'000;
}  // namespace prefetch_depth

enum class PrefetchRank : int {
    imminent = 0,
    branch = 1,
};

// Starts fetching a range in the background so a later data_read() of the
// same range does not have to wait for the network.  Native builds, where a
// read is a disk read, ignore it.  Safe to call repeatedly: a range already
// in flight, already fetched, or already cached is not requested again.
void data_prefetch(
    const std::filesystem::path& path, std::uint64_t offset, std::size_t size,
    int depth = prefetch_depth::imminent);

// Starts a new round of exploration.  Ranges the round does not ask for again
// are, by that fact, no longer on any path the script can take, and become
// the first candidates for eviction whatever depth they were fetched at.
void data_prefetch_new_round();

// Like data_prefetch(), but for a span that covers many later reads - an
// archive header and directory, say.  It is kept after being read from, and
// any read that falls inside it is served from it.
void data_prefetch_chunk(
    const std::filesystem::path& path, std::uint64_t offset,
    std::size_t size);

// True when a data_read() of this range would not have to go to the network.
bool data_is_resident(
    const std::filesystem::path& path, std::uint64_t offset,
    std::size_t size);

// Reads a whole (small) file; empty when it cannot be read.
std::vector<std::uint8_t> read_data_file(const std::filesystem::path& path);

}  // namespace th2
