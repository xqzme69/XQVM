#include "xqvm/runtime.hpp"

#include "xqvm/compiler.hpp"
#include "xqvm/disassembler.hpp"
#include "xqvm/physical.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>

namespace xqvm {
namespace {

class CodeCursor {
  public:
    CodeCursor(std::span<const Byte> code, const EncodingProfile& profile, std::size_t position)
        : code_(code), profile_(profile), position_(position), limit_(code.size()) {}

    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

    [[nodiscard]] Result<void> set_limit(std::size_t limit) {
        if (limit < position_ || limit > code_.size()) {
            return make_error(ErrorCode::IslandBounds, position_,
                              "island size exceeds code section");
        }
        limit_ = limit;
        return {};
    }

    [[nodiscard]] Result<Byte> read_byte() {
        if (position_ >= limit_) {
            return make_error(ErrorCode::IslandBounds, position_, "truncated physical island");
        }
        const Byte plain = profile_.mask_code_byte(code_[position_], position_);
        ++position_;
        return plain;
    }

    [[nodiscard]] Result<std::uint16_t> read_u16() {
        auto low = read_byte();
        auto high = read_byte();
        if (!low)
            return low.error();
        if (!high)
            return high.error();
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(low.value()) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(high.value()) << 8U));
    }

    [[nodiscard]] Result<std::uint32_t> read_u32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            auto byte = read_byte();
            if (!byte)
                return byte.error();
            value |= static_cast<std::uint32_t>(byte.value()) << shift;
        }
        return value;
    }

    [[nodiscard]] Result<std::uint64_t> read_u64() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            auto byte = read_byte();
            if (!byte)
                return byte.error();
            value |= static_cast<std::uint64_t>(byte.value()) << shift;
        }
        return value;
    }

  private:
    std::span<const Byte> code_;
    const EncodingProfile& profile_;
    std::size_t position_{};
    std::size_t limit_{};
};

[[nodiscard]] RuntimeFault fault_from_error(RuntimeFaultCode code, const Error& error) {
    switch (error.code) {
    case ErrorCode::IslandBounds:
        code = RuntimeFaultCode::IslandBounds;
        break;
    case ErrorCode::InvalidSchema:
        code = RuntimeFaultCode::InvalidSchema;
        break;
    case ErrorCode::InvalidOperand:
        code = RuntimeFaultCode::InvalidOperand;
        break;
    case ErrorCode::CarrierBounds:
        code = RuntimeFaultCode::CarrierBounds;
        break;
    case ErrorCode::MemoryBounds:
        code = RuntimeFaultCode::MemoryBounds;
        break;
    case ErrorCode::WriteConflict:
        code = RuntimeFaultCode::WriteConflict;
        break;
    case ErrorCode::ResourceLimit:
        code = RuntimeFaultCode::ResourceLimit;
        break;
    default:
        break;
    }
    return RuntimeFault{code, error.offset, error.message};
}

class TargetLocalFusedExecutor {
  public:
    TargetLocalFusedExecutor(CodeCursor& cursor, const MachineState& state,
                             std::vector<Word>& nodes)
        : cursor_(cursor), state_(state), nodes_(nodes) {}

    [[nodiscard]] Result<Word> execute(PrimitiveKind kind) {
        const auto index = static_cast<std::size_t>(kind);
        if (index >= handlers_.size()) {
            return make_error(ErrorCode::InvalidSchema, cursor_.position(),
                              "invalid physical schema");
        }
        return (this->*handlers_[index])();
    }

  private:
    using Handler = Result<Word> (TargetLocalFusedExecutor::*)();

    [[nodiscard]] Result<Word> node(NodeId id) const {
        if (id >= nodes_.size()) {
            return make_error(ErrorCode::InvalidOperand, id,
                              "primitive operand is not a prior DAG node");
        }
        return nodes_[id];
    }

    [[nodiscard]] Result<std::pair<Word, Word>> binary_operands() {
        auto left_id = cursor_.read_u16();
        auto right_id = cursor_.read_u16();
        if (!left_id)
            return left_id.error();
        if (!right_id)
            return right_id.error();
        auto left = node(left_id.value());
        auto right = node(right_id.value());
        if (!left)
            return left.error();
        if (!right)
            return right.error();
        return std::pair<Word, Word>{left.value(), right.value()};
    }

    [[nodiscard]] Result<Word> literal() {
        return cursor_.read_u64();
    }

    [[nodiscard]] Result<Word> read_carrier() {
        auto carrier = cursor_.read_u32();
        if (!carrier)
            return carrier.error();
        if (carrier.value() >= state_.carriers.size()) {
            return make_error(ErrorCode::CarrierBounds, carrier.value(),
                              "carrier read is out of range");
        }
        return state_.carriers[carrier.value()];
    }

    [[nodiscard]] Result<Word> read_memory8() {
        auto address_id = cursor_.read_u16();
        if (!address_id)
            return address_id.error();
        auto address = node(address_id.value());
        if (!address)
            return address.error();
        if (address.value() >= state_.memory.size()) {
            return make_error(ErrorCode::MemoryBounds, address.value(),
                              "byte read is out of range");
        }
        return state_.memory[static_cast<std::size_t>(address.value())];
    }

    [[nodiscard]] Result<Word> read_memory64() {
        auto address_id = cursor_.read_u16();
        if (!address_id)
            return address_id.error();
        auto address = node(address_id.value());
        if (!address)
            return address.error();
        if (address.value() > state_.memory.size() ||
            state_.memory.size() - static_cast<std::size_t>(address.value()) < sizeof(Word)) {
            return make_error(ErrorCode::MemoryBounds, address.value(),
                              "word read is out of range");
        }
        Word value = 0;
        const std::size_t base = static_cast<std::size_t>(address.value());
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            value |= static_cast<Word>(state_.memory[base + shift / 8U]) << shift;
        }
        return value;
    }

    [[nodiscard]] Result<Word> add() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return operands.value().first + operands.value().second;
    }
    [[nodiscard]] Result<Word> sub() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return operands.value().first - operands.value().second;
    }
    [[nodiscard]] Result<Word> mul() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return operands.value().first * operands.value().second;
    }
    [[nodiscard]] Result<Word> bit_xor() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return operands.value().first ^ operands.value().second;
    }
    [[nodiscard]] Result<Word> bit_and() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return operands.value().first & operands.value().second;
    }
    [[nodiscard]] Result<Word> bit_or() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return operands.value().first | operands.value().second;
    }

    [[nodiscard]] Result<Word> bit_not() {
        auto id = cursor_.read_u16();
        if (!id)
            return id.error();
        auto value = node(id.value());
        if (!value)
            return value.error();
        return ~value.value();
    }

    [[nodiscard]] Result<Word> negate() {
        auto id = cursor_.read_u16();
        if (!id)
            return id.error();
        auto value = node(id.value());
        if (!value)
            return value.error();
        return Word{0} - value.value();
    }

    [[nodiscard]] Result<Word> shl() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return operands.value().first << (operands.value().second & 63U);
    }
    [[nodiscard]] Result<Word> shr() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return operands.value().first >> (operands.value().second & 63U);
    }
    [[nodiscard]] Result<Word> sar() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        const unsigned amount = static_cast<unsigned>(operands.value().second & 63U);
        if (amount == 0)
            return operands.value().first;
        Word result = operands.value().first >> amount;
        if ((operands.value().first >> 63U) != 0) {
            result |= ~Word{0} << (64U - amount);
        }
        return result;
    }
    [[nodiscard]] Result<Word> rol() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return std::rotl(operands.value().first, static_cast<int>(operands.value().second & 63U));
    }
    [[nodiscard]] Result<Word> ror() {
        auto operands = binary_operands();
        if (!operands)
            return operands.error();
        return std::rotr(operands.value().first, static_cast<int>(operands.value().second & 63U));
    }

    [[nodiscard]] Result<Word> compare() {
        auto condition = cursor_.read_byte();
        auto left_id = cursor_.read_u16();
        auto right_id = cursor_.read_u16();
        if (!condition)
            return condition.error();
        if (!left_id)
            return left_id.error();
        if (!right_id)
            return right_id.error();
        if (condition.value() > static_cast<Byte>(CompareCondition::SignedLessEqual)) {
            return make_error(ErrorCode::InvalidOperand, condition.value(),
                              "invalid compare condition");
        }
        auto left = node(left_id.value());
        auto right = node(right_id.value());
        if (!left)
            return left.error();
        if (!right)
            return right.error();
        bool result = false;
        switch (static_cast<CompareCondition>(condition.value())) {
        case CompareCondition::Equal:
            result = left.value() == right.value();
            break;
        case CompareCondition::NotEqual:
            result = left.value() != right.value();
            break;
        case CompareCondition::UnsignedLess:
            result = left.value() < right.value();
            break;
        case CompareCondition::UnsignedLessEqual:
            result = left.value() <= right.value();
            break;
        case CompareCondition::SignedLess:
            result = std::bit_cast<std::int64_t>(left.value()) <
                     std::bit_cast<std::int64_t>(right.value());
            break;
        case CompareCondition::SignedLessEqual:
            result = std::bit_cast<std::int64_t>(left.value()) <=
                     std::bit_cast<std::int64_t>(right.value());
            break;
        }
        return result ? Word{1} : Word{0};
    }

    [[nodiscard]] Result<Word> select() {
        auto condition_id = cursor_.read_u16();
        auto true_id = cursor_.read_u16();
        auto false_id = cursor_.read_u16();
        if (!condition_id)
            return condition_id.error();
        if (!true_id)
            return true_id.error();
        if (!false_id)
            return false_id.error();
        auto condition = node(condition_id.value());
        if (!condition)
            return condition.error();
        return node(condition.value() != 0 ? true_id.value() : false_id.value());
    }

    [[nodiscard]] Result<Word> mix() {
        auto left_id = cursor_.read_u16();
        auto right_id = cursor_.read_u16();
        auto salt = cursor_.read_u64();
        if (!left_id)
            return left_id.error();
        if (!right_id)
            return right_id.error();
        if (!salt)
            return salt.error();
        auto left = node(left_id.value());
        auto right = node(right_id.value());
        if (!left)
            return left.error();
        if (!right)
            return right.error();
        return mix_word(left.value(), right.value(), salt.value());
    }

    CodeCursor& cursor_;
    const MachineState& state_;
    std::vector<Word>& nodes_;

    static constexpr std::array<Handler, kPrimitiveSchemaCount> handlers_{
        &TargetLocalFusedExecutor::literal,      &TargetLocalFusedExecutor::read_carrier,
        &TargetLocalFusedExecutor::read_memory8, &TargetLocalFusedExecutor::read_memory64,
        &TargetLocalFusedExecutor::add,          &TargetLocalFusedExecutor::sub,
        &TargetLocalFusedExecutor::mul,          &TargetLocalFusedExecutor::bit_xor,
        &TargetLocalFusedExecutor::bit_and,      &TargetLocalFusedExecutor::bit_or,
        &TargetLocalFusedExecutor::bit_not,      &TargetLocalFusedExecutor::negate,
        &TargetLocalFusedExecutor::shl,          &TargetLocalFusedExecutor::shr,
        &TargetLocalFusedExecutor::sar,          &TargetLocalFusedExecutor::rol,
        &TargetLocalFusedExecutor::ror,          &TargetLocalFusedExecutor::compare,
        &TargetLocalFusedExecutor::select,       &TargetLocalFusedExecutor::mix,
    };
};

struct StagedMemoryWrite {
    std::size_t address{};
    Byte width{};
    Word value{};
};

struct IslandExecution {
    std::uint16_t nodes{};
    std::uint16_t carrier_writes{};
    std::uint16_t memory_writes{};
};

[[nodiscard]] Result<IslandExecution>
execute_island(MachineState& state, std::span<const Byte> code, const EncodingProfile& profile,
               std::uint32_t opened_offset, const PrimitivePlanLimits& limits) {
    if (opened_offset >= code.size()) {
        return make_error(ErrorCode::IslandBounds, opened_offset,
                          "continuation points outside code");
    }
    CodeCursor cursor(code, profile, opened_offset);
    auto size = cursor.read_u32();
    auto node_count = cursor.read_u16();
    auto carrier_write_count = cursor.read_u16();
    auto memory_write_count = cursor.read_u16();
    auto reserved = cursor.read_u16();
    if (!size)
        return size.error();
    if (!node_count)
        return node_count.error();
    if (!carrier_write_count)
        return carrier_write_count.error();
    if (!memory_write_count)
        return memory_write_count.error();
    if (!reserved)
        return reserved.error();
    if (reserved.value() != 0 || size.value() < 12U || size.value() > code.size() - opened_offset) {
        return make_error(ErrorCode::IslandBounds, opened_offset, "invalid physical island header");
    }
    if (node_count.value() == 0 || node_count.value() > limits.max_nodes ||
        carrier_write_count.value() > limits.max_carrier_writes ||
        memory_write_count.value() > limits.max_memory_writes) {
        return make_error(ErrorCode::ResourceLimit, opened_offset,
                          "physical island exceeds runtime limits");
    }
    if (auto limited = cursor.set_limit(opened_offset + size.value()); !limited) {
        return limited.error();
    }

    std::vector<Word> nodes;
    nodes.reserve(node_count.value());
    TargetLocalFusedExecutor executor(cursor, state, nodes);
    for (std::uint16_t index = 0; index < node_count.value(); ++index) {
        auto encoded = cursor.read_byte();
        if (!encoded)
            return encoded.error();
        auto kind = profile.primitives().decode(encoded.value());
        if (!kind) {
            return make_error(ErrorCode::InvalidSchema, cursor.position() - 1U,
                              "physical opcode is absent from the external profile");
        }
        auto value = executor.execute(kind.value());
        if (!value)
            return value.error();
        nodes.push_back(value.value());
    }

    std::vector<std::pair<CarrierId, Word>> carrier_writes;
    carrier_writes.reserve(carrier_write_count.value());
    std::unordered_set<CarrierId> carrier_targets;
    for (std::uint16_t index = 0; index < carrier_write_count.value(); ++index) {
        auto carrier = cursor.read_u32();
        auto value_id = cursor.read_u16();
        if (!carrier)
            return carrier.error();
        if (!value_id)
            return value_id.error();
        if (carrier.value() >= state.carriers.size()) {
            return make_error(ErrorCode::CarrierBounds, carrier.value(),
                              "delayed carrier write is out of range");
        }
        if (value_id.value() >= nodes.size()) {
            return make_error(ErrorCode::InvalidOperand, value_id.value(),
                              "delayed carrier write references a missing DAG node");
        }
        if (carrier.value() >= 9U && carrier.value() < kFirstAllocatedCarrier) {
            return make_error(ErrorCode::CarrierBounds, carrier.value(),
                              "island writes a read-only ABI carrier");
        }
        if (!carrier_targets.emplace(carrier.value()).second) {
            return make_error(ErrorCode::WriteConflict, carrier.value(),
                              "duplicate delayed carrier write");
        }
        carrier_writes.emplace_back(carrier.value(), nodes[value_id.value()]);
    }
    for (CarrierId carrier = kContinuationLaneCarrier; carrier < kContinuationProofCarrier + 3U;
         ++carrier) {
        if (!carrier_targets.contains(carrier)) {
            return make_error(ErrorCode::InvalidOperand, carrier,
                              "island does not replace every distributed continuation share");
        }
    }

    std::vector<StagedMemoryWrite> memory_writes;
    memory_writes.reserve(memory_write_count.value());
    for (std::uint16_t index = 0; index < memory_write_count.value(); ++index) {
        auto width = cursor.read_byte();
        auto address_id = cursor.read_u16();
        auto value_id = cursor.read_u16();
        if (!width)
            return width.error();
        if (!address_id)
            return address_id.error();
        if (!value_id)
            return value_id.error();
        if ((width.value() != 1 && width.value() != 8) || address_id.value() >= nodes.size() ||
            value_id.value() >= nodes.size()) {
            return make_error(ErrorCode::InvalidOperand, cursor.position(),
                              "invalid delayed memory write");
        }
        const Word address_word = nodes[address_id.value()];
        if (address_word > state.memory.size() ||
            state.memory.size() - static_cast<std::size_t>(address_word) < width.value()) {
            return make_error(ErrorCode::MemoryBounds, address_word,
                              "delayed memory write is out of range");
        }
        const std::size_t address = static_cast<std::size_t>(address_word);
        for (const StagedMemoryWrite& previous : memory_writes) {
            const std::size_t previous_end = previous.address + previous.width;
            const std::size_t current_end = address + width.value();
            if (address < previous_end && previous.address < current_end) {
                return make_error(ErrorCode::WriteConflict, address,
                                  "overlapping delayed memory writes");
            }
        }
        memory_writes.push_back(StagedMemoryWrite{
            address,
            width.value(),
            nodes[value_id.value()],
        });
    }
    if (cursor.position() != opened_offset + size.value()) {
        return make_error(ErrorCode::IslandBounds, cursor.position(),
                          "island size has trailing bytes");
    }

    for (const auto& [carrier, value] : carrier_writes) {
        state.carriers[carrier] = value;
    }
    for (const StagedMemoryWrite& write : memory_writes) {
        for (unsigned shift = 0; shift < static_cast<unsigned>(write.width) * 8U; shift += 8U) {
            state.memory[write.address + shift / 8U] = static_cast<Byte>(write.value >> shift);
        }
    }
    return IslandExecution{
        node_count.value(),
        carrier_write_count.value(),
        memory_write_count.value(),
    };
}

[[nodiscard]] Continuation current_continuation(const MachineState& state) {
    return Continuation{
        state.carriers[kContinuationLaneCarrier] ^ state.carriers[kContinuationLaneCarrier + 1U] ^
            state.carriers[kContinuationLaneCarrier + 2U],
        state.carriers[kContinuationProofCarrier] ^ state.carriers[kContinuationProofCarrier + 1U] ^
            state.carriers[kContinuationProofCarrier + 2U],
    };
}

[[nodiscard]] Word current_return_value(const MachineState& state) {
    return state.carriers[kReturnValueCarrier] ^ state.carriers[kReturnValueCarrier + 1U] ^
           state.carriers[kReturnValueCarrier + 2U];
}

} // namespace

Result<Machine> Machine::create(Module module, EncodingProfile profile,
                                const PrimitivePlanLimits& plan_limits) {
    if (auto valid = validate_module(module); !valid) {
        return valid.error();
    }
    if (module.profile_fingerprint != profile.fingerprint()) {
        return make_error(ErrorCode::Verification, 0, "module and external profile do not match");
    }
    if (module.carrier_count < kFirstAllocatedCarrier ||
        module.initial_carriers.size() != module.carrier_count) {
        return make_error(ErrorCode::InvalidModule, 0,
                          "module has no reserved physical carrier window");
    }
    if (module.argument_count > kMaximumArguments) {
        return make_error(ErrorCode::InvalidModule, module.argument_count,
                          "module exceeds the physical argument carrier window");
    }
    if (module.data.size() > module.memory_size || module.code.empty()) {
        return make_error(ErrorCode::InvalidModule, 0, "module state is malformed");
    }
    auto verified = verify_physical_module(module, profile, plan_limits);
    if (!verified) {
        return verified.error();
    }
    return Machine(std::move(module), profile, plan_limits,
                   std::move(verified.value().island_offsets));
}

RunResult Machine::run(std::span<const Word> arguments, const RunOptions& options) const {
    MachineState state;
    state.carriers = module_.initial_carriers;
    state.memory.assign(module_.memory_size, 0);
    std::copy(module_.data.begin(), module_.data.end(), state.memory.begin());

    if (arguments.size() != module_.argument_count) {
        return RunResult{
            std::nullopt,
            RuntimeFault{
                RuntimeFaultCode::ArgumentWindow,
                arguments.size(),
                "runtime argument count does not match the compiled MachineIR contract",
            },
            std::move(state),
        };
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        state.carriers[kArgumentCarrierBase + index] = arguments[index];
    }

    const ContinuationCodec codec = profile_.continuations();
    const PrimitivePlanLimits plan_limits = options.plan_limits.value_or(plan_limits_);
    for (;;) {
        auto opened = codec.open(current_continuation(state));
        if (!opened) {
            return RunResult{
                std::nullopt,
                fault_from_error(RuntimeFaultCode::ContinuationProof, opened.error()),
                std::move(state),
            };
        }
        if (opened.value() == kHaltContinuation) {
            return RunResult{current_return_value(state), std::nullopt, std::move(state)};
        }
        if (state.islands_executed >= options.max_islands) {
            return RunResult{
                std::nullopt,
                RuntimeFault{
                    RuntimeFaultCode::ResourceLimit,
                    state.islands_executed,
                    "physical island execution limit reached",
                },
                std::move(state),
            };
        }
        if (!std::binary_search(island_offsets_.begin(), island_offsets_.end(), opened.value())) {
            return RunResult{
                std::nullopt,
                RuntimeFault{
                    RuntimeFaultCode::IslandBounds,
                    opened.value(),
                    "continuation does not target a verified island boundary",
                },
                std::move(state),
            };
        }

        auto execution = execute_island(state, module_.code, profile_, opened.value(), plan_limits);
        if (!execution) {
            return RunResult{
                std::nullopt,
                fault_from_error(RuntimeFaultCode::InvalidOperand, execution.error()),
                std::move(state),
            };
        }
        ++state.islands_executed;
        if (options.trace) {
            options.trace(IslandTraceEvent{
                state.islands_executed,
                opened.value(),
                execution.value().nodes,
                execution.value().carrier_writes,
                execution.value().memory_writes,
            });
        }
    }
}

std::string_view runtime_fault_name(RuntimeFaultCode code) noexcept {
    switch (code) {
    case RuntimeFaultCode::ArgumentWindow:
        return "argument-window";
    case RuntimeFaultCode::ContinuationProof:
        return "continuation-proof";
    case RuntimeFaultCode::IslandBounds:
        return "island-bounds";
    case RuntimeFaultCode::InvalidSchema:
        return "invalid-schema";
    case RuntimeFaultCode::InvalidOperand:
        return "invalid-operand";
    case RuntimeFaultCode::CarrierBounds:
        return "carrier-bounds";
    case RuntimeFaultCode::MemoryBounds:
        return "memory-bounds";
    case RuntimeFaultCode::WriteConflict:
        return "write-conflict";
    case RuntimeFaultCode::ResourceLimit:
        return "resource-limit";
    }
    return "unknown";
}

} // namespace xqvm
