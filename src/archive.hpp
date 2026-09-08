#pragma once

#include "data_source.hpp"

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

class Archive {
public:
    explicit Archive(const std::filesystem::path& path);

    ArchiveKind kind() const { return kind_; }
    const std::filesystem::path& path() const { return path_; }
    const std::vector<ArchiveEntry>& entries() const { return entries_; }
    const ArchiveEntry* find(std::string_view name) const;
    std::vector<std::uint8_t> read(const ArchiveEntry& entry) const;
    // Decompresses only the first max_bytes of an entry, for callers that
    // just need its beginning.
    std::vector<std::uint8_t> read_prefix(
        const ArchiveEntry& entry, std::size_t max_bytes) const;

    // Starts fetching an entry in the background so a later read() of it does
    // not wait on the network.  A no-op on the native builds.  The rank says
    // how speculative the guess is; see PrefetchRank.
    void prefetch(
        const ArchiveEntry& entry,
        PrefetchRank rank = PrefetchRank::imminent) const;
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
