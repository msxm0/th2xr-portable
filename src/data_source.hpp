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
enum class PrefetchRank : int {
    imminent = 0,  // the script the interpreter is in, ahead of its pc
    branch = 1,    // the opening of a script this one can reach
};

// Starts fetching a range in the background so a later data_read() of the
// same range does not have to wait for the network.  Native builds, where a
// read is a disk read, ignore it.  Safe to call repeatedly: a range already
// in flight, already fetched, or already cached is not requested again.
void data_prefetch(
    const std::filesystem::path& path, std::uint64_t offset, std::size_t size,
    PrefetchRank rank = PrefetchRank::imminent);

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
