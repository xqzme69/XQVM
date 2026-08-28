#ifndef XQVM_SCHEMA_HPP
#define XQVM_SCHEMA_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace xqvm {

using Byte = std::uint8_t;
using Word = std::uint64_t;
using NodeId = std::uint16_t;
using CarrierId = std::uint32_t;
using OriginId = std::uint32_t;

enum class PrimitiveKind : Byte {
    Literal,
    ReadCarrier,
    ReadMemory8,
    ReadMemory64,
    Add,
    Sub,
    Mul,
    Xor,
    And,
    Or,
    Not,
    Neg,
    Shl,
    Shr,
    Sar,
    Rol,
    Ror,
    Compare,
    Select,
    Mix,
    Count,
};

constexpr std::size_t kPrimitiveSchemaCount = static_cast<std::size_t>(PrimitiveKind::Count);
static_assert(kPrimitiveSchemaCount == 20, "the physical ISA has exactly 20 schemas");

constexpr std::array<std::string_view, kPrimitiveSchemaCount> kPrimitiveNames{
    "literal", "read.carrier", "read.memory8", "read.memory64", "add",    "sub", "mul",
    "xor",     "and",          "or",           "not",           "neg",    "shl", "shr",
    "sar",     "rol",          "ror",          "compare",       "select", "mix",
};

[[nodiscard]] constexpr std::string_view primitive_name(PrimitiveKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kPrimitiveNames.size() ? kPrimitiveNames[index] : "invalid";
}

enum class CompareCondition : Byte {
    Equal,
    NotEqual,
    UnsignedLess,
    UnsignedLessEqual,
    SignedLess,
    SignedLessEqual,
};

} // namespace xqvm

#endif // XQVM_SCHEMA_HPP
