#ifndef XQVM_PRIMITIVE_HPP
#define XQVM_PRIMITIVE_HPP

#include "xqvm/profile.hpp"
#include "xqvm/result.hpp"
#include "xqvm/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace xqvm {

struct LiteralPrimitive {
    Word value{};
};
struct ReadCarrierPrimitive {
    CarrierId carrier{};
};
struct ReadMemory8Primitive {
    NodeId address{};
};
struct ReadMemory64Primitive {
    NodeId address{};
};
struct AddPrimitive {
    NodeId left{};
    NodeId right{};
};
struct SubPrimitive {
    NodeId left{};
    NodeId right{};
};
struct MulPrimitive {
    NodeId left{};
    NodeId right{};
};
struct XorPrimitive {
    NodeId left{};
    NodeId right{};
};
struct AndPrimitive {
    NodeId left{};
    NodeId right{};
};
struct OrPrimitive {
    NodeId left{};
    NodeId right{};
};
struct NotPrimitive {
    NodeId input{};
};
struct NegPrimitive {
    NodeId input{};
};
struct ShlPrimitive {
    NodeId value{};
    NodeId amount{};
};
struct ShrPrimitive {
    NodeId value{};
    NodeId amount{};
};
struct SarPrimitive {
    NodeId value{};
    NodeId amount{};
};
struct RolPrimitive {
    NodeId value{};
    NodeId amount{};
};
struct RorPrimitive {
    NodeId value{};
    NodeId amount{};
};
struct ComparePrimitive {
    CompareCondition condition{CompareCondition::Equal};
    NodeId left{};
    NodeId right{};
};
struct SelectPrimitive {
    NodeId condition{};
    NodeId when_true{};
    NodeId when_false{};
};
struct MixPrimitive {
    NodeId left{};
    NodeId right{};
    Word salt{};
};

using PrimitiveNode =
    std::variant<LiteralPrimitive, ReadCarrierPrimitive, ReadMemory8Primitive,
                 ReadMemory64Primitive, AddPrimitive, SubPrimitive, MulPrimitive, XorPrimitive,
                 AndPrimitive, OrPrimitive, NotPrimitive, NegPrimitive, ShlPrimitive, ShrPrimitive,
                 SarPrimitive, RolPrimitive, RorPrimitive, ComparePrimitive, SelectPrimitive,
                 MixPrimitive>;

static_assert(std::variant_size_v<PrimitiveNode> == kPrimitiveSchemaCount,
              "each physical schema has one compiler-side node type");

struct DelayedCarrierWrite {
    CarrierId carrier{};
    NodeId value{};
};

struct DelayedMemoryWrite {
    Byte width{8};
    NodeId address{};
    NodeId value{};
};

struct PrimitivePlan {
    std::vector<PrimitiveNode> nodes;
    std::vector<DelayedCarrierWrite> carrier_writes;
    std::vector<DelayedMemoryWrite> memory_writes;
};

struct PhysicalIsland {
    OriginId origin{};
    std::string label;
    PrimitivePlan plan;
};

struct PrimitivePlanLimits {
    std::size_t max_nodes{4096};
    std::size_t max_carrier_writes{1024};
    std::size_t max_memory_writes{1024};
};

[[nodiscard]] PrimitiveKind primitive_kind(const PrimitiveNode& node);
[[nodiscard]] std::size_t primitive_plan_size(const PrimitivePlan& plan);
[[nodiscard]] Result<void> verify_primitive_plan(const PrimitivePlan& plan,
                                                 std::uint32_t carrier_count,
                                                 const PrimitivePlanLimits& limits = {});
[[nodiscard]] Result<std::vector<Byte>> encode_primitive_plan(const PrimitivePlan& plan,
                                                              const EncodingProfile& profile,
                                                              std::size_t absolute_offset);

} // namespace xqvm

#endif // XQVM_PRIMITIVE_HPP
