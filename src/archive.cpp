#include "archive.hpp"

#include "data_source.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace th2 {
namespace {

std::uint32_t read_u32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string ascii_lower(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

// max_output stops the stream early, for callers that only need the start of
// a file; the size check is then skipped, since a prefix is expected.
std::vector<std::uint8_t> decompress_lzs(
    std::span<const std::uint8_t> source, std::size_t output_size,
    std::size_t max_output = std::numeric_limits<std::size_t>::max())
{
    const std::size_t limit = std::min(output_size, max_output);
    constexpr std::size_t ring_size = 4096;
    constexpr std::size_t lookahead = 18;
    std::array<std::uint8_t, ring_size> ring{};
    ring.fill(' ');
    std::size_t ring_position = ring_size - lookahead;
    std::vector<std::uint8_t> output;
    output.reserve(limit);
    std::size_t source_position = 0;
    unsigned flags = 0;

    while (source_position < source.size() && output.size() < limit) {
        flags >>= 1;
        if ((flags & 0x100) == 0) {
            flags = source[source_position++] | 0xff00;
        }
        if (source_position >= source.size()) {
            break;
        }

        int first = source[source_position++];
        if (flags & 1) {
            const auto value = static_cast<std::uint8_t>(first);
            output.push_back(value);
            ring[ring_position++ & (ring_size - 1)] = value;
        } else {
            if (source_position >= source.size()) {
                break;
            }
            const int second = source[source_position++];
            int position = first | ((second & 0xf0) << 4);
            const int length = (second & 0x0f) + 3;
            for (int i = 0; i < length && output.size() < limit; ++i) {
                const auto value = ring[position++ & (ring_size - 1)];
                output.push_back(value);
                ring[ring_position++ & (ring_size - 1)] = value;
            }
        }
    }

    if (limit >= output_size && output.size() != output_size) {
        throw std::runtime_error("LZS stream produced an unexpected size");
    }
    return output;
}

std::string fixed_string(const char* data, std::size_t size)
{
    std::size_t length = 0;
    while (length < size) {
        const auto byte = static_cast<unsigned char>(data[length]);
        if (byte == 0 || byte == 0xff) {
            break;
        }
        ++length;
    }
    return std::string(data, length);
}

void validate_entry(
    const ArchiveEntry& entry, std::uintmax_t archive_size,
    const std::filesystem::path& path)
{
    const auto end = static_cast<std::uintmax_t>(entry.offset) + entry.stored_size;
    if (end > archive_size) {
        throw std::runtime_error(
            path.string() + ": entry extends beyond end of archive: " + entry.name);
    }
}

}  // namespace

Archive::Archive(const std::filesystem::path& path)
    : path_(path)
{
    constexpr std::size_t header_size = 8;
    constexpr std::size_t kcap_entry_size = 36;
    constexpr std::size_t lac_entry_size = 40;

    const auto archive_size = data_size(path);
    std::array<std::uint8_t, header_size> header{};
    if (archive_size < header_size || !data_read(path, 0, header)) {
        throw std::runtime_error("cannot open " + path.string());
    }
    const std::array<char, 4> magic{
        static_cast<char>(header[0]), static_cast<char>(header[1]),
        static_cast<char>(header[2]), static_cast<char>(header[3])};
    const std::uint32_t count = read_u32(header.data() + 4);

    std::size_t entry_size = 0;
    const char* kind_name = nullptr;
    if (magic == std::array<char, 4>{'K', 'C', 'A', 'P'}) {
        kind_ = ArchiveKind::kcap;
        entry_size = kcap_entry_size;
        kind_name = "KCAP";
    } else if (magic == std::array<char, 4>{'L', 'A', 'C', '\0'}) {
        kind_ = ArchiveKind::lac;
        entry_size = lac_entry_size;
        kind_name = "LAC";
    } else {
        throw std::runtime_error(path.string() + ": unsupported archive magic");
    }

    // The directory is read in a single request: the browser build pays a
    // network round trip per read, and voice.pak alone holds 29000 entries.
    const std::uint64_t directory_size =
        static_cast<std::uint64_t>(count) * entry_size;
    if (header_size + directory_size > archive_size) {
        throw std::runtime_error(
            path.string() + ": truncated " + kind_name + " directory");
    }
    std::vector<std::uint8_t> directory(
        static_cast<std::size_t>(directory_size));
    if (!data_read(path, header_size, directory)) {
        throw std::runtime_error(
            path.string() + ": truncated " + kind_name + " directory");
    }

    entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* record = directory.data() + i * entry_size;
        ArchiveEntry entry{};
        if (kind_ == ArchiveKind::kcap) {
            entry = ArchiveEntry{
                fixed_string(reinterpret_cast<const char*>(record + 4), 24),
                read_u32(record + 28),
                read_u32(record + 32),
                read_u32(record) != 0,
            };
        } else {
            std::array<char, 31> name{};
            for (std::size_t byte = 0; byte < name.size(); ++byte) {
                name[byte] = static_cast<char>(~record[byte]);
            }
            entry = ArchiveEntry{
                fixed_string(name.data(), name.size()),
                read_u32(record + 36),
                read_u32(record + 32),
                record[31] != 0,
            };
        }
        validate_entry(entry, archive_size, path);
        entries_.push_back(std::move(entry));
    }
}

const ArchiveEntry* Archive::find(std::string_view name) const
{
    // GRP.PAK alone holds tens of thousands of entries and the engine looks
    // one up for every asset it touches - and the prefetch scan looks up
    // many more - so the names are indexed on first use rather than scanned.
    if (index_.empty() && !entries_.empty()) {
        index_.reserve(entries_.size());
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            index_.emplace(ascii_lower(entries_[i].name), i);
        }
    }
    const auto found = index_.find(ascii_lower(name));
    return found == index_.end() ? nullptr : &entries_[found->second];
}

void Archive::prefetch(const ArchiveEntry& entry, PrefetchRank rank) const
{
    data_prefetch(path_, entry.offset, entry.stored_size, rank);
}

bool Archive::resident(const ArchiveEntry& entry) const
{
    return data_is_resident(path_, entry.offset, entry.stored_size);
}

std::vector<std::uint8_t> Archive::read_prefix(
    const ArchiveEntry& entry, std::size_t max_bytes) const
{
    std::vector<std::uint8_t> stored(entry.stored_size);
    if (!data_read(path_, entry.offset, stored)) {
        throw std::runtime_error(
            path_.string() + ": cannot read " + entry.name);
    }
    if (!entry.compressed) {
        stored.resize(std::min<std::size_t>(stored.size(), max_bytes));
        return stored;
    }
    const std::size_t header = kind_ == ArchiveKind::lac ? 4 : 8;
    if (stored.size() < header) {
        throw std::runtime_error(
            path_.string() + ": invalid compressed entry");
    }
    const auto size_offset = kind_ == ArchiveKind::lac ? 0 : 4;
    return decompress_lzs(
        std::span<const std::uint8_t>(stored).subspan(header),
        read_u32(stored.data() + size_offset), max_bytes);
}

std::vector<std::uint8_t> Archive::read(const ArchiveEntry& entry) const
{
    std::vector<std::uint8_t> stored(entry.stored_size);
    if (!data_read(path_, entry.offset, stored)) {
        throw std::runtime_error(path_.string() + ": cannot read " + entry.name);
    }
    if (!entry.compressed) {
        return stored;
    }
    if (kind_ == ArchiveKind::lac) {
        if (stored.size() < 4) {
            throw std::runtime_error(path_.string() + ": invalid compressed LAC entry");
        }
        return decompress_lzs(
            std::span<const std::uint8_t>(stored).subspan(4), read_u32(stored.data()));
    }
    if (stored.size() < 8) {
        throw std::runtime_error(path_.string() + ": invalid compressed KCAP entry");
    }
    return decompress_lzs(
        std::span<const std::uint8_t>(stored).subspan(8), read_u32(stored.data() + 4));
}

}  // namespace th2
