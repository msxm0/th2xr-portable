#include "archive.hpp"
#include "bytecode.hpp"
#include "event.hpp"
#include "scenario.hpp"

#include <algorithm>
#include <cctype>
#include <span>
#include <array>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

bool is_scenario(std::string_view name)
{
    return name.size() >= 4
        && (name.substr(name.size() - 4) == ".SDT"
            || name.substr(name.size() - 4) == ".sdt");
}

}  // namespace

void print_arguments(const th2::Instruction& instruction,
                     std::span<const std::uint8_t> bytecode)
{
    static const std::array<std::int32_t, 50> registers{};
    try {
        const auto event = th2::decode_event(
            instruction, bytecode.subspan(instruction.offset, instruction.size),
            registers);
        for (const auto& argument : event.arguments) {
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::int32_t>) {
                    std::cout << ' ' << value;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    std::cout << ' ' << std::quoted(value);
                } else if constexpr (std::is_same_v<T, th2::RegisterTarget>) {
                    std::cout << " reg[" << +value.index << ']';
                } else {
                    std::cout << " cmp[" << +value.register_index << ','
                              << +value.operation << ',' << value.value << ']';
                }
            }, argument);
        }
    } catch (const std::exception& error) {
        std::cout << " <dynamic: " << error.what() << '>';
    }
}

std::uint32_t u32_at(std::span<const std::uint8_t> code, std::size_t at)
{
    return static_cast<std::uint32_t>(code[at]) | code[at + 1] << 8
        | code[at + 2] << 16 | static_cast<std::uint32_t>(code[at + 3]) << 24;
}

// The core opcodes' operands, as Vm::execute reads them.
void print_core(const th2::Instruction& instruction,
                std::span<const std::uint8_t> code)
{
    static constexpr std::array<const char*, 6> ops{
        "<", "<=", ">", ">=", "==", "!="};
    const auto pc = instruction.offset;
    const auto op = instruction.opcode;
    const auto value = [&](std::size_t at, bool immediate) {
        return immediate ? std::to_string(static_cast<std::int32_t>(u32_at(code, at)))
                         : "r" + std::to_string(code[at]);
    };
    const auto cmp = [&](std::uint8_t c) {
        return c < ops.size() ? ops[c] : "?";
    };
    switch (op) {
    case 2: case 3:     // MovR / MovV
        std::cout << " r" << +code[pc + 2] << " = " << value(pc + 3, op == 3);
        break;
    case 4:
        std::cout << " r" << +code[pc + 2] << " <-> r" << +code[pc + 3];
        break;
    case 5:
        std::cout << " r" << +code[pc + 2] << " = rand";
        break;
    case 6: case 7: {   // IfR / IfV: jump when true
        const bool imm = op == 7;
        std::cout << " if r" << +code[pc + 2] << ' ' << cmp(code[pc + 3]) << ' '
                  << value(pc + 4, imm) << " goto "
                  << u32_at(code, pc + (imm ? 8 : 5));
        break;
    }
    case 8: case 9: {
        const bool imm = op == 9;
        const auto t = pc + (imm ? 8 : 5);
        std::cout << " if r" << +code[pc + 2] << ' ' << cmp(code[pc + 3]) << ' '
                  << value(pc + 4, imm) << " goto " << u32_at(code, t)
                  << " else " << u32_at(code, t + 4);
        break;
    }
    case 10:
        std::cout << " while r" << +code[pc + 2] << "-- > 0 goto "
                  << u32_at(code, pc + 3);
        break;
    case 11:
        std::cout << " goto " << u32_at(code, pc + 2);
        break;
    case 12: case 13: case 14: case 15:
        std::cout << " r" << +code[pc + 2];
        break;
    default:
        if (op >= 16 && op <= 31) {
            std::cout << " r" << +code[pc + 2] << ", " << value(pc + 3, op % 2);
        }
        break;
    }
}

int dump(const th2::Archive& archive, std::string want)
{
    for (auto& c : want) c = static_cast<char>(std::toupper(c));
    if (want.find('.') == std::string::npos) want += ".SDT";
    for (const auto& entry : archive.entries()) {
        std::string name = entry.name;
        for (auto& c : name) c = static_cast<char>(std::toupper(c));
        if (name != want) continue;
        const th2::Scenario scenario(archive.read(entry));
        const auto code = scenario.bytecode();
        std::size_t offset = 0;
        while (offset < code.size()) {
            const auto instruction = th2::decode_instruction(code, offset);
            std::cout << std::setw(6) << instruction.offset << ' '
                      << instruction.name;
            if (instruction.opcode >= 64) {
                print_arguments(instruction, code);
            } else {
                print_core(instruction, code);
            }
            std::cout << '\n';
            offset += instruction.size;
        }
        return 0;
    }
    std::cerr << "no scenario " << want << '\n';
    return 1;
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[2]) == "--dump") {
        try {
            return dump(th2::Archive(argv[1]), argv[3]);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: th2-bytecode-info SDT.PAK [OPCODE]\n"
                     "       th2-bytecode-info SDT.PAK --dump SCRIPT\n";
        return 2;
    }

    try {
        const th2::Archive archive(argv[1]);
        const std::string_view requested = argc == 3 ? argv[2] : "";
        std::map<std::string, std::size_t> counts;
        std::size_t instructions = 0;
        const std::array<std::int32_t, 50> registers{};

        for (const auto& entry : archive.entries()) {
            if (!is_scenario(entry.name)) {
                continue;
            }
            const th2::Scenario scenario(archive.read(entry));
            std::size_t offset = 0;
            try {
                while (offset < scenario.bytecode().size()) {
                    const auto instruction = th2::decode_instruction(
                        scenario.bytecode(), offset);
                    ++counts[std::string(instruction.name)];
                    ++instructions;
                    if (!requested.empty() && instruction.name == requested) {
                        std::cout << entry.name << ':' << instruction.offset;
                        if (instruction.opcode >= 64) {
                            try {
                                const auto event = th2::decode_event(
                                    instruction,
                                    scenario.bytecode().subspan(
                                        instruction.offset, instruction.size),
                                    registers);
                                for (const auto& argument : event.arguments) {
                                    std::visit([&](const auto& value) {
                                        using T = std::decay_t<decltype(value)>;
                                        if constexpr (
                                            std::is_same_v<T, std::int32_t>) {
                                            std::cout << ' ' << value;
                                        } else if constexpr (
                                            std::is_same_v<T, std::string>) {
                                            std::cout << ' '
                                                      << std::quoted(value);
                                        } else if constexpr (
                                            std::is_same_v<
                                                T, th2::RegisterTarget>) {
                                            std::cout << " reg["
                                                      << +value.index << ']';
                                        } else {
                                            std::cout << " cmp["
                                                      << +value.register_index
                                                      << ',' << +value.operation
                                                      << ',' << value.value
                                                      << ']';
                                        }
                                    }, argument);
                                }
                            } catch (const std::exception& error) {
                                std::cout << " <dynamic: " << error.what()
                                          << '>';
                            }
                        }
                        std::cout << '\n';
                    }
                    offset += instruction.size;
                }
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    entry.name + " at byte " + std::to_string(offset)
                    + ": " + error.what());
            }
        }

        if (!requested.empty()) {
            return 0;
        }

        std::vector<std::pair<std::string, std::size_t>> sorted(
            counts.begin(), counts.end());
        std::ranges::sort(sorted, {}, &std::pair<std::string, std::size_t>::second);
        std::cout << instructions << " instructions, " << counts.size()
                  << " opcodes\n";
        for (std::size_t index = 0; index < sorted.size(); ++index) {
            const auto& [name, count] = sorted[sorted.size() - index - 1];
            std::cout << "  " << name << ": " << count << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
