// The resumable LZS decoder has to agree with the one-shot one exactly, and
// has to agree with itself whatever slice sizes it is stopped at.  A picture
// that decodes differently when a frame happens to run out of budget would be
// a hard bug to ever see, so it is worth pinning down here.
#include "archive.hpp"
#include "image.hpp"

#include <SDL3/SDL.h>
#include <cstring>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <vector>

namespace {

// Compresses with the same scheme archive.cpp decodes: literals, and back
// references into a 4KB ring primed with spaces.  Only literals are emitted
// for bytes that have no match, which is enough to exercise both branches
// once the input repeats.
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input)
{
    constexpr std::size_t ring_size = 4096;
    constexpr std::size_t lookahead = 18;
    std::vector<std::uint8_t> ring(ring_size, ' ');
    std::size_t ring_position = ring_size - lookahead;
    std::vector<std::uint8_t> output;
    std::size_t position = 0;
    while (position < input.size()) {
        std::vector<std::uint8_t> chunk;
        unsigned flags = 0;
        int count = 0;
        while (count < 8 && position < input.size()) {
            // Look for a run of 3..18 bytes already in the ring.
            std::size_t best_length = 0;
            std::size_t best_offset = 0;
            const std::size_t most = std::min<std::size_t>(
                lookahead, input.size() - position);
            if (most >= 3) {
                for (std::size_t offset = 0; offset < ring_size; ++offset) {
                    std::size_t length = 0;
                    while (length < most
                           && ring[(offset + length) & (ring_size - 1)]
                               == input[position + length]) {
                        ++length;
                    }
                    if (length > best_length) {
                        best_length = length;
                        best_offset = offset;
                    }
                }
            }
            if (best_length >= 3) {
                chunk.push_back(static_cast<std::uint8_t>(best_offset & 0xff));
                chunk.push_back(static_cast<std::uint8_t>(
                    ((best_offset >> 4) & 0xf0) | ((best_length - 3) & 0x0f)));
                for (std::size_t i = 0; i < best_length; ++i) {
                    ring[ring_position++ & (ring_size - 1)] =
                        input[position + i];
                }
                position += best_length;
            } else {
                flags |= 1u << count;
                const auto value = input[position++];
                chunk.push_back(value);
                ring[ring_position++ & (ring_size - 1)] = value;
            }
            ++count;
        }
        output.push_back(static_cast<std::uint8_t>(flags));
        output.insert(output.end(), chunk.begin(), chunk.end());
    }
    return output;
}

std::vector<std::uint8_t> sample_input(std::size_t size, unsigned seed)
{
    std::mt19937 random(seed);
    std::vector<std::uint8_t> data;
    data.reserve(size);
    // A mix of runs, repeats and noise, so both branches and the ring wrap
    // all get used.
    while (data.size() < size) {
        const int mode = static_cast<int>(random() % 3);
        if (mode == 0) {
            const auto value = static_cast<std::uint8_t>(random() % 256);
            for (int i = 0, n = 1 + static_cast<int>(random() % 40); i < n; ++i) {
                data.push_back(value);
            }
        } else if (mode == 1 && data.size() > 64) {
            const std::size_t from = random() % (data.size() - 32);
            for (int i = 0, n = 8 + static_cast<int>(random() % 24); i < n; ++i) {
                data.push_back(data[from + static_cast<std::size_t>(i)]);
            }
        } else {
            for (int i = 0, n = 1 + static_cast<int>(random() % 16); i < n; ++i) {
                data.push_back(static_cast<std::uint8_t>(random() % 256));
            }
        }
    }
    data.resize(size);
    return data;
}

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

// A banded TGA decode has to produce exactly what the one-shot decode does,
// whatever budget it is given.  Built here rather than loaded from the game so
// the test needs no data files.
void check_tga_streaming()
{
    for (const int width : {1, 7, 64, 331}) {
        for (const int height : {1, 15, 16, 17, 200}) {
            std::vector<std::uint8_t> tga(18, 0);
            tga[2] = 2;                       // uncompressed true-colour
            tga[12] = static_cast<std::uint8_t>(width & 0xff);
            tga[13] = static_cast<std::uint8_t>((width >> 8) & 0xff);
            tga[14] = static_cast<std::uint8_t>(height & 0xff);
            tga[15] = static_cast<std::uint8_t>((height >> 8) & 0xff);
            tga[16] = 32;
            std::mt19937 random(static_cast<unsigned>(width * 1000 + height));
            for (int i = 0; i < width * height * 4; ++i) {
                tga.push_back(static_cast<std::uint8_t>(random() % 256));
            }

            SDL_Surface* once = th2::load_tga(tga);
            th2::TgaStream stream(tga);
            int slices = 0;
            while (!stream.advance(std::chrono::nanoseconds(1))) {
                if (++slices > 10000) {
                    check(false, "banded tga makes progress");
                    break;
                }
            }
            SDL_Surface* banded = stream.take();
            const bool same = once && banded
                && once->w == banded->w && once->h == banded->h
                && std::memcmp(once->pixels, banded->pixels,
                               static_cast<std::size_t>(once->h) * once->pitch)
                    == 0;
            check(same, "banded tga matches one-shot");
            if (height > 16) {
                check(slices > 0, "a tall picture really is banded");
            }
            SDL_DestroySurface(once);
            SDL_DestroySurface(banded);
        }
    }
}

int main()
{
    for (const std::size_t size : {1u, 2u, 17u, 4095u, 4096u, 4097u,
                                   65536u, 300000u}) {
        const auto original = sample_input(size, 1234u + size);
        const auto packed = compress(original);

        // The property under test is that slicing changes nothing, so the
        // reference is the one-shot decoder rather than the original bytes:
        // whether this test's toy compressor is optimal, or even correct
        // about overlapping matches, is beside the point.
        const auto reference = th2::decompress_lzs(packed, original.size());

        th2::LzsStream whole(packed, original.size());
        check(whole.advance(std::chrono::hours(1)), "one slice completes");
        check(whole.take() == reference, "one slice matches the one-shot");

        // A budget so small that every call has to stop early.
        th2::LzsStream sliced(packed, original.size());
        int slices = 0;
        while (!sliced.advance(std::chrono::nanoseconds(1))) {
            if (++slices > 200000) {
                check(false, "sliced decode makes progress");
                break;
            }
        }
        check(sliced.take() == reference, "many slices match the one-shot");
        if (size > 65536) {
            check(slices > 0, "a large stream really is split");
        }

        // And a middling budget, which is what actually happens in a frame.
        th2::LzsStream chunked(packed, original.size());
        while (!chunked.advance(std::chrono::microseconds(50))) {
        }
        check(chunked.take() == reference, "50us slices match the one-shot");
    }

    // A declared size shorter than the stream must stop exactly there.
    const auto original = sample_input(50000, 99u);
    const auto packed = compress(original);
    const auto reference = th2::decompress_lzs(packed, 50000, 1000);
    th2::LzsStream clipped(packed, 1000);
    while (!clipped.advance(std::chrono::nanoseconds(1))) {
    }
    const auto clipped_output = clipped.take();
    check(clipped_output.size() == 1000, "declared size is honoured");
    check(clipped_output == reference, "clipped output matches the one-shot");

    check_tga_streaming();

    if (failures == 0) {
        std::printf("lzs stream: ok\n");
    }
    return failures == 0 ? 0 : 1;
}
