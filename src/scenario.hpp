#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace th2 {

class Scenario {
public:
    static constexpr std::size_t block_count = 256;
    // Signature, declared size and the block table, ahead of the bytecode.
    static constexpr std::size_t header_size =
        2 * sizeof(std::uint16_t) + sizeof(std::uint32_t)
        + block_count * sizeof(std::uint32_t);

    explicit Scenario(std::span<const std::uint8_t> bytes);

    const std::array<std::uint32_t, block_count>& blocks() const { return blocks_; }
    std::span<const std::uint8_t> bytecode() const { return bytecode_; }
    std::span<const std::uint8_t> block(std::size_t index) const;

private:
    std::array<std::uint32_t, block_count> blocks_{};
    std::vector<std::uint8_t> bytecode_;
};

}  // namespace th2
