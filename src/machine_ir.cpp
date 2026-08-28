#include "xqvm/machine_ir.hpp"

#include <algorithm>
#include <cstddef>
#include <queue>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace xqvm {
namespace {

using DomainMap = std::unordered_map<ValueId, ValueDomain>;

[[nodiscard]] Error ir_error(std::size_t offset, std::string message) {
    return make_error(ErrorCode::Verification, offset, std::move(message));
}

[[nodiscard]] Result<ValueDomain> require_local(const DomainMap& available, ValueId id,
                                                const char* role) {
    const auto found = available.find(id);
    if (found == available.end()) {
        return ir_error(id, std::string(role) + " is not defined in this block");
    }
    return found->second;
}

[[nodiscard]] Result<void> require_domain(const DomainMap& available, ValueId id,
                                          ValueDomain expected, const char* role) {
    auto domain = require_local(available, id, role);
    if (!domain) {
        return domain.error();
    }
    if (domain.value() != expected) {
        return ir_error(id, std::string(role) + " has domain " +
                                std::string(domain_name(domain.value())) + ", expected " +
                                std::string(domain_name(expected)));
    }
    return {};
}

[[nodiscard]] bool is_data_domain(ValueDomain domain) noexcept {
    return domain == ValueDomain::Integer || domain == ValueDomain::Address ||
           domain == ValueDomain::Boolean;
}

[[nodiscard]] bool is_valid_binary_operator(IntegerBinaryOperator operation) noexcept {
    return static_cast<Byte>(operation) <= static_cast<Byte>(IntegerBinaryOperator::RotateRight);
}

[[nodiscard]] bool is_valid_unary_operator(IntegerUnaryOperator operation) noexcept {
    return static_cast<Byte>(operation) <= static_cast<Byte>(IntegerUnaryOperator::Negate);
}

[[nodiscard]] bool is_valid_compare_condition(CompareCondition condition) noexcept {
    return static_cast<Byte>(condition) <= static_cast<Byte>(CompareCondition::SignedLessEqual);
}

[[nodiscard]] bool is_continuation_byte(unsigned char byte) noexcept {
    return byte >= 0x80U && byte <= 0xBFU;
}

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            if (index + 1U >= text.size() ||
                !is_continuation_byte(static_cast<unsigned char>(text[index + 1U]))) {
                return false;
            }
            index += 2U;
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU) {
            if (index + 2U >= text.size())
                return false;
            const auto second = static_cast<unsigned char>(text[index + 1U]);
            const auto third = static_cast<unsigned char>(text[index + 2U]);
            const bool second_valid = first == 0xE0U   ? second >= 0xA0U && second <= 0xBFU
                                      : first == 0xEDU ? second >= 0x80U && second <= 0x9FU
                                                       : is_continuation_byte(second);
            if (!second_valid || !is_continuation_byte(third))
                return false;
            index += 3U;
            continue;
        }
        if (first >= 0xF0U && first <= 0xF4U) {
            if (index + 3U >= text.size())
                return false;
            const auto second = static_cast<unsigned char>(text[index + 1U]);
            const bool second_valid = first == 0xF0U   ? second >= 0x90U && second <= 0xBFU
                                      : first == 0xF4U ? second >= 0x80U && second <= 0x8FU
                                                       : is_continuation_byte(second);
            if (!second_valid ||
                !is_continuation_byte(static_cast<unsigned char>(text[index + 2U])) ||
                !is_continuation_byte(static_cast<unsigned char>(text[index + 3U]))) {
                return false;
            }
            index += 4U;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] Result<void>
verify_edge(const BlockEdge& edge, const DomainMap& available,
            const std::unordered_map<BlockId, const MachineBlock*>& blocks) {
    const auto target = blocks.find(edge.target);
    if (target == blocks.end()) {
        return ir_error(edge.target, "edge targets a missing block");
    }
    if (edge.arguments.size() != target->second->parameters.size()) {
        return ir_error(edge.target, "edge argument count does not match block parameters");
    }
    for (std::size_t index = 0; index < edge.arguments.size(); ++index) {
        auto domain = require_local(available, edge.arguments[index], "edge argument");
        if (!domain) {
            return domain.error();
        }
        if (domain.value() != target->second->parameters[index].domain) {
            return ir_error(edge.arguments[index], "edge argument domain mismatch");
        }
    }
    return {};
}

} // namespace

const IrValue* produced_value(const TypedAction& action) {
    return std::visit(
        [](const auto& typed) -> const IrValue* {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Action, StoreAction>) {
                return nullptr;
            } else {
                return &typed.output;
            }
        },
        action);
}

OriginId action_origin(const TypedAction& action) {
    return std::visit(
        [](const auto& typed) -> OriginId {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Action, StoreAction>) {
                return typed.origin;
            } else {
                return typed.output.origin;
            }
        },
        action);
}

std::string_view action_label(const TypedAction& action) {
    return std::visit(
        [](const auto& typed) -> std::string_view {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Action, StoreAction>) {
                return typed.label;
            } else {
                return typed.output.label;
            }
        },
        action);
}

Result<IrVerificationReport> verify_machine_ir(const MachineFunction& function) {
    if (function.blocks.empty()) {
        return ir_error(0, "MachineIR function has no blocks");
    }
    if (!is_valid_utf8(function.name)) {
        return ir_error(0, "function name is not valid UTF-8");
    }

    std::unordered_map<BlockId, const MachineBlock*> blocks;
    std::unordered_set<OriginId> origins;
    std::unordered_set<ValueId> all_values;
    for (const MachineBlock& block : function.blocks) {
        if (!is_valid_utf8(block.label)) {
            return ir_error(block.id, "block label is not valid UTF-8");
        }
        if (!blocks.emplace(block.id, &block).second) {
            return ir_error(block.id, "duplicate block id");
        }
        if (block.origin == 0 || !origins.emplace(block.origin).second) {
            return ir_error(block.origin, "block origin must be nonzero and unique");
        }
        for (const IrValue& parameter : block.parameters) {
            if (!is_valid_utf8(parameter.label)) {
                return ir_error(parameter.id, "value label is not valid UTF-8");
            }
            if (!is_data_domain(parameter.domain)) {
                return ir_error(parameter.id, "block parameter uses an internal physical domain");
            }
            if (parameter.origin == 0 || !origins.emplace(parameter.origin).second) {
                return ir_error(parameter.origin, "value origin must be nonzero and unique");
            }
            if (!all_values.emplace(parameter.id).second) {
                return ir_error(parameter.id, "duplicate MachineIR value id");
            }
        }
        for (const TypedAction& action : block.actions) {
            if (!is_valid_utf8(action_label(action))) {
                return ir_error(action_origin(action), "action label is not valid UTF-8");
            }
            if (const IrValue* output = produced_value(action)) {
                if (!is_data_domain(output->domain)) {
                    return ir_error(output->id, "action output uses an internal physical domain");
                }
                if (!all_values.emplace(output->id).second) {
                    return ir_error(output->id, "duplicate MachineIR value id");
                }
            }
            const OriginId origin = action_origin(action);
            if (origin == 0 || !origins.emplace(origin).second) {
                return ir_error(origin, "action origin must be nonzero and unique");
            }
        }
    }

    if (!blocks.contains(function.entry)) {
        return ir_error(function.entry, "entry block is missing");
    }
    if (!blocks.at(function.entry)->parameters.empty()) {
        return ir_error(function.entry, "entry block cannot have block parameters");
    }

    std::vector<bool> seen_arguments(function.argument_domains.size(), false);
    IrVerificationReport report;
    report.blocks = function.blocks.size();
    report.values = all_values.size();

    std::unordered_map<BlockId, std::vector<BlockId>> successors;
    for (const MachineBlock& block : function.blocks) {
        DomainMap available;
        for (const IrValue& parameter : block.parameters) {
            available.emplace(parameter.id, parameter.domain);
        }

        for (const TypedAction& action : block.actions) {
            ++report.actions;
            auto valid = std::visit(
                [&](const auto& typed) -> Result<void> {
                    using Action = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<Action, ArgumentAction>) {
                        if (block.id != function.entry) {
                            return ir_error(typed.output.id,
                                            "argument action is outside entry block");
                        }
                        if (typed.index >= function.argument_domains.size()) {
                            return ir_error(typed.index, "argument index is out of range");
                        }
                        if (seen_arguments[typed.index]) {
                            return ir_error(typed.index, "argument index is materialized twice");
                        }
                        if (typed.output.domain != function.argument_domains[typed.index]) {
                            return ir_error(typed.output.id, "argument domain mismatch");
                        }
                        seen_arguments[typed.index] = true;
                    } else if constexpr (std::is_same_v<Action, ConstantAction>) {
                        if (typed.output.domain == ValueDomain::Boolean && typed.value > 1) {
                            return ir_error(typed.output.id,
                                            "boolean constant must be zero or one");
                        }
                    } else if constexpr (std::is_same_v<Action, IntegerBinaryAction>) {
                        if (!is_valid_binary_operator(typed.operation)) {
                            return ir_error(typed.output.id, "invalid integer binary operator");
                        }
                        if (typed.output.domain != ValueDomain::Integer) {
                            return ir_error(typed.output.id,
                                            "integer binary output is not integer");
                        }
                        if (auto result = require_domain(
                                available, typed.left, ValueDomain::Integer, "binary left operand");
                            !result) {
                            return result;
                        }
                        if (auto result =
                                require_domain(available, typed.right, ValueDomain::Integer,
                                               "binary right operand");
                            !result) {
                            return result;
                        }
                    } else if constexpr (std::is_same_v<Action, IntegerUnaryAction>) {
                        if (!is_valid_unary_operator(typed.operation)) {
                            return ir_error(typed.output.id, "invalid integer unary operator");
                        }
                        if (typed.output.domain != ValueDomain::Integer) {
                            return ir_error(typed.output.id, "integer unary output is not integer");
                        }
                        if (auto result = require_domain(available, typed.input,
                                                         ValueDomain::Integer, "unary operand");
                            !result) {
                            return result;
                        }
                    } else if constexpr (std::is_same_v<Action, CompareAction>) {
                        if (!is_valid_compare_condition(typed.condition)) {
                            return ir_error(typed.output.id, "invalid comparison condition");
                        }
                        if (typed.output.domain != ValueDomain::Boolean) {
                            return ir_error(typed.output.id, "comparison output is not boolean");
                        }
                        auto left = require_local(available, typed.left, "comparison left operand");
                        auto right =
                            require_local(available, typed.right, "comparison right operand");
                        if (!left)
                            return left.error();
                        if (!right)
                            return right.error();
                        if (left.value() != right.value() ||
                            (left.value() != ValueDomain::Integer &&
                             left.value() != ValueDomain::Address)) {
                            return ir_error(typed.output.id, "comparison operand domains mismatch");
                        }
                    } else if constexpr (std::is_same_v<Action, LoadAction>) {
                        if (typed.output.domain != ValueDomain::Integer) {
                            return ir_error(typed.output.id, "load output is not integer");
                        }
                        if (typed.width != 1 && typed.width != 8) {
                            return ir_error(typed.output.id, "load width must be 1 or 8");
                        }
                        if (auto result = require_domain(available, typed.address,
                                                         ValueDomain::Address, "load address");
                            !result) {
                            return result;
                        }
                    } else if constexpr (std::is_same_v<Action, StoreAction>) {
                        if (typed.width != 1 && typed.width != 8) {
                            return ir_error(typed.origin, "store width must be 1 or 8");
                        }
                        if (auto result = require_domain(available, typed.address,
                                                         ValueDomain::Address, "store address");
                            !result) {
                            return result;
                        }
                        if (auto result = require_domain(available, typed.value,
                                                         ValueDomain::Integer, "stored value");
                            !result) {
                            return result;
                        }
                    }
                    return {};
                },
                action);
            if (!valid) {
                return valid.error();
            }
            if (const IrValue* output = produced_value(action)) {
                available.emplace(output->id, output->domain);
            }
        }

        auto add_edge = [&](const BlockEdge& edge) -> Result<void> {
            if (auto result = verify_edge(edge, available, blocks); !result) {
                return result;
            }
            successors[block.id].push_back(edge.target);
            ++report.edges;
            return {};
        };

        auto terminator_valid = std::visit(
            [&](const auto& terminator) -> Result<void> {
                using Terminator = std::decay_t<decltype(terminator)>;
                if constexpr (std::is_same_v<Terminator, std::monostate>) {
                    return ir_error(block.id, "block has no terminator");
                } else if constexpr (std::is_same_v<Terminator, JumpTerminator>) {
                    return add_edge(terminator.edge);
                } else if constexpr (std::is_same_v<Terminator, BranchTerminator>) {
                    if (auto result = require_domain(available, terminator.condition,
                                                     ValueDomain::Boolean, "branch condition");
                        !result) {
                        return result;
                    }
                    if (auto result = add_edge(terminator.when_true); !result) {
                        return result;
                    }
                    return add_edge(terminator.when_false);
                } else {
                    return require_domain(available, terminator.value, ValueDomain::Integer,
                                          "return value");
                }
            },
            block.terminator);
        if (!terminator_valid) {
            return terminator_valid.error();
        }
    }

    if (!std::all_of(seen_arguments.begin(), seen_arguments.end(),
                     [](bool seen) { return seen; })) {
        return ir_error(function.entry, "not every function argument is materialized");
    }

    std::unordered_set<BlockId> reached;
    std::queue<BlockId> work;
    reached.emplace(function.entry);
    work.push(function.entry);
    while (!work.empty()) {
        const BlockId block = work.front();
        work.pop();
        for (BlockId successor : successors[block]) {
            if (reached.emplace(successor).second) {
                work.push(successor);
            }
        }
    }
    if (reached.size() != function.blocks.size()) {
        return ir_error(function.entry, "MachineIR contains an unreachable block");
    }
    return report;
}

} // namespace xqvm
