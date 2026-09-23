// Compare two trace frame dumps tick by tick.
//
// Both sides write the same thing - the reference build through
// th2ref_dump_frame, ours through write_trace_frame - a "TH2REF <w> <h> 24\n"
// header line followed by w*h BGR triples, top row first.  Nothing between
// the framebuffer and the file re-encodes a pixel, so a difference here is a
// difference in what was drawn rather than in how it was stored.
//
//     th2-trace-diff <dir-a> <dir-b> [options]
//
//       --offset N   add to A's tick to get B's tick
//       --align      search for the offset that matches best, and use it
//       --from N --to N --step N
//       --psnr DB    below this a tick counts as a mismatch (default 40)
//       --max-delta N  the largest a single channel may differ by (default 2)
//       --bmp TICK   write BMPs of that tick from both sides
//       --rect X,Y,W,H  compare only this rectangle
//       --quiet      only the summary and the mismatches
//       --survey     don't compare; just report what is on screen in A,
//                    which is how you find the stretch worth comparing
//
// --rect is for the parts of the screen the two sides are never going to
// agree on because they are not trying to.  The 2002 system bar down the
// right edge, x 772..799, is the one that matters here: our port replaced it
// with the ImGui menu on purpose, so leaving it in would report a permanent
// 30 dB penalty that hides every real difference underneath it.
//
// Two gates, and they fail on different things.  PSNR is an average: one
// badly wrong pixel in a frame of 460,000 barely moves it, so a misplaced
// sprite or a wipe a frame out of step can sail through.  The maximum
// per-channel difference catches exactly that, and is blind to the broad,
// tiny disagreement that PSNR is good at spotting.  A tick has to pass both.
//
// The offset is what the two sides need in practice: the reference starts at
// the title screen and reaches the scenario only after a click, while ours
// starts the scenario at tick 0.  Rather than making the caller guess the
// lag, --align probes a frame in the middle and takes the shift that lines up.

#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <utility>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgr;
};

// Reads one file of the dump format both sides write: a text header
// "TH2REFZ <w> <h> 24 <key> <bytes>\n" and a deflate stream of either the
// picture or its per-byte difference from the previous tick's.
//
// A short or mistyped file is an error rather than something to skip: a
// comparison that silently drops frames would report agreement it never
// checked.
struct Packed {
    int width = 0;
    int height = 0;
    bool key = false;
    std::size_t bytes = 0;
    std::vector<std::uint8_t> deflated;
};

std::optional<Packed> read_packed(
    const std::filesystem::path& path, std::string& error)
{
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (!file) {
        error = "cannot open " + path.string();
        return std::nullopt;
    }
    Packed out;
    int depth = 0, key = 0;
    unsigned long long bytes = 0;
    // No "\n" in the format string: to scanf a whitespace directive means
    // "consume every whitespace byte you can", and the byte after the header
    // is the first byte of the deflate stream.  Whenever that happened to be
    // whitespace it was swallowed as part of the header and the stream came
    // up short.  Under the old uncompressed format the same bug ate a 0x20
    // pixel and hid for a long time because frames starting on black were
    // unaffected; here it would be every file whose stream happens to start
    // that way, so the newline is consumed explicitly.
    if (std::fscanf(file, "TH2REFZ %d %d %d %d %llu",
                    &out.width, &out.height, &depth, &key, &bytes) != 5) {
        std::fclose(file);
        error = path.string() + ": not a TH2REFZ dump";
        return std::nullopt;
    }
    if (std::fgetc(file) != '\n') {
        std::fclose(file);
        error = path.string() + ": header is not newline terminated";
        return std::nullopt;
    }
    if (depth != 24 || out.width <= 0 || out.height <= 0) {
        std::fclose(file);
        error = path.string() + ": unsupported "
            + std::to_string(out.width) + "x" + std::to_string(out.height)
            + " at " + std::to_string(depth) + "bpp";
        return std::nullopt;
    }
    out.key = key != 0;
    out.bytes = static_cast<std::size_t>(bytes);
    if (out.bytes
        != static_cast<std::size_t>(out.width) * out.height * 3) {
        std::fclose(file);
        error = path.string() + ": header claims "
            + std::to_string(out.bytes) + " bytes for a "
            + std::to_string(out.width) + "x" + std::to_string(out.height)
            + " frame";
        return std::nullopt;
    }
    std::uint8_t chunk[65536];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
        out.deflated.insert(out.deflated.end(), chunk, chunk + read);
    }
    std::fclose(file);
    if (out.deflated.empty()) {
        error = path.string() + ": no deflate stream after the header";
        return std::nullopt;
    }
    return out;
}

// Decodes one file given the frame before it, which is needed unless the
// file is a key frame.  `previous` is left holding this frame, so a
// sequential walk hands each decode its own predecessor for free.
bool inflate_frame(const std::filesystem::path& path, Frame& previous,
                   std::string& error)
{
    const auto packed = read_packed(path, error);
    if (!packed) {
        return false;
    }
    std::vector<std::uint8_t> plain(packed->bytes);
    uLongf plain_size = static_cast<uLongf>(packed->bytes);
    const int status = uncompress(
        plain.data(), &plain_size, packed->deflated.data(),
        static_cast<uLong>(packed->deflated.size()));
    if (status != Z_OK || plain_size != packed->bytes) {
        error = path.string() + ": deflate stream will not inflate"
            " (zlib " + std::to_string(status) + ")";
        return false;
    }
    if (!packed->key) {
        if (previous.bgr.size() != packed->bytes) {
            error = path.string() + ": needs the frame before it, which is"
                " missing or a different size";
            return false;
        }
        for (std::size_t i = 0; i < plain.size(); ++i) {
            plain[i] = static_cast<std::uint8_t>(plain[i] + previous.bgr[i]);
        }
    }
    previous.width = packed->width;
    previous.height = packed->height;
    previous.bgr = std::move(plain);
    return true;
}

std::map<std::uint64_t, std::filesystem::path> ticks(
    const std::filesystem::path& directory)
{
    std::map<std::uint64_t, std::filesystem::path> out;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        const auto name = entry.path().filename().string();
        std::uint64_t tick = 0;
        // f%06lu.binz on both sides; anything else in the directory is not
        // ours.
        if (std::sscanf(name.c_str(), "f%llu.binz",
                        reinterpret_cast<unsigned long long*>(&tick)) == 1
            && name.size() > 5
            && name.compare(name.size() - 5, 5, ".binz") == 0) {
            out.emplace(tick, entry.path());
        }
    }
    return out;
}

// Decodes frames out of one directory, keeping the last one it produced.
//
// Asked for the tick after the one it holds, a difference frame costs a
// single inflate.  Asked for anything else, it walks back to the nearest key
// frame and replays forward - at most one key period, because the writers
// emit a key frame that often for exactly this reason.  Every walk is
// bounded and every frame is exact; the cache is a speed, not a semantics.
class Reader {
public:
    Reader(std::map<std::uint64_t, std::filesystem::path> ticks,
           std::string name)
        : ticks_(std::move(ticks)), name_(std::move(name))
    {
    }

    const std::map<std::uint64_t, std::filesystem::path>& ticks() const
    {
        return ticks_;
    }

    // Returns nullptr and sets `error` rather than throwing: the align pass
    // probes frames it is willing to skip.
    const Frame* at(std::uint64_t tick, std::string& error)
    {
        const auto wanted = ticks_.find(tick);
        if (wanted == ticks_.end()) {
            error = name_ + ": no frame for tick " + std::to_string(tick);
            return nullptr;
        }
        if (have_ && cached_tick_ == tick) {
            return &cache_;
        }
        // Where to start replaying from: the frame after the one already
        // decoded when that one is this frame's predecessor, otherwise the
        // nearest key frame at or before the target.
        // Replay onward from the cached frame only when that frame really is
        // this one's predecessor.  Testing `cached_tick_ < tick` assumed the
        // dump was contiguous, which it is not when frames are sampled: with
        // tick 60 cached and 120 asked for, it set start to 61 and then
        // demanded the fifty-nine frames that were never dumped.  A sampled
        // dump writes every frame as a key frame, so the search below finds
        // one immediately.
        const bool incremental = have_ && cached_tick_ + 1 == tick;
        std::uint64_t start = tick;
        if (!incremental) {
            auto walk = wanted;
            while (true) {
                const auto packed = read_packed(walk->second, error);
                if (!packed) {
                    return nullptr;
                }
                if (packed->key) {
                    break;
                }
                if (walk == ticks_.begin()) {
                    error = walk->second.string() + ": a difference frame with"
                        " nothing before it in " + name_;
                    return nullptr;
                }
                const auto previous = std::prev(walk);
                if (previous->first + 1 != walk->first) {
                    error = walk->second.string() + ": a difference frame"
                        " whose predecessor (tick "
                        + std::to_string(walk->first - 1) + ") was not dumped";
                    return nullptr;
                }
                walk = previous;
            }
            start = walk->first;
            have_ = false;
        }
        for (std::uint64_t t = start; t <= tick; ++t) {
            const auto step = ticks_.find(t);
            if (step == ticks_.end()) {
                error = name_ + ": no frame for tick " + std::to_string(t)
                    + ", needed to reach " + std::to_string(tick);
                return nullptr;
            }
            if (!inflate_frame(step->second, cache_, error)) {
                have_ = false;
                return nullptr;
            }
        }
        cached_tick_ = tick;
        have_ = true;
        return &cache_;
    }

private:
    std::map<std::uint64_t, std::filesystem::path> ticks_;
    std::string name_;
    Frame cache_;
    std::uint64_t cached_tick_ = 0;
    bool have_ = false;
};

// A window onto a frame.  Defaults to the whole of it; --rect narrows it.
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;

    void clamp_to(const Frame& frame)
    {
        if (width <= 0 || height <= 0) {
            x = y = 0;
            width = frame.width;
            height = frame.height;
        }
        x = std::clamp(x, 0, frame.width);
        y = std::clamp(y, 0, frame.height);
        width = std::min(width, frame.width - x);
        height = std::min(height, frame.height - y);
    }
};

struct Stats {
    double mean_abs = 0.0;
    double psnr = 0.0;
    double differing = 0.0;   // fraction of bytes that differ at all
    int max_delta = 0;        // the worst single channel, 0..255
};

Stats compare(const Frame& a, const Frame& b, const Rect& rect)
{
    const auto stride = static_cast<std::size_t>(a.width) * 3;
    const auto span = static_cast<std::size_t>(rect.width) * 3;
    const std::size_t count = span * static_cast<std::size_t>(rect.height);
    std::uint64_t total = 0;
    std::uint64_t square = 0;
    std::uint64_t differing = 0;
    int worst = 0;
    for (int y = 0; y < rect.height; ++y) {
        const auto row = static_cast<std::size_t>(rect.y + y) * stride
            + static_cast<std::size_t>(rect.x) * 3;
        for (std::size_t i = 0; i < span; ++i) {
            const int d = static_cast<int>(a.bgr[row + i])
                        - static_cast<int>(b.bgr[row + i]);
            if (d == 0) {
                continue;
            }
            ++differing;
            const auto magnitude = static_cast<std::uint64_t>(d < 0 ? -d : d);
            total += magnitude;
            square += magnitude * magnitude;
            if (static_cast<int>(magnitude) > worst) {
                worst = static_cast<int>(magnitude);
            }
        }
    }
    Stats stats;
    stats.max_delta = worst;
    stats.mean_abs = static_cast<double>(total) / static_cast<double>(count);
    stats.differing = static_cast<double>(differing) / static_cast<double>(count);
    const double mse = static_cast<double>(square) / static_cast<double>(count);
    stats.psnr = mse == 0.0
        ? std::numeric_limits<double>::infinity()
        : 10.0 * std::log10(255.0 * 255.0 / mse);
    return stats;
}

// What is on screen inside the window: the mean level, and the mean absolute
// deviation from it.  The spread is what ranks "has something on it" above
// "is one flat colour", which is the only distinction the callers need.
std::pair<double, double> mean_and_spread(const Frame& frame, const Rect& rect)
{
    const auto stride = static_cast<std::size_t>(frame.width) * 3;
    const auto span = static_cast<std::size_t>(rect.width) * 3;
    const auto count = span * static_cast<std::size_t>(rect.height);
    if (count == 0) {
        return {0.0, 0.0};
    }
    const auto each = [&](auto&& visit) {
        for (int y = 0; y < rect.height; ++y) {
            const auto row = static_cast<std::size_t>(rect.y + y) * stride
                + static_cast<std::size_t>(rect.x) * 3;
            for (std::size_t i = 0; i < span; ++i) {
                visit(frame.bgr[row + i]);
            }
        }
    };
    std::uint64_t sum = 0;
    each([&](std::uint8_t byte) { sum += byte; });
    const double mean = static_cast<double>(sum) / static_cast<double>(count);
    double spread = 0.0;
    each([&](std::uint8_t byte) {
        spread += std::fabs(static_cast<double>(byte) - mean);
    });
    return {mean, spread / static_cast<double>(count)};
}

// A BMP so a mismatching tick can actually be looked at.  BMP rather than
// anything compressed because a 24-bit bottom-up BMP *is* this format with a
// header in front of it - there is no encoder in the path that could turn a
// real difference into a rounding one.
bool write_bmp(const std::filesystem::path& path, const Frame& frame)
{
    const auto stride = static_cast<std::size_t>(frame.width) * 3;
    const std::size_t padding = (4 - stride % 4) % 4;
    const auto row = stride + padding;
    const auto pixels = row * static_cast<std::size_t>(frame.height);
    const std::uint32_t offset = 14 + 40;

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (!file) {
        return false;
    }
    std::vector<std::uint8_t> header(offset, 0);
    const auto put16 = [&](std::size_t at, std::uint16_t v) {
        header[at] = static_cast<std::uint8_t>(v);
        header[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            header[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    };
    header[0] = 'B';
    header[1] = 'M';
    put32(2, static_cast<std::uint32_t>(offset + pixels));
    put32(10, offset);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(frame.width));
    put32(22, static_cast<std::uint32_t>(frame.height));   // positive: bottom-up
    put16(26, 1);
    put16(28, 24);
    put32(34, static_cast<std::uint32_t>(pixels));
    std::fwrite(header.data(), 1, header.size(), file);

    static const std::uint8_t pad[3] = {0, 0, 0};
    for (int y = frame.height - 1; y >= 0; --y) {
        std::fwrite(frame.bgr.data() + static_cast<std::size_t>(y) * stride,
                    1, stride, file);
        if (padding) {
            std::fwrite(pad, 1, padding, file);
        }
    }
    std::fclose(file);
    return true;
}

[[noreturn]] void fail(const std::string& message)
{
    std::fprintf(stderr, "th2-trace-diff: %s\n", message.c_str());
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv)
{
    std::filesystem::path a;
    std::filesystem::path b;
    std::int64_t offset = 0;
    std::uint64_t first = 0;
    std::uint64_t last = ~std::uint64_t{0};
    std::uint64_t step = 1;
    double threshold = 40.0;
    int max_delta_limit = 2;
    std::optional<std::uint64_t> bmp_tick;
    std::filesystem::path bmp_dir = ".";
    bool align = false;
    bool quiet = false;
    bool survey = false;
    Rect rect;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto value = [&](const char* what) -> std::string {
            if (++i >= argc) {
                fail(std::string(what) + " needs a value");
            }
            return argv[i];
        };
        if (argument == "--offset") {
            offset = std::stoll(value("--offset"));
        } else if (argument == "--from") {
            first = std::stoull(value("--from"));
        } else if (argument == "--to") {
            last = std::stoull(value("--to"));
        } else if (argument == "--step") {
            step = std::max<std::uint64_t>(1, std::stoull(value("--step")));
        } else if (argument == "--psnr") {
            threshold = std::stod(value("--psnr"));
        } else if (argument == "--max-delta") {
            max_delta_limit = std::stoi(value("--max-delta"));
        } else if (argument == "--bmp") {
            bmp_tick = std::stoull(value("--bmp"));
        } else if (argument == "--bmp-dir") {
            bmp_dir = value("--bmp-dir");
        } else if (argument == "--align") {
            align = true;
        } else if (argument == "--rect") {
            const auto text = value("--rect");
            if (std::sscanf(text.c_str(), "%d,%d,%d,%d",
                            &rect.x, &rect.y, &rect.width, &rect.height) != 4) {
                fail("--rect wants X,Y,W,H");
            }
        } else if (argument == "--survey") {
            survey = true;
        } else if (argument == "--quiet") {
            quiet = true;
        } else if (argument.rfind("--", 0) == 0) {
            fail("unknown option " + argument);
        } else if (a.empty()) {
            a = argument;
        } else if (b.empty()) {
            b = argument;
        } else {
            fail("too many directories");
        }
    }
    if (a.empty() || b.empty()) {
        fail("usage: th2-trace-diff <dir-a> <dir-b> [--align|--offset N] "
             "[--from N] [--to N] [--step N] [--psnr DB] [--max-delta N] "
             "[--rect X,Y,W,H] [--bmp TICK]");
    }

    Reader reader_a(ticks(a), a.string());
    Reader reader_b(ticks(b), b.string());
    const auto& ticks_a = reader_a.ticks();
    const auto& ticks_b = reader_b.ticks();
    if (ticks_a.empty() || ticks_b.empty()) {
        fail("no frames: " + std::to_string(ticks_a.size()) + " in " + a.string()
             + ", " + std::to_string(ticks_b.size()) + " in " + b.string());
    }

    std::string error;
    // Returns a reference into the reader's own cache: the two sides have a
    // reader each, so a frame from one is never invalidated by fetching the
    // other, and the comparison loop copies nothing.
    const auto get = [&](Reader& from, std::uint64_t tick) -> const Frame& {
        const Frame* frame = from.at(tick, error);
        if (!frame) {
            fail(error);
        }
        return *frame;
    };

    if (survey) {
        // Not a comparison at all - a reading of one side, so you can see
        // where a run is on the title screen, where it is a flat black
        // dream scene, and where it finally has a background worth
        // comparing.  Aiming the window is most of the work.
        std::printf("%6s %8s %8s\n", "tick", "mean", "spread");
        double previous = -1.0;
        for (const auto& [tick, path] : ticks_a) {
            if (tick < first || tick > last || (tick - first) % step != 0) {
                continue;
            }
            const auto frame = get(reader_a, tick);
            rect.clamp_to(frame);
            const auto [mean, spread] = mean_and_spread(frame, rect);
            const bool changed = previous >= 0.0
                && std::fabs(spread - previous) > 0.5;
            std::printf("%6llu %8.3f %8.3f%s\n",
                        static_cast<unsigned long long>(tick), mean, spread,
                        changed ? "  <-- change" : "");
            previous = spread;
        }
        return 0;
    }

    if (align) {
        // Probe one frame of A against every shift B can offer.  Which frame
        // matters more than it looks: long stretches of this game are a flat
        // black screen, and a flat frame matches at every offset equally well,
        // so picking by position - the first, or the middle - can report an
        // alignment that means nothing.  Pick the busiest frame instead.
        std::uint64_t probe = ticks_a.begin()->first;
        double busiest = -1.0;
        for (const auto& [tick, path] : ticks_a) {
            if (tick < first || tick > last) {
                continue;
            }
            const Frame* frame = reader_a.at(tick, error);
            if (!frame) {
                fail(error);
            }
            rect.clamp_to(*frame);
            const double spread = mean_and_spread(*frame, rect).second;
            if (spread > busiest) {
                busiest = spread;
                probe = tick;
            }
        }
        const auto reference = get(reader_a, probe);
        double best_psnr = -1.0;
        std::int64_t best = 0;
        for (const auto& [tick, path] : ticks_b) {
            const Frame* candidate = reader_b.at(tick, error);
            if (!candidate || candidate->width != reference.width
                || candidate->height != reference.height) {
                continue;
            }
            const auto stats = compare(reference, *candidate, rect);
            if (stats.psnr > best_psnr) {
                best_psnr = stats.psnr;
                best = static_cast<std::int64_t>(tick)
                     - static_cast<std::int64_t>(probe);
            }
        }
        std::printf("best offset %+lld, probing tick %llu (spread %.1f): "
                    "%.2f dB\n",
                    static_cast<long long>(best),
                    static_cast<unsigned long long>(probe), busiest, best_psnr);
        offset = best;
    }

    std::vector<std::uint64_t> shared;
    for (const auto& [tick, path] : ticks_a) {
        if (tick < first || tick > last) {
            continue;
        }
        const auto shifted = static_cast<std::int64_t>(tick) + offset;
        if (shifted >= 0 && ticks_b.count(static_cast<std::uint64_t>(shifted))) {
            shared.push_back(tick);
        }
    }
    if (shared.empty()) {
        fail("no overlapping ticks (try --align or --offset)");
    }

    if (!quiet) {
        std::printf("%6s %8s %8s %8s %6s\n",
                    "tick", "mad", "psnr", "diff%", "maxd");
    }
    std::vector<std::pair<std::uint64_t, double>> mismatches;
    std::size_t compared = 0;
    int worst_delta = 0;
    std::uint64_t worst_delta_tick = 0;
    int seen_delta = 0;
    double worst = std::numeric_limits<double>::infinity();
    std::uint64_t worst_tick = 0;
    for (std::size_t i = 0; i < shared.size(); i += step) {
        const auto tick = shared[i];
        const Frame& frame_a = get(reader_a, tick);
        const Frame& frame_b = get(
            reader_b, static_cast<std::uint64_t>(
                static_cast<std::int64_t>(tick) + offset));
        if (frame_a.width != frame_b.width || frame_a.height != frame_b.height) {
            fail("tick " + std::to_string(tick) + ": size mismatch");
        }
        rect.clamp_to(frame_a);
        const auto stats = compare(frame_a, frame_b, rect);
        ++compared;
        seen_delta = std::max(seen_delta, stats.max_delta);
        if (stats.psnr < worst) {
            worst = stats.psnr;
            worst_tick = tick;
        }
        // Both gates, independently.
        const bool dim = stats.psnr < threshold;
        const bool spike = stats.max_delta > max_delta_limit;
        const bool bad = dim || spike;
        if (bad) {
            mismatches.emplace_back(tick, stats.psnr);
            if (spike && stats.max_delta > worst_delta) {
                worst_delta = stats.max_delta;
                worst_delta_tick = tick;
            }
        }
        if (!quiet || bad) {
            std::printf("%6llu %8.3f %8.2f %8.3f %6d%s%s\n",
                        static_cast<unsigned long long>(tick),
                        stats.mean_abs, stats.psnr, stats.differing * 100.0,
                        stats.max_delta,
                        bad ? "  <-- " : "",
                        !bad ? "" : (dim && spike) ? "psnr+delta"
                                  : dim ? "psnr" : "delta");
        }
    }

    std::printf("\n");
    if (mismatches.empty()) {
        std::printf("all %zu compared ticks at or above %.1f dB and within "
                    "%d per channel (worst %.2f dB at tick %llu, "
                    "largest channel difference %d)\n",
                    compared, threshold, max_delta_limit, worst,
                    static_cast<unsigned long long>(worst_tick), seen_delta);
    } else {
        std::printf("%zu/%zu ticks failed a gate; first at tick %llu "
                    "(%.2f dB), worst %.2f dB at tick %llu\n",
                    mismatches.size(), compared,
                    static_cast<unsigned long long>(mismatches.front().first),
                    mismatches.front().second, worst,
                    static_cast<unsigned long long>(worst_tick));
        if (worst_delta > max_delta_limit) {
            std::printf("largest channel difference %d (limit %d) "
                        "at tick %llu\n",
                        worst_delta, max_delta_limit,
                        static_cast<unsigned long long>(worst_delta_tick));
        }
    }

    if (bmp_tick) {
        const auto tick = *bmp_tick;
        const auto shifted = static_cast<std::int64_t>(tick) + offset;
        if (ticks_a.count(tick)) {
            const auto path = bmp_dir / ("a" + std::to_string(tick) + ".bmp");
            if (write_bmp(path, get(reader_a, tick))) {
                std::printf("wrote %s\n", path.string().c_str());
            }
        }
        if (shifted >= 0 && ticks_b.count(static_cast<std::uint64_t>(shifted))) {
            const auto path = bmp_dir / ("b" + std::to_string(shifted) + ".bmp");
            if (write_bmp(path, get(reader_b,
                                    static_cast<std::uint64_t>(shifted)))) {
                std::printf("wrote %s\n", path.string().c_str());
            }
        }
    }

    return mismatches.empty() ? 0 : 2;
}
