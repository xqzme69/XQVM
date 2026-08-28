#include "xqvm/compiler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace xqvm {
namespace {

struct IslandState {
    PhysicalValue semantic;
    PhysicalValue history;
};

struct EdgeBuild {
    BlockId source{};
    BlockEdge edge;
    std::size_t island{};
    OriginId origin{};
    std::string label;
};

enum class RelocationPart : Byte {
    Lane,
    Proof,
};

struct ContinuationRelocation {
    std::size_t island{};
    NodeId node{};
    std::optional<std::size_t> target_island;
    RelocationPart part{RelocationPart::Lane};
};

class PlanEmitter {
  public:
    explicit PlanEmitter(PrimitivePlan& plan) : plan_(plan) {}

    template <typename Node> [[nodiscard]] NodeId add(Node node) {
        if (plan_.nodes.size() >= std::numeric_limits<NodeId>::max()) {
            overflowed_ = true;
            return 0;
        }
        const NodeId id = static_cast<NodeId>(plan_.nodes.size());
        plan_.nodes.emplace_back(std::move(node));
        return id;
    }

    void write_carrier(CarrierId carrier, NodeId value) {
        plan_.carrier_writes.push_back(DelayedCarrierWrite{carrier, value});
    }

    void write_memory(Byte width, NodeId address, NodeId value) {
        plan_.memory_writes.push_back(DelayedMemoryWrite{width, address, value});
    }

    [[nodiscard]] bool overflowed() const noexcept {
        return overflowed_;
    }

  private:
    PrimitivePlan& plan_;
    bool overflowed_{};
};

class CarrierAllocator {
  public:
    // The seed is mandatory; the resource ceiling is an optional test/compiler limit.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    explicit CarrierAllocator(Word seed, CarrierId max_carriers = ModuleLimits{}.max_carriers)
        : seed_(seed), max_carriers_(max_carriers) {}

    [[nodiscard]] Result<PhysicalValue> allocate(ValueDomain domain, OriginId origin) {
        constexpr CarrierId kPhysicalShares = 3;
        if (max_carriers_ < kPhysicalShares || next_ > max_carriers_ - kPhysicalShares) {
            return make_error(ErrorCode::Assembly, next_, "carrier id space is exhausted");
        }
        const Word family_key = origin_salt(origin, 0);
        PhysicalValue value;
        value.domain = domain;
        value.family = (family_key & 1U) == 0 ? ShareFamily::Xor : ShareFamily::Additive;
        value.origin = origin;
        for (std::size_t index = 0; index < value.shares.size(); ++index) {
            const Word multiplier = origin_salt(origin, 1U + index * 2U) | 1U;
            const Word addend = origin_salt(origin, 2U + index * 2U);
            value.shares[index] = PhysicalShare{next_++, AffineMap{multiplier, addend}};
        }
        allocated_.push_back(value);
        return value;
    }

    [[nodiscard]] CarrierId count() const noexcept {
        return next_;
    }
    [[nodiscard]] const std::vector<PhysicalValue>& allocated() const noexcept {
        return allocated_;
    }

  private:
    [[nodiscard]] Word origin_salt(OriginId origin, std::size_t slot) const noexcept {
        return mix_word(seed_ ^ static_cast<Word>(origin) * 0xD6E8FEB86659FD93ULL,
                        static_cast<Word>(slot) * 0xA0761D6478BD642FULL, 0xE7037ED1A0B428DBULL);
    }

    Word seed_{};
    CarrierId next_{kFirstAllocatedCarrier};
    CarrierId max_carriers_{};
    std::vector<PhysicalValue> allocated_;
};

[[nodiscard]] Word compiler_salt(Word seed, OriginId origin, Word slot) noexcept {
    return mix_word(seed ^ static_cast<Word>(origin) * 0xD6E8FEB86659FD93ULL,
                    slot * 0xA0761D6478BD642FULL, 0x8EBC6AF09C88C6E3ULL);
}

[[nodiscard]] PhysicalValue fixed_value(ValueDomain domain, CarrierId first) {
    PhysicalValue value;
    value.domain = domain;
    value.family = ShareFamily::Xor;
    for (std::size_t index = 0; index < value.shares.size(); ++index) {
        value.shares[index] = PhysicalShare{
            static_cast<CarrierId>(first + static_cast<CarrierId>(index)),
            AffineMap{},
        };
    }
    return value;
}

[[nodiscard]] Result<NodeId> emit_integer_binary(PlanEmitter& emitter,
                                                 IntegerBinaryOperator operation, NodeId left,
                                                 NodeId right) {
    switch (operation) {
    case IntegerBinaryOperator::Add:
        return emitter.add(AddPrimitive{left, right});
    case IntegerBinaryOperator::Subtract:
        return emitter.add(SubPrimitive{left, right});
    case IntegerBinaryOperator::Multiply:
        return emitter.add(MulPrimitive{left, right});
    case IntegerBinaryOperator::Xor:
        return emitter.add(XorPrimitive{left, right});
    case IntegerBinaryOperator::And:
        return emitter.add(AndPrimitive{left, right});
    case IntegerBinaryOperator::Or:
        return emitter.add(OrPrimitive{left, right});
    case IntegerBinaryOperator::ShiftLeft:
        return emitter.add(ShlPrimitive{left, right});
    case IntegerBinaryOperator::ShiftRightLogical:
        return emitter.add(ShrPrimitive{left, right});
    case IntegerBinaryOperator::ShiftRightArithmetic:
        return emitter.add(SarPrimitive{left, right});
    case IntegerBinaryOperator::RotateLeft:
        return emitter.add(RolPrimitive{left, right});
    case IntegerBinaryOperator::RotateRight:
        return emitter.add(RorPrimitive{left, right});
    }
    return make_error(ErrorCode::Assembly, 0,
                      "invalid integer binary operator after IR verification");
}

[[nodiscard]] NodeId reconstruct(PlanEmitter& emitter, const PhysicalValue& value) {
    std::array<NodeId, 3> shares{};
    for (std::size_t index = 0; index < value.shares.size(); ++index) {
        const PhysicalShare& share = value.shares[index];
        const AffineMap inverse = share.storage.inverse();
        const NodeId raw = emitter.add(ReadCarrierPrimitive{share.carrier});
        const NodeId multiplier = emitter.add(LiteralPrimitive{inverse.multiplier});
        const NodeId product = emitter.add(MulPrimitive{raw, multiplier});
        const NodeId addend = emitter.add(LiteralPrimitive{inverse.addend});
        shares[index] = emitter.add(AddPrimitive{product, addend});
    }
    if (value.family == ShareFamily::Xor) {
        const NodeId pair = emitter.add(XorPrimitive{shares[0], shares[1]});
        return emitter.add(XorPrimitive{pair, shares[2]});
    }
    const NodeId pair = emitter.add(AddPrimitive{shares[0], shares[1]});
    return emitter.add(AddPrimitive{pair, shares[2]});
}

// Logical, semantic and history are separate DAG nodes with a fixed mixing order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void stage_physical_write(PlanEmitter& emitter, const PhysicalValue& value, NodeId logical,
                          NodeId semantic, NodeId history, Word salt) {
    const NodeId first = emitter.add(MixPrimitive{semantic, history, salt});
    const NodeId second =
        emitter.add(MixPrimitive{history, semantic, salt ^ 0x9E3779B97F4A7C15ULL});
    NodeId third{};
    if (value.family == ShareFamily::Xor) {
        third = emitter.add(XorPrimitive{
            emitter.add(XorPrimitive{logical, first}),
            second,
        });
    } else {
        third = emitter.add(SubPrimitive{
            emitter.add(SubPrimitive{logical, first}),
            second,
        });
    }
    const std::array<NodeId, 3> shares{first, second, third};
    for (std::size_t index = 0; index < value.shares.size(); ++index) {
        const PhysicalShare& target = value.shares[index];
        const NodeId multiplier = emitter.add(LiteralPrimitive{target.storage.multiplier});
        const NodeId product = emitter.add(MulPrimitive{shares[index], multiplier});
        const NodeId addend = emitter.add(LiteralPrimitive{target.storage.addend});
        const NodeId stored = emitter.add(AddPrimitive{product, addend});
        emitter.write_carrier(target.carrier, stored);
    }
}

[[nodiscard]] NodeId add_relocation(PlanEmitter& emitter,
                                    std::vector<ContinuationRelocation>& relocations,
                                    std::size_t island, std::optional<std::size_t> target,
                                    RelocationPart part) {
    const NodeId node = emitter.add(LiteralPrimitive{});
    relocations.push_back(ContinuationRelocation{island, node, target, part});
    return node;
}

void stage_continuation(PlanEmitter& emitter, const PhysicalValue& lane_value,
                        const PhysicalValue& proof_value, NodeId lane, NodeId proof,
                        NodeId semantic, NodeId history, Word salt) {
    stage_physical_write(emitter, lane_value, lane, semantic, history,
                         salt ^ 0x4C414E4553484152ULL);
    stage_physical_write(emitter, proof_value, proof, history, semantic,
                         salt ^ 0x50524F4F46534852ULL);
}

[[nodiscard]] std::string escape_json(const std::string& text) {
    std::ostringstream escaped;
    escaped.imbue(std::locale::classic());
    for (char raw_character : text) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"':
            escaped << "\\\"";
            break;
        case '\\':
            escaped << "\\\\";
            break;
        case '\b':
            escaped << "\\b";
            break;
        case '\f':
            escaped << "\\f";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            if (character < 0x20U) {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned>(character) << std::dec;
            } else {
                escaped << static_cast<char>(character);
            }
        }
    }
    return escaped.str();
}

[[nodiscard]] SidecarPhysicalValue sidecar_value(std::string role, const PhysicalValue& value) {
    return SidecarPhysicalValue{
        std::move(role), value.origin, value.domain, value.family, value.shares,
    };
}

} // namespace

Result<CompiledProgram> compile_machine_ir(const MachineFunction& function,
                                           const EncodingProfile& profile,
                                           const CompileOptions& options) {
    auto ir_report = verify_machine_ir(function);
    if (!ir_report)
        return ir_report.error();
    if (function.argument_domains.size() > kMaximumArguments) {
        return make_error(ErrorCode::Assembly, function.argument_domains.size(),
                          "function exceeds the physical argument carrier window");
    }
    if (options.initial_data.size() > options.memory_size) {
        return make_error(ErrorCode::Assembly, options.initial_data.size(),
                          "initial data exceeds configured memory");
    }

    OriginId maximum_origin = 0;
    for (const MachineBlock& block : function.blocks) {
        maximum_origin = std::max(maximum_origin, block.origin);
        for (const IrValue& parameter : block.parameters) {
            maximum_origin = std::max(maximum_origin, parameter.origin);
        }
        for (const TypedAction& action : block.actions) {
            maximum_origin = std::max(maximum_origin, action_origin(action));
        }
    }
    if (maximum_origin == std::numeric_limits<OriginId>::max()) {
        return make_error(ErrorCode::Assembly, maximum_origin, "origin id space is exhausted");
    }
    OriginId next_origin = maximum_origin + 1U;
    auto synthetic_origin = [&]() -> Result<OriginId> {
        if (next_origin == 0) {
            return make_error(ErrorCode::Assembly, 0, "origin id space is exhausted");
        }
        const OriginId result = next_origin;
        ++next_origin;
        return result;
    };

    CarrierAllocator carriers(profile.seed());
    std::unordered_map<ValueId, PhysicalValue> values;
    std::unordered_map<BlockId, std::size_t> block_island;
    std::unordered_map<BlockId, const MachineBlock*> blocks;
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        block_island.emplace(function.blocks[index].id, index);
        blocks.emplace(function.blocks[index].id, &function.blocks[index]);
    }
    for (const MachineBlock& block : function.blocks) {
        for (const IrValue& parameter : block.parameters) {
            auto physical = carriers.allocate(parameter.domain, parameter.origin);
            if (!physical)
                return physical.error();
            values.emplace(parameter.id, physical.value());
        }
        for (const TypedAction& action : block.actions) {
            if (const IrValue* output = produced_value(action)) {
                auto physical = carriers.allocate(output->domain, output->origin);
                if (!physical)
                    return physical.error();
                values.emplace(output->id, physical.value());
            }
        }
    }

    std::vector<EdgeBuild> edges;
    std::unordered_map<BlockId, std::vector<std::size_t>> outgoing;
    for (const MachineBlock& block : function.blocks) {
        auto add_edge = [&](const BlockEdge& edge, const std::string& suffix) -> Result<void> {
            auto origin = synthetic_origin();
            if (!origin)
                return origin.error();
            const std::size_t index = edges.size();
            const std::size_t island = function.blocks.size() + index;
            edges.push_back(EdgeBuild{
                block.id,
                edge,
                island,
                origin.value(),
                block.label + "::" + suffix + "->" + blocks.at(edge.target)->label,
            });
            outgoing[block.id].push_back(index);
            return {};
        };

        auto result = std::visit(
            [&](const auto& terminator) -> Result<void> {
                using Terminator = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<Terminator, JumpTerminator>) {
                    return add_edge(terminator.edge, "jump");
                } else if constexpr (std::is_same_v<Terminator, BranchTerminator>) {
                    if (auto added = add_edge(terminator.when_true, "true"); !added)
                        return added;
                    return add_edge(terminator.when_false, "false");
                } else {
                    return {};
                }
            },
            block.terminator);
        if (!result)
            return result.error();
    }

    const std::size_t island_count = function.blocks.size() + edges.size();
    std::vector<PhysicalIsland> islands(island_count);
    std::vector<IslandState> states;
    states.reserve(function.blocks.size());
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        auto semantic_origin = synthetic_origin();
        auto history_origin = synthetic_origin();
        if (!semantic_origin)
            return semantic_origin.error();
        if (!history_origin)
            return history_origin.error();
        auto semantic = carriers.allocate(ValueDomain::Semantic, semantic_origin.value());
        auto history = carriers.allocate(ValueDomain::History, history_origin.value());
        if (!semantic)
            return semantic.error();
        if (!history)
            return history.error();
        states.push_back(IslandState{semantic.value(), history.value()});
    }
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        islands[index].origin = function.blocks[index].origin;
        islands[index].label = function.blocks[index].label;
    }
    for (const EdgeBuild& edge : edges) {
        islands[edge.island].origin = edge.origin;
        islands[edge.island].label = edge.label;
    }

    const PhysicalValue continuation_lane =
        fixed_value(ValueDomain::ContinuationLane, kContinuationLaneCarrier);
    const PhysicalValue continuation_proof =
        fixed_value(ValueDomain::ContinuationProof, kContinuationProofCarrier);
    const PhysicalValue return_value = fixed_value(ValueDomain::Integer, kReturnValueCarrier);
    std::vector<ContinuationRelocation> relocations;

    for (std::size_t island_index = 0; island_index < function.blocks.size(); ++island_index) {
        const MachineBlock& block = function.blocks[island_index];
        PrimitivePlan& plan = islands[island_index].plan;
        PlanEmitter emitter(plan);
        const NodeId semantic = reconstruct(emitter, states[island_index].semantic);
        const NodeId history = reconstruct(emitter, states[island_index].history);
        NodeId last = semantic;
        NodeId semantic_fold = semantic;
        std::unordered_map<ValueId, NodeId> bindings;
        for (const IrValue& parameter : block.parameters) {
            bindings.emplace(parameter.id, reconstruct(emitter, values.at(parameter.id)));
        }

        for (const TypedAction& action : block.actions) {
            auto lowered = std::visit(
                [&](const auto& typed) -> Result<std::optional<NodeId>> {
                    using Action = std::decay_t<decltype(typed)>;
                    NodeId logical{};
                    if constexpr (std::is_same_v<Action, ArgumentAction>) {
                        logical = emitter.add(ReadCarrierPrimitive{
                            static_cast<CarrierId>(kArgumentCarrierBase + typed.index),
                        });
                        if (typed.output.domain == ValueDomain::Boolean) {
                            const NodeId zero = emitter.add(LiteralPrimitive{0});
                            logical = emitter.add(
                                ComparePrimitive{CompareCondition::NotEqual, logical, zero});
                        }
                    } else if constexpr (std::is_same_v<Action, ConstantAction>) {
                        logical = emitter.add(LiteralPrimitive{typed.value});
                    } else if constexpr (std::is_same_v<Action, IntegerBinaryAction>) {
                        auto binary =
                            emit_integer_binary(emitter, typed.operation, bindings.at(typed.left),
                                                bindings.at(typed.right));
                        if (!binary)
                            return binary.error();
                        logical = binary.value();
                    } else if constexpr (std::is_same_v<Action, IntegerUnaryAction>) {
                        switch (typed.operation) {
                        case IntegerUnaryOperator::BitwiseNot:
                            logical = emitter.add(NotPrimitive{bindings.at(typed.input)});
                            break;
                        case IntegerUnaryOperator::Negate:
                            logical = emitter.add(NegPrimitive{bindings.at(typed.input)});
                            break;
                        default:
                            return make_error(
                                ErrorCode::Assembly, typed.output.id,
                                "invalid integer unary operator after IR verification");
                        }
                    } else if constexpr (std::is_same_v<Action, CompareAction>) {
                        logical = emitter.add(ComparePrimitive{
                            typed.condition,
                            bindings.at(typed.left),
                            bindings.at(typed.right),
                        });
                    } else if constexpr (std::is_same_v<Action, LoadAction>) {
                        logical =
                            typed.width == 1
                                ? emitter.add(ReadMemory8Primitive{bindings.at(typed.address)})
                                : emitter.add(ReadMemory64Primitive{bindings.at(typed.address)});
                    } else {
                        const NodeId address = bindings.at(typed.address);
                        const NodeId stored = bindings.at(typed.value);
                        emitter.write_memory(typed.width, address, stored);
                        logical = emitter.add(MixPrimitive{
                            address,
                            stored,
                            compiler_salt(profile.seed(), typed.origin, 0x53544F5245ULL),
                        });
                        return std::optional<NodeId>{logical};
                    }

                    const IrValue* output = produced_value(action);
                    if (output == nullptr) {
                        return std::optional<NodeId>{logical};
                    }
                    bindings.emplace(output->id, logical);
                    stage_physical_write(
                        emitter, values.at(output->id), logical, semantic, history,
                        compiler_salt(profile.seed(), output->origin, 0x56414C5545ULL));
                    return std::optional<NodeId>{logical};
                },
                action);
            if (!lowered)
                return lowered.error();
            if (lowered.value().has_value()) {
                last = *lowered.value();
                semantic_fold = emitter.add(MixPrimitive{
                    semantic_fold,
                    last,
                    compiler_salt(profile.seed(), action_origin(action), 0x414354494F4E464FULL),
                });
            }
        }

        const NodeId new_semantic = emitter.add(MixPrimitive{
            semantic_fold,
            history,
            compiler_salt(profile.seed(), block.origin, 0x53454D414E544943ULL),
        });
        const NodeId new_history = emitter.add(MixPrimitive{
            history,
            new_semantic,
            compiler_salt(profile.seed(), block.origin, 0x484953544F5259ULL),
        });
        stage_physical_write(emitter, states[island_index].semantic, new_semantic, history, last,
                             compiler_salt(profile.seed(), block.origin, 0x53454D534852ULL));
        stage_physical_write(emitter, states[island_index].history, new_history, semantic,
                             new_semantic,
                             compiler_salt(profile.seed(), block.origin, 0x484953534852ULL));

        const auto& block_edges = outgoing[block.id];
        auto terminated = std::visit(
            [&](const auto& terminator) -> Result<void> {
                using Terminator = std::decay_t<decltype(terminator)>;
                NodeId lane{};
                NodeId proof{};
                if constexpr (std::is_same_v<Terminator, JumpTerminator>) {
                    const std::size_t target = edges.at(block_edges.at(0)).island;
                    lane = add_relocation(emitter, relocations, island_index, target,
                                          RelocationPart::Lane);
                    proof = add_relocation(emitter, relocations, island_index, target,
                                           RelocationPart::Proof);
                } else if constexpr (std::is_same_v<Terminator, BranchTerminator>) {
                    const std::size_t true_target = edges.at(block_edges.at(0)).island;
                    const std::size_t false_target = edges.at(block_edges.at(1)).island;
                    const NodeId true_lane = add_relocation(emitter, relocations, island_index,
                                                            true_target, RelocationPart::Lane);
                    const NodeId false_lane = add_relocation(emitter, relocations, island_index,
                                                             false_target, RelocationPart::Lane);
                    const NodeId true_proof = add_relocation(emitter, relocations, island_index,
                                                             true_target, RelocationPart::Proof);
                    const NodeId false_proof = add_relocation(emitter, relocations, island_index,
                                                              false_target, RelocationPart::Proof);
                    lane = emitter.add(SelectPrimitive{
                        bindings.at(terminator.condition),
                        true_lane,
                        false_lane,
                    });
                    proof = emitter.add(SelectPrimitive{
                        bindings.at(terminator.condition),
                        true_proof,
                        false_proof,
                    });
                } else if constexpr (std::is_same_v<Terminator, ReturnTerminator>) {
                    lane = add_relocation(emitter, relocations, island_index, std::nullopt,
                                          RelocationPart::Lane);
                    proof = add_relocation(emitter, relocations, island_index, std::nullopt,
                                           RelocationPart::Proof);
                    stage_physical_write(
                        emitter, return_value, bindings.at(terminator.value), new_semantic,
                        new_history,
                        compiler_salt(profile.seed(), block.origin, 0x52455455524EULL));
                } else {
                    return make_error(ErrorCode::Assembly, block.id, "missing block terminator");
                }
                stage_continuation(
                    emitter, continuation_lane, continuation_proof, lane, proof, new_semantic,
                    new_history,
                    compiler_salt(profile.seed(), block.origin, 0x434F4E54494E5545ULL));
                return {};
            },
            block.terminator);
        if (!terminated)
            return terminated.error();
        if (emitter.overflowed()) {
            return make_error(ErrorCode::Assembly, island_index,
                              "primitive node id space exhausted");
        }
    }

    for (const EdgeBuild& edge : edges) {
        PrimitivePlan& plan = islands[edge.island].plan;
        PlanEmitter emitter(plan);
        const std::size_t source_island = block_island.at(edge.source);
        const std::size_t target_island = block_island.at(edge.edge.target);
        const NodeId semantic = reconstruct(emitter, states[source_island].semantic);
        const NodeId history = reconstruct(emitter, states[source_island].history);
        NodeId last = semantic;
        NodeId semantic_fold = semantic;
        const MachineBlock& target = *blocks.at(edge.edge.target);
        for (std::size_t index = 0; index < edge.edge.arguments.size(); ++index) {
            const PhysicalValue& source = values.at(edge.edge.arguments[index]);
            const PhysicalValue& destination = values.at(target.parameters[index].id);
            const NodeId logical = reconstruct(emitter, source);
            stage_physical_write(
                emitter, destination, logical, semantic, history,
                compiler_salt(profile.seed(), destination.origin, 0x4544474556414CULL));
            last = logical;
            semantic_fold = emitter.add(MixPrimitive{
                semantic_fold,
                logical,
                compiler_salt(profile.seed(), destination.origin, 0x45444745464F4C44ULL),
            });
        }
        const NodeId new_semantic = emitter.add(MixPrimitive{
            semantic_fold,
            history,
            compiler_salt(profile.seed(), edge.origin, 0x4544474553454DULL),
        });
        const NodeId new_history = emitter.add(MixPrimitive{
            history,
            new_semantic,
            compiler_salt(profile.seed(), edge.origin, 0x45444745484953ULL),
        });
        stage_physical_write(emitter, states[target_island].semantic, new_semantic, history, last,
                             compiler_salt(profile.seed(), edge.origin, 0x4553454D534852ULL));
        stage_physical_write(emitter, states[target_island].history, new_history, semantic,
                             new_semantic,
                             compiler_salt(profile.seed(), edge.origin, 0x45484953534852ULL));

        const NodeId lane =
            add_relocation(emitter, relocations, edge.island, target_island, RelocationPart::Lane);
        const NodeId proof =
            add_relocation(emitter, relocations, edge.island, target_island, RelocationPart::Proof);
        stage_continuation(emitter, continuation_lane, continuation_proof, lane, proof,
                           new_semantic, new_history,
                           compiler_salt(profile.seed(), edge.origin, 0x45444745434F4E54ULL));
        if (emitter.overflowed()) {
            return make_error(ErrorCode::Assembly, edge.island,
                              "primitive node id space exhausted");
        }
    }

    const std::uint32_t carrier_count = carriers.count();
    std::vector<std::uint32_t> offsets(island_count, 0);
    std::uint64_t code_size = 0;
    for (std::size_t index = 0; index < islands.size(); ++index) {
        if (auto valid =
                verify_primitive_plan(islands[index].plan, carrier_count, options.plan_limits);
            !valid) {
            return valid.error();
        }
        if (code_size >= kHaltContinuation) {
            return make_error(ErrorCode::Assembly, index,
                              "island stream exceeds continuation space");
        }
        offsets[index] = static_cast<std::uint32_t>(code_size);
        code_size += primitive_plan_size(islands[index].plan);
    }
    if (code_size >= kHaltContinuation || code_size > std::numeric_limits<std::uint32_t>::max()) {
        return make_error(ErrorCode::Assembly, 0, "island stream exceeds continuation space");
    }

    const ContinuationCodec continuation_codec = profile.continuations();
    for (const ContinuationRelocation& relocation : relocations) {
        const std::uint32_t target = relocation.target_island.has_value()
                                         ? offsets.at(*relocation.target_island)
                                         : kHaltContinuation;
        const Continuation continuation = continuation_codec.seal(target);
        auto* literal = std::get_if<LiteralPrimitive>(
            &islands.at(relocation.island).plan.nodes.at(relocation.node));
        if (literal == nullptr) {
            return make_error(ErrorCode::Assembly, relocation.node,
                              "continuation relocation is not literal");
        }
        literal->value =
            relocation.part == RelocationPart::Lane ? continuation.lane : continuation.proof;
    }

    Module module;
    module.memory_size = options.memory_size;
    module.carrier_count = carrier_count;
    module.argument_count = static_cast<std::uint16_t>(function.argument_domains.size());
    module.profile_fingerprint = profile.fingerprint();
    module.data = options.initial_data;
    module.code.reserve(static_cast<std::size_t>(code_size));
    for (std::size_t index = 0; index < islands.size(); ++index) {
        auto encoded = encode_primitive_plan(islands[index].plan, profile, offsets[index]);
        if (!encoded)
            return encoded.error();
        module.code.insert(module.code.end(), encoded.value().begin(), encoded.value().end());
    }

    module.initial_carriers.assign(carrier_count, 0);
    for (const PhysicalValue& value : carriers.allocated()) {
        Word logical = 0;
        if (value.domain == ValueDomain::Semantic) {
            logical = compiler_salt(profile.seed(), value.origin, 0x53454D494E4954ULL);
        } else if (value.domain == ValueDomain::History) {
            logical = compiler_salt(profile.seed(), value.origin, 0x484953494E4954ULL);
        }
        const Word first = compiler_salt(profile.seed(), value.origin, 0x494E4954534831ULL);
        const Word second = compiler_salt(profile.seed(), value.origin, 0x494E4954534832ULL);
        if (auto written =
                write_physical_value(value, logical, first, second, module.initial_carriers);
            !written) {
            return written.error();
        }
    }
    const OriginId entry_origin = blocks.at(function.entry)->origin;
    const Continuation entry = continuation_codec.seal(offsets.at(block_island.at(function.entry)));
    if (auto written =
            write_physical_value(continuation_lane, entry.lane,
                                 compiler_salt(profile.seed(), entry_origin, 0x454E5452594C31ULL),
                                 compiler_salt(profile.seed(), entry_origin, 0x454E5452594C32ULL),
                                 module.initial_carriers);
        !written) {
        return written.error();
    }

    if (auto written =
            write_physical_value(continuation_proof, entry.proof,
                                 compiler_salt(profile.seed(), entry_origin, 0x454E5452595031ULL),
                                 compiler_salt(profile.seed(), entry_origin, 0x454E5452595032ULL),
                                 module.initial_carriers);
        !written) {
        return written.error();
    }
    if (auto written = write_physical_value(
            return_value, 0, compiler_salt(profile.seed(), entry_origin, 0x524554494E495431ULL),
            compiler_salt(profile.seed(), entry_origin, 0x524554494E495432ULL),
            module.initial_carriers);
        !written) {
        return written.error();
    }
    if (auto valid = validate_module(module); !valid)
        return valid.error();

    OriginSidecar sidecar;
    sidecar.function = function.name;
    sidecar.profile_fingerprint = profile.fingerprint();
    for (const MachineBlock& block : function.blocks) {
        const std::size_t island = block_island.at(block.id);
        sidecar.records.push_back(SidecarRecord{
            block.origin,
            "physical-island",
            block.label,
            {
                sidecar_value("block.semantic", states[island].semantic),
                sidecar_value("block.history", states[island].history),
            },
            offsets[island],
            continuation_codec.seal(offsets[island]),
        });
        for (const IrValue& parameter : block.parameters) {
            const PhysicalValue& value = values.at(parameter.id);
            sidecar.records.push_back(SidecarRecord{
                parameter.origin,
                "block-parameter",
                parameter.label,
                {sidecar_value("parameter", value)},
                offsets[island],
                continuation_codec.seal(offsets[island]),
            });
        }
        for (const TypedAction& action : block.actions) {
            if (const IrValue* output = produced_value(action)) {
                const PhysicalValue& value = values.at(output->id);
                sidecar.records.push_back(SidecarRecord{
                    output->origin,
                    "typed-action",
                    output->label,
                    {sidecar_value("output", value)},
                    offsets[island],
                    continuation_codec.seal(offsets[island]),
                });
            } else {
                sidecar.records.push_back(SidecarRecord{
                    action_origin(action),
                    "typed-effect",
                    std::string(action_label(action)),
                    {},
                    offsets[island],
                    continuation_codec.seal(offsets[island]),
                });
            }
        }
    }
    for (const EdgeBuild& edge : edges) {
        const std::size_t source_island = block_island.at(edge.source);
        const std::size_t target_island = block_island.at(edge.edge.target);
        sidecar.records.push_back(SidecarRecord{
            edge.origin,
            "edge-island",
            edge.label,
            {
                sidecar_value("source.semantic", states[source_island].semantic),
                sidecar_value("source.history", states[source_island].history),
                sidecar_value("target.semantic", states[target_island].semantic),
                sidecar_value("target.history", states[target_island].history),
            },
            offsets[edge.island],
            continuation_codec.seal(offsets[edge.island]),
        });
    }

    return CompiledProgram{std::move(module), std::move(sidecar)};
}

Result<void> save_origin_sidecar(const OriginSidecar& sidecar, const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return make_error(ErrorCode::Io, 0, "cannot open private origin sidecar: " + path.string());
    }
    output.imbue(std::locale::classic());
    output << "{\n  \"format\": \"xqvm-private-origin-v3\",\n"
           << "  \"function\": \"" << escape_json(sidecar.function) << "\",\n"
           << "  \"profile_fingerprint\": \"0x" << std::hex << std::setw(16) << std::setfill('0')
           << sidecar.profile_fingerprint << std::dec << "\",\n"
           << "  \"records\": [\n";
    for (std::size_t index = 0; index < sidecar.records.size(); ++index) {
        const SidecarRecord& record = sidecar.records[index];
        output << "    {\"origin\": " << record.origin << ", \"kind\": \""
               << escape_json(record.kind) << "\", \"label\": \"" << escape_json(record.label)
               << "\", \"island_offset\": " << record.island_offset
               << ", \"continuation\": {\"lane\": \"0x" << std::hex << std::setw(16)
               << std::setfill('0') << record.continuation.lane << "\", \"proof\": \"0x"
               << std::setw(16) << record.continuation.proof << std::dec
               << "\"}, \"physical_values\": [";
        for (std::size_t value_index = 0; value_index < record.physical_values.size();
             ++value_index) {
            if (value_index != 0)
                output << ", ";
            const SidecarPhysicalValue& value = record.physical_values[value_index];
            output << "{\"role\": \"" << escape_json(value.role)
                   << "\", \"origin\": " << value.origin << ", \"domain\": \""
                   << domain_name(value.domain) << "\", \"family\": \""
                   << (value.family == ShareFamily::Xor ? "xor" : "additive")
                   << "\", \"shares\": [";
            for (std::size_t share_index = 0; share_index < value.shares.size(); ++share_index) {
                if (share_index != 0)
                    output << ", ";
                const PhysicalShare& share = value.shares[share_index];
                output << "{\"carrier\": " << std::dec << share.carrier
                       << ", \"storage\": {\"multiplier\": \"0x" << std::hex << std::setw(16)
                       << std::setfill('0') << share.storage.multiplier << "\", \"addend\": \"0x"
                       << std::setw(16) << share.storage.addend << "\"}}";
            }
            output << std::dec << "]}";
        }
        output << "]}" << (index + 1U == sidecar.records.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    output.close();
    if (!output) {
        return make_error(ErrorCode::Io, 0,
                          "cannot write private origin sidecar: " + path.string());
    }
    return {};
}

} // namespace xqvm
