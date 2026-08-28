#include "xqvm/primitive.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_set>

namespace xqvm {
namespace {

constexpr std::size_t kIslandHeaderSize = 12;

template <typename> inline constexpr bool kAlwaysFalse = false;

void append_u16(std::vector<Byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<Byte>(value));
    bytes.push_back(static_cast<Byte>(value >> 8U));
}

void append_u32(std::vector<Byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<Byte>(value >> shift));
    }
}

void append_u64(std::vector<Byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<Byte>(value >> shift));
    }
}

void patch_u32(std::vector<Byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        bytes[offset + shift / 8U] = static_cast<Byte>(value >> shift);
    }
}

[[nodiscard]] std::size_t node_payload_size(const PrimitiveNode& node) {
    return std::visit(
        [](const auto& typed) -> std::size_t {
            using Node = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Node, LiteralPrimitive>) {
                return 8;
            } else if constexpr (std::is_same_v<Node, ReadCarrierPrimitive> ||
                                 std::is_same_v<Node, AddPrimitive> ||
                                 std::is_same_v<Node, SubPrimitive> ||
                                 std::is_same_v<Node, MulPrimitive> ||
                                 std::is_same_v<Node, XorPrimitive> ||
                                 std::is_same_v<Node, AndPrimitive> ||
                                 std::is_same_v<Node, OrPrimitive> ||
                                 std::is_same_v<Node, ShlPrimitive> ||
                                 std::is_same_v<Node, ShrPrimitive> ||
                                 std::is_same_v<Node, SarPrimitive> ||
                                 std::is_same_v<Node, RolPrimitive> ||
                                 std::is_same_v<Node, RorPrimitive>) {
                return 4;
            } else if constexpr (std::is_same_v<Node, ReadMemory8Primitive> ||
                                 std::is_same_v<Node, ReadMemory64Primitive> ||
                                 std::is_same_v<Node, NotPrimitive> ||
                                 std::is_same_v<Node, NegPrimitive>) {
                return 2;
            } else if constexpr (std::is_same_v<Node, ComparePrimitive>) {
                return 5;
            } else if constexpr (std::is_same_v<Node, SelectPrimitive>) {
                return 6;
            } else if constexpr (std::is_same_v<Node, MixPrimitive>) {
                return 12;
            } else {
                static_assert(kAlwaysFalse<Node>, "primitive payload size is not defined");
            }
        },
        node);
}

[[nodiscard]] Result<void> require_prior(NodeId reference, std::size_t current) {
    if (reference >= current) {
        return make_error(ErrorCode::Verification, current,
                          "primitive DAG reference is not strictly backward");
    }
    return {};
}

[[nodiscard]] Result<void> verify_node(const PrimitiveNode& node, std::size_t current) {
    return std::visit(
        [&](const auto& typed) -> Result<void> {
            using Node = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Node, LiteralPrimitive> ||
                          std::is_same_v<Node, ReadCarrierPrimitive>) {
                return {};
            } else if constexpr (std::is_same_v<Node, ReadMemory8Primitive> ||
                                 std::is_same_v<Node, ReadMemory64Primitive> ||
                                 std::is_same_v<Node, NotPrimitive> ||
                                 std::is_same_v<Node, NegPrimitive>) {
                if constexpr (std::is_same_v<Node, ReadMemory8Primitive> ||
                              std::is_same_v<Node, ReadMemory64Primitive>) {
                    return require_prior(typed.address, current);
                } else {
                    return require_prior(typed.input, current);
                }
            } else if constexpr (std::is_same_v<Node, SelectPrimitive>) {
                if (auto result = require_prior(typed.condition, current); !result)
                    return result;
                if (auto result = require_prior(typed.when_true, current); !result)
                    return result;
                return require_prior(typed.when_false, current);
            } else if constexpr (std::is_same_v<Node, MixPrimitive>) {
                if (auto result = require_prior(typed.left, current); !result)
                    return result;
                return require_prior(typed.right, current);
            } else if constexpr (std::is_same_v<Node, ShlPrimitive> ||
                                 std::is_same_v<Node, ShrPrimitive> ||
                                 std::is_same_v<Node, SarPrimitive> ||
                                 std::is_same_v<Node, RolPrimitive> ||
                                 std::is_same_v<Node, RorPrimitive>) {
                if (auto result = require_prior(typed.value, current); !result)
                    return result;
                return require_prior(typed.amount, current);
            } else {
                if (auto result = require_prior(typed.left, current); !result)
                    return result;
                return require_prior(typed.right, current);
            }
        },
        node);
}

void append_node(std::vector<Byte>& bytes, const PrimitiveNode& node,
                 const EncodingProfile& profile) {
    bytes.push_back(profile.primitives().encode(primitive_kind(node)));
    std::visit(
        [&](const auto& typed) {
            using Node = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Node, LiteralPrimitive>) {
                append_u64(bytes, typed.value);
            } else if constexpr (std::is_same_v<Node, ReadCarrierPrimitive>) {
                append_u32(bytes, typed.carrier);
            } else if constexpr (std::is_same_v<Node, ReadMemory8Primitive> ||
                                 std::is_same_v<Node, ReadMemory64Primitive>) {
                append_u16(bytes, typed.address);
            } else if constexpr (std::is_same_v<Node, NotPrimitive> ||
                                 std::is_same_v<Node, NegPrimitive>) {
                append_u16(bytes, typed.input);
            } else if constexpr (std::is_same_v<Node, ComparePrimitive>) {
                bytes.push_back(static_cast<Byte>(typed.condition));
                append_u16(bytes, typed.left);
                append_u16(bytes, typed.right);
            } else if constexpr (std::is_same_v<Node, SelectPrimitive>) {
                append_u16(bytes, typed.condition);
                append_u16(bytes, typed.when_true);
                append_u16(bytes, typed.when_false);
            } else if constexpr (std::is_same_v<Node, MixPrimitive>) {
                append_u16(bytes, typed.left);
                append_u16(bytes, typed.right);
                append_u64(bytes, typed.salt);
            } else if constexpr (std::is_same_v<Node, ShlPrimitive> ||
                                 std::is_same_v<Node, ShrPrimitive> ||
                                 std::is_same_v<Node, SarPrimitive> ||
                                 std::is_same_v<Node, RolPrimitive> ||
                                 std::is_same_v<Node, RorPrimitive>) {
                append_u16(bytes, typed.value);
                append_u16(bytes, typed.amount);
            } else {
                append_u16(bytes, typed.left);
                append_u16(bytes, typed.right);
            }
        },
        node);
}

} // namespace

PrimitiveKind primitive_kind(const PrimitiveNode& node) {
    return static_cast<PrimitiveKind>(node.index());
}

std::size_t primitive_plan_size(const PrimitivePlan& plan) {
    std::size_t size = kIslandHeaderSize;
    for (const PrimitiveNode& node : plan.nodes) {
        size += 1U + node_payload_size(node);
    }
    size += plan.carrier_writes.size() * 6U;
    size += plan.memory_writes.size() * 5U;
    return size;
}

Result<void> verify_primitive_plan(const PrimitivePlan& plan, std::uint32_t carrier_count,
                                   const PrimitivePlanLimits& limits) {
    if (plan.nodes.empty()) {
        return make_error(ErrorCode::Verification, 0, "physical island has no primitive nodes");
    }
    if (plan.nodes.size() > limits.max_nodes ||
        plan.nodes.size() > std::numeric_limits<NodeId>::max()) {
        return make_error(ErrorCode::Verification, 0, "physical island has too many nodes");
    }
    if (plan.carrier_writes.size() > limits.max_carrier_writes ||
        plan.carrier_writes.size() > std::numeric_limits<std::uint16_t>::max()) {
        return make_error(ErrorCode::Verification, 0,
                          "physical island has too many carrier writes");
    }
    if (plan.memory_writes.size() > limits.max_memory_writes ||
        plan.memory_writes.size() > std::numeric_limits<std::uint16_t>::max()) {
        return make_error(ErrorCode::Verification, 0, "physical island has too many memory writes");
    }
    if (primitive_plan_size(plan) > std::numeric_limits<std::uint32_t>::max()) {
        return make_error(ErrorCode::Verification, 0, "physical island exceeds format size");
    }

    for (std::size_t index = 0; index < plan.nodes.size(); ++index) {
        if (auto result = verify_node(plan.nodes[index], index); !result) {
            return result;
        }
        if (const auto* compare = std::get_if<ComparePrimitive>(&plan.nodes[index]);
            compare != nullptr && static_cast<Byte>(compare->condition) >
                                      static_cast<Byte>(CompareCondition::SignedLessEqual)) {
            return make_error(ErrorCode::Verification, index,
                              "comparison uses an invalid condition");
        }
        if (const auto* read = std::get_if<ReadCarrierPrimitive>(&plan.nodes[index]);
            read != nullptr && read->carrier >= carrier_count) {
            return make_error(ErrorCode::Verification, index, "read references a missing carrier");
        }
    }

    std::unordered_set<CarrierId> written_carriers;
    for (const DelayedCarrierWrite& write : plan.carrier_writes) {
        if (write.carrier >= carrier_count) {
            return make_error(ErrorCode::Verification, write.carrier,
                              "write references a missing carrier");
        }
        if (write.value >= plan.nodes.size()) {
            return make_error(ErrorCode::Verification, write.value, "write uses a missing node");
        }
        if (!written_carriers.emplace(write.carrier).second) {
            return make_error(ErrorCode::Verification, write.carrier,
                              "physical island writes one carrier twice");
        }
    }
    for (const DelayedMemoryWrite& write : plan.memory_writes) {
        if (write.width != 1 && write.width != 8) {
            return make_error(ErrorCode::Verification, write.width, "invalid delayed write width");
        }
        if (write.address >= plan.nodes.size() || write.value >= plan.nodes.size()) {
            return make_error(ErrorCode::Verification, 0,
                              "delayed memory write uses a missing node");
        }
    }
    return {};
}

Result<std::vector<Byte>> encode_primitive_plan(const PrimitivePlan& plan,
                                                const EncodingProfile& profile,
                                                std::size_t absolute_offset) {
    if (plan.nodes.size() > std::numeric_limits<std::uint16_t>::max() ||
        plan.carrier_writes.size() > std::numeric_limits<std::uint16_t>::max() ||
        plan.memory_writes.size() > std::numeric_limits<std::uint16_t>::max() ||
        primitive_plan_size(plan) > std::numeric_limits<std::uint32_t>::max()) {
        return make_error(ErrorCode::Assembly, absolute_offset, "physical island exceeds format");
    }

    std::vector<Byte> bytes;
    bytes.reserve(primitive_plan_size(plan));
    append_u32(bytes, 0);
    append_u16(bytes, static_cast<std::uint16_t>(plan.nodes.size()));
    append_u16(bytes, static_cast<std::uint16_t>(plan.carrier_writes.size()));
    append_u16(bytes, static_cast<std::uint16_t>(plan.memory_writes.size()));
    append_u16(bytes, 0);
    for (const PrimitiveNode& node : plan.nodes) {
        append_node(bytes, node, profile);
    }
    for (const DelayedCarrierWrite& write : plan.carrier_writes) {
        append_u32(bytes, write.carrier);
        append_u16(bytes, write.value);
    }
    for (const DelayedMemoryWrite& write : plan.memory_writes) {
        bytes.push_back(write.width);
        append_u16(bytes, write.address);
        append_u16(bytes, write.value);
    }
    patch_u32(bytes, 0, static_cast<std::uint32_t>(bytes.size()));

    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = profile.mask_code_byte(bytes[index], absolute_offset + index);
    }
    return bytes;
}

} // namespace xqvm
