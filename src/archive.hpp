#pragma once

#include "data_source.hpp"

#include <array>
#include <limits>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <span>
#include <unordered_map>
#include <vector>

namespace th2 {

enum class ArchiveKind {
    kcap,
    lac,
};

struct ArchiveEntry {
    std::string name;
    std::uint32_t offset;
    std::uint32_t stored_size;
    bool compressed;
};

// The one-shot decoder.  Exposed so the resumable one below can be held
// against it: agreeing with it is the property that matters.
std::vector<std::uint8_t> decompress_lzs(
    std::span<const std::uint8_t> source, std::size_t output_size,
    std::size_t max_output = std::numeric_limits<std::size_t>::max());

// Decompresses an LZS stream a slice at a time.
//
// The one-shot decompress_lzs() runs to completion, and a large picture can
// hold the thread for twenty milliseconds - three vblanks - which is not
// something a frame can absorb.  The algorithm is LZ77 over a 4KB ring, and
// all of its state is four values, so it stops and resumes cheaply.  The
// clock is only read every few thousand bytes: checking it per byte costs
// more than the decompression.
class LzsStream {
public:
    // The compressed bytes are copied in, because the caller that started this
    // is not going to be alive on the frame that finishes it.
    LzsStream(std::vector<std::uint8_t> source, std::size_t output_size);

    // Decompresses until the budget runs out or the stream ends.  True when
    // there is nothing left to do.
    bool advance(std::chrono::nanoseconds budget);
    bool done() const { return done_; }
    // Only meaningful once done(); moves the result out.
    std::vector<std::uint8_t> take();
    std::size_t produced() const { return output_.size(); }
    std::size_t expected() const { return limit_; }

private:
    static constexpr std::size_t ring_size = 4096;
    static constexpr std::size_t lookahead = 18;
    // Bytes between clock reads.  Big enough that the check is noise, small
    // enough that a 4ms slice is not badly overshot.
    static constexpr std::size_t check_interval = 8192;

    std::vector<std::uint8_t> source_;
    std::vector<std::uint8_t> output_;
    std::array<std::uint8_t, ring_size> ring_{};
    std::size_t ring_position_ = ring_size - lookahead;
    std::size_t source_position_ = 0;
    std::size_t limit_ = 0;
    unsigned flags_ = 0;
    bool done_ = false;
};

class Archive {
public:
    explicit Archive(const std::filesystem::path& path);

    ArchiveKind kind() const { return kind_; }
    const std::filesystem::path& path() const { return path_; }
    const std::vector<ArchiveEntry>& entries() const { return entries_; }
    const ArchiveEntry* find(std::string_view name) const;
    std::vector<std::uint8_t> read(const ArchiveEntry& entry) const;

    // The entry as it sits in the file, with the header stripped and the size
    // it expands to.  For work that is being done early, so it can be handed
    // to LzsStream and spread over several frames instead of decompressed in
    // one go.
    struct StoredEntry {
        std::vector<std::uint8_t> bytes;
        std::size_t output_size = 0;
        bool compressed = false;
    };
    StoredEntry read_stored(const ArchiveEntry& entry) const;
    // Decompresses only the first max_bytes of an entry, for callers that
    // just need its beginning.
    std::vector<std::uint8_t> read_prefix(
        const ArchiveEntry& entry, std::size_t max_bytes) const;

    // Starts fetching an entry in the background so a later read() of it does
    // not wait on the network.  A no-op on the native builds.  The rank says
    // how speculative the guess is; see PrefetchRank.
    // Where an entry lives, for a caller assembling a scan's wanted list.
    struct Range {
        std::string path;
        std::uint64_t offset = 0;
        std::size_t size = 0;
    };
    Range range_of(const ArchiveEntry& entry) const;
    // True when read() would not have to go to the network for this entry.
    bool resident(const ArchiveEntry& entry) const;

private:
    std::filesystem::path path_;
    ArchiveKind kind_;
    std::vector<ArchiveEntry> entries_;
    // Lower-cased name to index, built on the first find().
    mutable std::unordered_map<std::string, std::size_t> index_;
};

}  // namespace th2
