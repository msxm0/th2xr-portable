#pragma once

#include <cstdint>

namespace th2 {

// The engine's one rand(): msvcrt's, never seeded, so it runs from seed 1.
//     holdrand = holdrand * 214013 + 2531011;  return (holdrand >> 16) & 0x7fff;
// Everything the engine draws at random shares it - a shake's direction, the
// script's RAND, weather - so a draw only agrees with the reference when it
// is the same draw of the same sequence.  Three tables are built from it at
// start-up before anything else asks (measured with the reference's rand
// wrapped, TH2REF_RAND_LOG):
//     BMP_CreateRandMeshTable    64*64*64 = 262144
//     BMP_CreatePixelBombTable        256
//     MM_std's RandTbl            0x10000 =  65536
// and the port builds none of them, so the sequence starts past them.
inline constexpr std::uint32_t engine_rand_startup_calls = 262144 + 256 + 65536;

constexpr std::uint32_t engine_rand_advance(std::uint32_t state,
                                            std::uint32_t calls)
{
    for (std::uint32_t i = 0; i < calls; ++i) {
        state = state * 214013u + 2531011u;
    }
    return state;
}

inline std::uint32_t engine_rand_state =
    engine_rand_advance(1u, engine_rand_startup_calls);

// Calls since start-up, for comparing call order against TH2REF_RAND_LOG.
inline std::uint64_t engine_rand_calls = 0;

inline int engine_rand()
{
    ++engine_rand_calls;
    engine_rand_state = engine_rand_state * 214013u + 2531011u;
    return static_cast<int>((engine_rand_state >> 16) & 0x7fff);
}

}  // namespace th2
