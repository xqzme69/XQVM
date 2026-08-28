#ifndef XQVM_MACHINE_IR_HPP
#define XQVM_MACHINE_IR_HPP

#include "xqvm/physical.hpp"
#include "xqvm/result.hpp"
#include "xqvm/schema.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace xqvm {

using ValueId = std::uint32_t;
using BlockId = std::uint32_t;

struct IrValue {
    ValueId id{};
    ValueDomain domain{ValueDomain::Integer};
    OriginId origin{};
    std::string label;
};

enum class IntegerBinaryOperator : Byte {
    Add,
    Subtract,
    Multiply,
    Xor,
    And,
    Or,
    ShiftLeft,
    ShiftRightLogical,
    ShiftRightArithmetic,
    RotateLeft,
    RotateRight,
};

enum class IntegerUnaryOperator : Byte {
    BitwiseNot,
    Negate,
};

struct ArgumentAction {
    IrValue output;
    std::uint16_t index{};
};

struct ConstantAction {
    IrValue output;
    Word value{};
};

struct IntegerBinaryAction {
    IrValue output;
    IntegerBinaryOperator operation{IntegerBinaryOperator::Add};
    ValueId left{};
    ValueId right{};
};

struct IntegerUnaryAction {
    IrValue output;
    IntegerUnaryOperator operation{IntegerUnaryOperator::BitwiseNot};
    ValueId input{};
};

struct CompareAction {
    IrValue output;
    CompareCondition condition{CompareCondition::Equal};
    ValueId left{};
    ValueId right{};
};

struct LoadAction {
    IrValue output;
    ValueId address{};
    Byte width{8};
};

struct StoreAction {
    OriginId origin{};
    std::string label;
    ValueId address{};
    ValueId value{};
    Byte width{8};
};

using TypedAction = std::variant<ArgumentAction, ConstantAction, IntegerBinaryAction,
                                 IntegerUnaryAction, CompareAction, LoadAction, StoreAction>;

[[nodiscard]] const IrValue* produced_value(const TypedAction& action);
[[nodiscard]] OriginId action_origin(const TypedAction& action);
[[nodiscard]] std::string_view action_label(const TypedAction& action);

struct BlockEdge {
    BlockId target{};
    std::vector<ValueId> arguments;
};

struct JumpTerminator {
    BlockEdge edge;
};

struct BranchTerminator {
    ValueId condition{};
    BlockEdge when_true;
    BlockEdge when_false;
};

struct ReturnTerminator {
    ValueId value{};
};

using BlockTerminator =
    std::variant<std::monostate, JumpTerminator, BranchTerminator, ReturnTerminator>;

struct MachineBlock {
    BlockId id{};
    OriginId origin{};
    std::string label;
    std::vector<IrValue> parameters;
    std::vector<TypedAction> actions;
    BlockTerminator terminator;
};

struct MachineFunction {
    std::string name;
    BlockId entry{};
    std::vector<ValueDomain> argument_domains;
    std::vector<MachineBlock> blocks;
};

struct IrVerificationReport {
    std::size_t blocks{};
    std::size_t values{};
    std::size_t actions{};
    std::size_t edges{};
};

[[nodiscard]] Result<IrVerificationReport> verify_machine_ir(const MachineFunction& function);

} // namespace xqvm

#endif // XQVM_MACHINE_IR_HPP
