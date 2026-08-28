#include "xqvm/compiler.hpp"
#include "xqvm/disassembler.hpp"
#include "xqvm/examples.hpp"
#include "xqvm/machine_ir.hpp"
#include "xqvm/module.hpp"
#include "xqvm/physical.hpp"
#include "xqvm/primitive.hpp"
#include "xqvm/profile.hpp"
#include "xqvm/runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

static_assert(!std::is_default_constructible_v<xqvm::PrimitiveEncoding>);
static_assert(!std::is_default_constructible_v<xqvm::EncodingProfile>);

int failures = 0;

class GroupedNumberPunct final : public std::numpunct<char> {
  protected:
    [[nodiscard]] char do_thousands_sep() const override {
        return '_';
    }

    [[nodiscard]] std::string do_grouping() const override {
        return "\3";
    }
};

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';    \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

[[nodiscard]] std::uint32_t crc32_for_test(std::span<const xqvm::Byte> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (xqvm::Byte byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

void write_u32_for_test(std::vector<xqvm::Byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        bytes[offset + shift / 8U] = static_cast<xqvm::Byte>(value >> shift);
}

void write_u64_for_test(std::vector<xqvm::Byte>& bytes, std::size_t offset, xqvm::Word value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U)
        bytes[offset + shift / 8U] = static_cast<xqvm::Byte>(value >> shift);
}

[[nodiscard]] xqvm::MachineFunction memory_roundtrip_ir() {
    xqvm::MachineFunction function;
    function.name = "memory-roundtrip";
    function.entry = 0;

    xqvm::MachineBlock writer;
    writer.id = 0;
    writer.origin = 100;
    writer.label = "writer";
    writer.actions = {
        xqvm::ConstantAction{
            xqvm::IrValue{1, xqvm::ValueDomain::Address, 101, "address"},
            24,
        },
        xqvm::ConstantAction{
            xqvm::IrValue{2, xqvm::ValueDomain::Integer, 102, "payload"},
            0x1122334455667788ULL,
        },
        xqvm::StoreAction{103, "store", 1, 2, 8},
    };
    writer.terminator = xqvm::JumpTerminator{xqvm::BlockEdge{1, {}}};

    xqvm::MachineBlock reader;
    reader.id = 1;
    reader.origin = 104;
    reader.label = "reader";
    reader.actions = {
        xqvm::ConstantAction{
            xqvm::IrValue{3, xqvm::ValueDomain::Address, 105, "address.again"},
            24,
        },
        xqvm::LoadAction{
            xqvm::IrValue{4, xqvm::ValueDomain::Integer, 106, "loaded"},
            3,
            8,
        },
    };
    reader.terminator = xqvm::ReturnTerminator{4};
    function.blocks = {std::move(writer), std::move(reader)};
    return function;
}

[[nodiscard]] xqvm::MachineFunction all_schema_ir() {
    using namespace xqvm;

    MachineFunction function;
    function.name = "all-schema-contract";
    function.entry = 0;
    function.argument_domains = {ValueDomain::Integer};

    MachineBlock entry;
    entry.id = 0;
    entry.origin = 1000;
    entry.label = "entry";
    entry.actions = {
        ArgumentAction{IrValue{1, ValueDomain::Integer, 1001, "input"}, 0},
        ConstantAction{IrValue{2, ValueDomain::Integer, 1002, "mask"}, 0x13579BDF2468ACE0ULL},
        ConstantAction{IrValue{3, ValueDomain::Integer, 1003, "shift"}, 7},
        ConstantAction{IrValue{4, ValueDomain::Address, 1004, "word.address"}, 0},
        ConstantAction{IrValue{5, ValueDomain::Address, 1005, "byte.address"}, 8},
        IntegerBinaryAction{IrValue{6, ValueDomain::Integer, 1006, "add"},
                            IntegerBinaryOperator::Add, 1, 2},
        IntegerBinaryAction{IrValue{7, ValueDomain::Integer, 1007, "sub"},
                            IntegerBinaryOperator::Subtract, 6, 2},
        IntegerBinaryAction{IrValue{8, ValueDomain::Integer, 1008, "mul"},
                            IntegerBinaryOperator::Multiply, 7, 3},
        IntegerBinaryAction{IrValue{9, ValueDomain::Integer, 1009, "xor"},
                            IntegerBinaryOperator::Xor, 8, 2},
        IntegerBinaryAction{IrValue{10, ValueDomain::Integer, 1010, "and"},
                            IntegerBinaryOperator::And, 9, 2},
        IntegerBinaryAction{IrValue{11, ValueDomain::Integer, 1011, "or"},
                            IntegerBinaryOperator::Or, 10, 3},
        IntegerBinaryAction{IrValue{12, ValueDomain::Integer, 1012, "shl"},
                            IntegerBinaryOperator::ShiftLeft, 11, 3},
        IntegerBinaryAction{IrValue{13, ValueDomain::Integer, 1013, "shr"},
                            IntegerBinaryOperator::ShiftRightLogical, 12, 3},
        IntegerBinaryAction{IrValue{14, ValueDomain::Integer, 1014, "sar"},
                            IntegerBinaryOperator::ShiftRightArithmetic, 9, 3},
        IntegerBinaryAction{IrValue{15, ValueDomain::Integer, 1015, "rol"},
                            IntegerBinaryOperator::RotateLeft, 14, 3},
        IntegerBinaryAction{IrValue{16, ValueDomain::Integer, 1016, "ror"},
                            IntegerBinaryOperator::RotateRight, 15, 3},
        IntegerUnaryAction{IrValue{17, ValueDomain::Integer, 1017, "not"},
                           IntegerUnaryOperator::BitwiseNot, 16},
        IntegerUnaryAction{IrValue{18, ValueDomain::Integer, 1018, "neg"},
                           IntegerUnaryOperator::Negate, 17},
        CompareAction{IrValue{19, ValueDomain::Boolean, 1019, "self.equal"},
                      CompareCondition::Equal, 18, 18},
        StoreAction{1020, "store.word", 4, 18, 8},
        StoreAction{1021, "store.byte", 5, 11, 1},
    };
    entry.terminator = JumpTerminator{BlockEdge{1, {18, 19}}};

    MachineBlock reader;
    reader.id = 1;
    reader.origin = 1100;
    reader.label = "reader";
    reader.parameters = {
        IrValue{20, ValueDomain::Integer, 1101, "expected.word"},
        IrValue{21, ValueDomain::Boolean, 1102, "continue"},
    };
    reader.actions = {
        ConstantAction{IrValue{22, ValueDomain::Address, 1103, "word.address.again"}, 0},
        ConstantAction{IrValue{23, ValueDomain::Address, 1104, "byte.address.again"}, 8},
        LoadAction{IrValue{24, ValueDomain::Integer, 1105, "loaded.word"}, 22, 8},
        LoadAction{IrValue{25, ValueDomain::Integer, 1106, "loaded.byte"}, 23, 1},
        IntegerBinaryAction{IrValue{26, ValueDomain::Integer, 1107, "combined"},
                            IntegerBinaryOperator::Xor, 24, 25},
    };
    reader.terminator = BranchTerminator{
        21,
        BlockEdge{2, {26}},
        BlockEdge{3, {}},
    };

    MachineBlock success;
    success.id = 2;
    success.origin = 1200;
    success.label = "success";
    success.parameters = {IrValue{27, ValueDomain::Integer, 1201, "result"}};
    success.terminator = ReturnTerminator{27};

    MachineBlock failure;
    failure.id = 3;
    failure.origin = 1300;
    failure.label = "failure";
    failure.actions = {
        ConstantAction{IrValue{28, ValueDomain::Integer, 1301, "zero"}, 0},
    };
    failure.terminator = ReturnTerminator{28};

    function.blocks = {
        std::move(entry),
        std::move(reader),
        std::move(success),
        std::move(failure),
    };
    return function;
}

[[nodiscard]] xqvm::MachineFunction boolean_argument_ir() {
    using namespace xqvm;

    MachineFunction function;
    function.name = "boolean-argument";
    function.entry = 0;
    function.argument_domains = {ValueDomain::Boolean};

    MachineBlock entry;
    entry.id = 0;
    entry.origin = 1400;
    entry.label = "entry";
    entry.actions = {
        ArgumentAction{IrValue{1, ValueDomain::Boolean, 1401, "condition"}, 0},
    };
    entry.terminator = BranchTerminator{1, BlockEdge{1, {}}, BlockEdge{2, {}}};

    MachineBlock when_true;
    when_true.id = 1;
    when_true.origin = 1410;
    when_true.label = "true";
    when_true.actions = {
        ConstantAction{IrValue{2, ValueDomain::Integer, 1411, "true.value"}, 11},
    };
    when_true.terminator = ReturnTerminator{2};

    MachineBlock when_false;
    when_false.id = 2;
    when_false.origin = 1420;
    when_false.label = "false";
    when_false.actions = {
        ConstantAction{IrValue{3, ValueDomain::Integer, 1421, "false.value"}, 22},
    };
    when_false.terminator = ReturnTerminator{3};

    function.blocks = {std::move(entry), std::move(when_true), std::move(when_false)};
    return function;
}

[[nodiscard]] xqvm::MachineFunction binary_operation_ir(xqvm::IntegerBinaryOperator operation) {
    using namespace xqvm;

    MachineFunction function;
    function.name = "binary-boundary";
    function.entry = 0;
    function.argument_domains = {ValueDomain::Integer, ValueDomain::Integer};

    MachineBlock entry;
    entry.id = 0;
    entry.origin = 1500;
    entry.label = "entry";
    entry.actions = {
        ArgumentAction{IrValue{1, ValueDomain::Integer, 1501, "left"}, 0},
        ArgumentAction{IrValue{2, ValueDomain::Integer, 1502, "right"}, 1},
        IntegerBinaryAction{IrValue{3, ValueDomain::Integer, 1503, "result"}, operation, 1, 2},
    };
    entry.terminator = ReturnTerminator{3};
    function.blocks = {std::move(entry)};
    return function;
}

[[nodiscard]] xqvm::MachineFunction comparison_ir(xqvm::CompareCondition condition) {
    using namespace xqvm;

    MachineFunction function;
    function.name = "comparison-boundary";
    function.entry = 0;
    function.argument_domains = {ValueDomain::Integer, ValueDomain::Integer};

    MachineBlock entry;
    entry.id = 0;
    entry.origin = 1600;
    entry.label = "entry";
    entry.actions = {
        ArgumentAction{IrValue{1, ValueDomain::Integer, 1601, "left"}, 0},
        ArgumentAction{IrValue{2, ValueDomain::Integer, 1602, "right"}, 1},
        CompareAction{IrValue{3, ValueDomain::Boolean, 1603, "condition"}, condition, 1, 2},
    };
    entry.terminator = BranchTerminator{3, BlockEdge{1, {}}, BlockEdge{2, {}}};

    MachineBlock when_true;
    when_true.id = 1;
    when_true.origin = 1610;
    when_true.label = "true";
    when_true.actions = {
        ConstantAction{IrValue{4, ValueDomain::Integer, 1611, "one"}, 1},
    };
    when_true.terminator = ReturnTerminator{4};

    MachineBlock when_false;
    when_false.id = 2;
    when_false.origin = 1620;
    when_false.label = "false";
    when_false.actions = {
        ConstantAction{IrValue{5, ValueDomain::Integer, 1621, "zero"}, 0},
    };
    when_false.terminator = ReturnTerminator{5};

    function.blocks = {std::move(entry), std::move(when_true), std::move(when_false)};
    return function;
}

[[nodiscard]] xqvm::Word arithmetic_shift_right(xqvm::Word value, unsigned amount) {
    if (amount == 0)
        return value;
    xqvm::Word result = value >> amount;
    if ((value >> 63U) != 0)
        result |= ~xqvm::Word{0} << (64U - amount);
    return result;
}

[[nodiscard]] xqvm::Word binary_reference(xqvm::IntegerBinaryOperator operation, xqvm::Word left,
                                          xqvm::Word right) {
    const unsigned amount = static_cast<unsigned>(right & 63U);
    switch (operation) {
    case xqvm::IntegerBinaryOperator::Add:
        return left + right;
    case xqvm::IntegerBinaryOperator::Subtract:
        return left - right;
    case xqvm::IntegerBinaryOperator::Multiply:
        return left * right;
    case xqvm::IntegerBinaryOperator::Xor:
        return left ^ right;
    case xqvm::IntegerBinaryOperator::And:
        return left & right;
    case xqvm::IntegerBinaryOperator::Or:
        return left | right;
    case xqvm::IntegerBinaryOperator::ShiftLeft:
        return left << amount;
    case xqvm::IntegerBinaryOperator::ShiftRightLogical:
        return left >> amount;
    case xqvm::IntegerBinaryOperator::ShiftRightArithmetic:
        return arithmetic_shift_right(left, amount);
    case xqvm::IntegerBinaryOperator::RotateLeft:
        return std::rotl(left, static_cast<int>(amount));
    case xqvm::IntegerBinaryOperator::RotateRight:
        return std::rotr(left, static_cast<int>(amount));
    }
    return 0;
}

[[nodiscard]] bool comparison_reference(xqvm::CompareCondition condition, xqvm::Word left,
                                        xqvm::Word right) {
    switch (condition) {
    case xqvm::CompareCondition::Equal:
        return left == right;
    case xqvm::CompareCondition::NotEqual:
        return left != right;
    case xqvm::CompareCondition::UnsignedLess:
        return left < right;
    case xqvm::CompareCondition::UnsignedLessEqual:
        return left <= right;
    case xqvm::CompareCondition::SignedLess:
        return std::bit_cast<std::int64_t>(left) < std::bit_cast<std::int64_t>(right);
    case xqvm::CompareCondition::SignedLessEqual:
        return std::bit_cast<std::int64_t>(left) <= std::bit_cast<std::int64_t>(right);
    }
    return false;
}

[[nodiscard]] xqvm::Word all_schema_reference(xqvm::Word input) {
    constexpr xqvm::Word mask = 0x13579BDF2468ACE0ULL;
    constexpr unsigned shift = 7;
    const xqvm::Word added = input + mask;
    const xqvm::Word subtracted = added - mask;
    const xqvm::Word multiplied = subtracted * shift;
    const xqvm::Word xored = multiplied ^ mask;
    const xqvm::Word anded = xored & mask;
    const xqvm::Word ored = anded | shift;
    const xqvm::Word shifted_left = ored << shift;
    const xqvm::Word shifted_right = shifted_left >> shift;
    static_cast<void>(shifted_right);
    const xqvm::Word shifted_arithmetic = arithmetic_shift_right(xored, shift);
    const xqvm::Word rotated = std::rotr(std::rotl(shifted_arithmetic, shift), shift);
    const xqvm::Word negated = xqvm::Word{0} - ~rotated;
    return negated ^ (ored & 0xFFU);
}

[[nodiscard]] xqvm::MachineFunction constant_result_ir() {
    using namespace xqvm;
    MachineFunction function;
    function.name = "constant-with-history";
    function.entry = 0;
    function.argument_domains = {ValueDomain::Integer};
    MachineBlock block;
    block.id = 0;
    block.origin = 2000;
    block.label = "entry";
    block.actions = {
        ArgumentAction{IrValue{1, ValueDomain::Integer, 2001, "input"}, 0},
        IntegerBinaryAction{IrValue{2, ValueDomain::Integer, 2002, "cancelled"},
                            IntegerBinaryOperator::Xor, 1, 1},
        ConstantAction{IrValue{3, ValueDomain::Integer, 2003, "forty.two"}, 42},
        IntegerBinaryAction{IrValue{4, ValueDomain::Integer, 2004, "result"},
                            IntegerBinaryOperator::Add, 2, 3},
    };
    block.terminator = ReturnTerminator{4};
    function.blocks = {std::move(block)};
    return function;
}

[[nodiscard]] xqvm::MachineFunction equal_result_different_path_ir() {
    using namespace xqvm;

    MachineFunction function;
    function.name = "equal-result-different-path";
    function.entry = 0;
    function.argument_domains = {ValueDomain::Integer};

    MachineBlock entry;
    entry.id = 0;
    entry.origin = 3000;
    entry.label = "entry";
    entry.actions = {
        ArgumentAction{IrValue{1, ValueDomain::Integer, 3001, "input"}, 0},
        ConstantAction{IrValue{2, ValueDomain::Integer, 3002, "zero"}, 0},
        CompareAction{IrValue{3, ValueDomain::Boolean, 3003, "is.zero"}, CompareCondition::Equal, 1,
                      2},
    };
    entry.terminator = BranchTerminator{3, BlockEdge{1, {}}, BlockEdge{2, {}}};

    MachineBlock left;
    left.id = 1;
    left.origin = 3100;
    left.label = "left";
    left.actions = {
        ConstantAction{IrValue{4, ValueDomain::Integer, 3101, "left.answer"}, 42},
    };
    left.terminator = JumpTerminator{BlockEdge{3, {4}}};

    MachineBlock right;
    right.id = 2;
    right.origin = 3200;
    right.label = "right";
    right.actions = {
        ConstantAction{IrValue{5, ValueDomain::Integer, 3201, "right.answer"}, 42},
    };
    right.terminator = JumpTerminator{BlockEdge{3, {5}}};

    MachineBlock merge;
    merge.id = 3;
    merge.origin = 3300;
    merge.label = "merge";
    merge.parameters = {IrValue{6, ValueDomain::Integer, 3301, "answer"}};
    merge.terminator = ReturnTerminator{6};

    function.blocks = {
        std::move(entry),
        std::move(left),
        std::move(right),
        std::move(merge),
    };
    return function;
}

void test_family_algebra() {
    CHECK(xqvm::kPrimitiveSchemaCount == 20);
    const xqvm::AffineMap outer{0xF1357AEA2E62A9C5ULL, 0x123456789ABCDEF0ULL};
    const xqvm::AffineMap inner{0x9E3779B97F4A7C15ULL, 0x0FEDCBA987654321ULL};
    for (xqvm::Word value : std::array<xqvm::Word, 6>{
             0,
             1,
             2,
             0x7FFFFFFFFFFFFFFFULL,
             0x8000000000000000ULL,
             0xFFFFFFFFFFFFFFFFULL,
         }) {
        CHECK(outer.inverse().apply(outer.apply(value)) == value);
        CHECK(outer.compose(inner).apply(value) == outer.apply(inner.apply(value)));
        for (xqvm::ShareFamily family : {
                 xqvm::ShareFamily::Xor,
                 xqvm::ShareFamily::Additive,
             }) {
            const auto shares = xqvm::split_value(family, value, xqvm::mix_word(value, 1, 2),
                                                  xqvm::mix_word(value, 3, 4));
            CHECK(xqvm::join_shares(family, shares) == value);
        }
    }

    xqvm::PhysicalValue physical;
    physical.domain = xqvm::ValueDomain::Integer;
    physical.family = xqvm::ShareFamily::Additive;
    physical.origin = 7;
    physical.shares = {
        xqvm::PhysicalShare{0, xqvm::AffineMap{3, 10}},
        xqvm::PhysicalShare{1, xqvm::AffineMap{5, 20}},
        xqvm::PhysicalShare{2, xqvm::AffineMap{7, 30}},
    };
    std::array<xqvm::Word, 3> carriers{};
    auto written = xqvm::write_physical_value(
        physical, 0xDEADBEEFCAFEBABEULL, 0x1111111111111111ULL, 0x2222222222222222ULL, carriers);
    CHECK(written.has_value());
    auto read = xqvm::read_physical_value(physical, carriers);
    CHECK(read.has_value());
    if (read)
        CHECK(read.value() == 0xDEADBEEFCAFEBABEULL);

    auto malformed = physical;
    malformed.shares[1].carrier = malformed.shares[0].carrier;
    const std::array<xqvm::Word, 3> before{11, 22, 33};
    auto untouched = before;
    CHECK(!xqvm::write_physical_value(malformed, 42, 1, 2, untouched).has_value());
    CHECK(untouched == before);
    malformed = physical;
    malformed.shares[2].storage.multiplier = 2;
    untouched = before;
    CHECK(!xqvm::write_physical_value(malformed, 42, 1, 2, untouched).has_value());
    CHECK(untouched == before);
}

void test_ir_contracts() {
    auto factorial = xqvm::build_factorial_ir();
    auto verified = xqvm::verify_machine_ir(factorial);
    CHECK(verified.has_value());
    if (verified) {
        CHECK(verified.value().blocks == 4);
        CHECK(verified.value().edges == 4);
    }

    auto invalid = factorial;
    auto& body_action = std::get<xqvm::IntegerBinaryAction>(invalid.blocks[2].actions[0]);
    body_action.left = 4;
    CHECK(!xqvm::verify_machine_ir(invalid).has_value());

    auto invalid_binary = factorial;
    std::get<xqvm::IntegerBinaryAction>(invalid_binary.blocks[2].actions[0]).operation =
        static_cast<xqvm::IntegerBinaryOperator>(0xFFU); // NOLINT
    CHECK(!xqvm::verify_machine_ir(invalid_binary).has_value());

    auto invalid_compare = factorial;
    std::get<xqvm::CompareAction>(invalid_compare.blocks[1].actions[1]).condition =
        static_cast<xqvm::CompareCondition>(0xFFU); // NOLINT
    CHECK(!xqvm::verify_machine_ir(invalid_compare).has_value());

    auto invalid_text = factorial;
    invalid_text.name = std::string("\xC0\xAF", 2);
    CHECK(!xqvm::verify_machine_ir(invalid_text).has_value());

    auto missing_argument = factorial;
    missing_argument.argument_domains.push_back(xqvm::ValueDomain::Integer);
    CHECK(!xqvm::verify_machine_ir(missing_argument).has_value());

    auto duplicate_origin = factorial;
    std::get<xqvm::ArgumentAction>(duplicate_origin.blocks[0].actions[0]).output.origin =
        duplicate_origin.blocks[0].origin;
    CHECK(!xqvm::verify_machine_ir(duplicate_origin).has_value());

    auto edge_domain_mismatch = factorial;
    edge_domain_mismatch.blocks[1].parameters[0].domain = xqvm::ValueDomain::Boolean;
    CHECK(!xqvm::verify_machine_ir(edge_domain_mismatch).has_value());

    auto unreachable = factorial;
    xqvm::MachineBlock dead;
    dead.id = 99;
    dead.origin = 99;
    dead.label = "dead";
    dead.actions = {
        xqvm::ConstantAction{xqvm::IrValue{99, xqvm::ValueDomain::Integer, 98, "dead.value"}, 0},
    };
    dead.terminator = xqvm::ReturnTerminator{99};
    unreachable.blocks.push_back(std::move(dead));
    CHECK(!xqvm::verify_machine_ir(unreachable).has_value());
}

void test_end_to_end() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x13579BDF2468ACE0ULL);
    auto compiled = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;

    auto verified = xqvm::verify_physical_module(compiled.value().module, profile);
    CHECK(verified.has_value());
    if (verified) {
        CHECK(verified.value().islands == 8);
        CHECK(verified.value().primitive_nodes > 100);
        CHECK(verified.value().carrier_writes > 40);
    }
    auto disassembly = xqvm::disassemble_module(compiled.value().module, profile);
    CHECK(disassembly.has_value());
    if (disassembly) {
        CHECK(disassembly.value().find("delay c") != std::string::npos);
        CHECK(disassembly.value().find("commit") != std::string::npos);
    }

    auto machine = xqvm::Machine::create(compiled.value().module, profile);
    CHECK(machine.has_value());
    if (!machine)
        return;
    xqvm::PrimitivePlanLimits tiny_limits;
    tiny_limits.max_nodes = 1;
    CHECK(!xqvm::Machine::create(compiled.value().module, profile, tiny_limits).has_value());
    xqvm::RunOptions tiny_run;
    tiny_run.plan_limits = tiny_limits;
    const xqvm::Word tiny_input = 2;
    auto limited = machine.value().run(std::span<const xqvm::Word>(&tiny_input, 1), tiny_run);
    CHECK(!limited.ok());
    if (limited.fault)
        CHECK(limited.fault->code == xqvm::RuntimeFaultCode::ResourceLimit);

    auto missing_argument = machine.value().run();
    CHECK(!missing_argument.ok());
    if (missing_argument.fault) {
        CHECK(missing_argument.fault->code == xqvm::RuntimeFaultCode::ArgumentWindow);
    }
    const std::array<xqvm::Word, 2> excess_arguments{1, 2};
    auto excess = machine.value().run(excess_arguments);
    CHECK(!excess.ok());
    if (excess.fault)
        CHECK(excess.fault->code == xqvm::RuntimeFaultCode::ArgumentWindow);

    xqvm::Word expected = 1;
    for (xqvm::Word input = 0; input <= 12; ++input) {
        if (input > 1)
            expected *= input;
        const xqvm::RunResult result = machine.value().run(std::span<const xqvm::Word>(&input, 1));
        CHECK(result.ok());
        if (result.ok())
            CHECK(*result.value == expected);
    }

    std::vector<xqvm::IslandTraceEvent> events;
    xqvm::RunOptions traced_options;
    traced_options.trace = [&](const xqvm::IslandTraceEvent& event) { events.push_back(event); };
    const xqvm::Word traced_input = 5;
    const auto traced =
        machine.value().run(std::span<const xqvm::Word>(&traced_input, 1), traced_options);
    CHECK(traced.ok());
    CHECK(events.size() == 21);
    for (std::size_t index = 0; index < events.size(); ++index) {
        CHECK(events[index].ordinal == index + 1U);
        CHECK(events[index].primitive_nodes != 0);
    }

    const auto wrong_profile = xqvm::EncodingProfile::from_seed(0x13579BDF2468ACE1ULL);
    CHECK(!xqvm::Machine::create(compiled.value().module, wrong_profile).has_value());

    auto serialized = xqvm::serialize_module(compiled.value().module);
    CHECK(serialized.has_value());
    if (serialized) {
        const std::string bytes(serialized.value().begin(), serialized.value().end());
        CHECK(bytes.find("factorial") == std::string::npos);
        CHECK(bytes.find("accumulator") == std::string::npos);
        auto parsed = xqvm::parse_module(serialized.value());
        CHECK(parsed.has_value());
        if (parsed)
            CHECK(parsed.value().argument_count == 1);
        auto damaged = serialized.value();
        damaged.back() ^= 0x80U;
        CHECK(!xqvm::parse_module(damaged).has_value());
    }
    CHECK(!compiled.value().sidecar.records.empty());
    CHECK(std::any_of(
        compiled.value().sidecar.records.begin(), compiled.value().sidecar.records.end(),
        [](const xqvm::SidecarRecord& record) { return record.label == "next.accumulator"; }));
    const auto island_record = std::find_if(
        compiled.value().sidecar.records.begin(), compiled.value().sidecar.records.end(),
        [](const xqvm::SidecarRecord& record) { return record.kind == "physical-island"; });
    CHECK(island_record != compiled.value().sidecar.records.end());
    if (island_record != compiled.value().sidecar.records.end()) {
        CHECK(island_record->physical_values.size() == 2);
        if (island_record->physical_values.size() == 2) {
            CHECK(island_record->physical_values[0].role == "block.semantic");
            CHECK(island_record->physical_values[1].role == "block.history");
            CHECK(island_record->physical_values[0].domain == xqvm::ValueDomain::Semantic);
            CHECK(island_record->physical_values[1].domain == xqvm::ValueDomain::History);
            CHECK(island_record->physical_values[0].origin !=
                  island_record->physical_values[1].origin);
            for (const auto& value : island_record->physical_values) {
                CHECK(value.shares.size() == 3);
                CHECK((value.shares[0].storage.multiplier & 1U) == 1U);
                CHECK((value.shares[1].storage.multiplier & 1U) == 1U);
                CHECK((value.shares[2].storage.multiplier & 1U) == 1U);
                CHECK(value.shares[0].carrier != value.shares[1].carrier);
                CHECK(value.shares[0].carrier != value.shares[2].carrier);
                CHECK(value.shares[1].carrier != value.shares[2].carrier);
            }
        }
    }
    const auto edge_record = std::find_if(
        compiled.value().sidecar.records.begin(), compiled.value().sidecar.records.end(),
        [](const xqvm::SidecarRecord& record) { return record.kind == "edge-island"; });
    CHECK(edge_record != compiled.value().sidecar.records.end());
    if (edge_record != compiled.value().sidecar.records.end()) {
        CHECK(edge_record->physical_values.size() == 4);
        if (edge_record->physical_values.size() == 4) {
            CHECK(edge_record->physical_values[0].role == "source.semantic");
            CHECK(edge_record->physical_values[1].role == "source.history");
            CHECK(edge_record->physical_values[2].role == "target.semantic");
            CHECK(edge_record->physical_values[3].role == "target.history");
        }
    }
}

void test_compilation_reproducibility() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x1122334455667788ULL);
    auto first = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
    auto second = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
    CHECK(first.has_value());
    CHECK(second.has_value());
    if (!first || !second)
        return;

    auto first_bytes = xqvm::serialize_module(first.value().module);
    auto second_bytes = xqvm::serialize_module(second.value().module);
    CHECK(first_bytes.has_value());
    CHECK(second_bytes.has_value());
    if (first_bytes && second_bytes)
        CHECK(first_bytes.value() == second_bytes.value());

    const auto& first_sidecar = first.value().sidecar;
    const auto& second_sidecar = second.value().sidecar;
    CHECK(first_sidecar.function == second_sidecar.function);
    CHECK(first_sidecar.profile_fingerprint == second_sidecar.profile_fingerprint);
    CHECK(first_sidecar.records.size() == second_sidecar.records.size());
    if (first_sidecar.records.size() != second_sidecar.records.size())
        return;

    for (std::size_t index = 0; index < first_sidecar.records.size(); ++index) {
        const auto& left = first_sidecar.records[index];
        const auto& right = second_sidecar.records[index];
        CHECK(left.origin == right.origin);
        CHECK(left.kind == right.kind);
        CHECK(left.label == right.label);
        CHECK(left.island_offset == right.island_offset);
        CHECK(left.continuation.lane == right.continuation.lane);
        CHECK(left.continuation.proof == right.continuation.proof);
        CHECK(left.physical_values.size() == right.physical_values.size());
        if (left.physical_values.size() != right.physical_values.size())
            continue;
        for (std::size_t value_index = 0; value_index < left.physical_values.size();
             ++value_index) {
            const auto& left_value = left.physical_values[value_index];
            const auto& right_value = right.physical_values[value_index];
            CHECK(left_value.role == right_value.role);
            CHECK(left_value.origin == right_value.origin);
            CHECK(left_value.domain == right_value.domain);
            CHECK(left_value.family == right_value.family);
            CHECK(left_value.shares == right_value.shares);
        }
    }
}

void test_sidecar_file_contract() {
    const auto profile = xqvm::EncodingProfile::from_seed(0xA11CE5EED1234567ULL);
    auto compiled = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;

    const auto path = std::filesystem::current_path() / "xqvm-sidecar-v3-test.json";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const std::locale previous_locale = std::locale();
    std::locale::global(std::locale(previous_locale, new GroupedNumberPunct));
    auto saved = xqvm::save_origin_sidecar(compiled.value().sidecar, path);
    std::locale::global(previous_locale);
    CHECK(saved.has_value());
    if (!saved)
        return;

    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    const std::string json{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(json.find("\"format\": \"xqvm-private-origin-v3\"") != std::string::npos);
    CHECK(json.find("\"physical_values\": [") != std::string::npos);
    CHECK(json.find("\"shares\": [") != std::string::npos);
    CHECK(json.find("\"storage\": {\"multiplier\": \"0x") != std::string::npos);
    CHECK(json.find("\", \"addend\": \"0x") != std::string::npos);
    CHECK(json.find("\"role\": \"block.semantic\"") != std::string::npos);
    CHECK(json.find("\"role\": \"source.history\"") != std::string::npos);
    CHECK(json.find("\"profile_fingerprint\": \"0x") != std::string::npos);
    CHECK(json.find("\"continuation\": {\"lane\": \"0x") != std::string::npos);
    bool grouped_number = false;
    for (std::size_t index = 1; index + 1U < json.size(); ++index) {
        const auto decimal = [](char value) { return value >= '0' && value <= '9'; };
        if (json[index] == '_' && decimal(json[index - 1U]) && decimal(json[index + 1U])) {
            grouped_number = true;
            break;
        }
    }
    CHECK(!grouped_number);
    CHECK(json.find('\r') == std::string::npos);

    input.close();
    CHECK(std::filesystem::remove(path, ignored));
    CHECK(!ignored);
}

void test_artifact_file_roundtrip() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x46494C4552545450ULL);
    auto compiled = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;

    const auto directory = std::filesystem::current_path();
    const auto module_path = directory / "xqvm-module-roundtrip.xqvm";
    const auto profile_path = directory / "xqvm-profile-roundtrip.xqprofile";
    std::error_code ignored;
    std::filesystem::remove(module_path, ignored);
    ignored.clear();
    std::filesystem::remove(profile_path, ignored);

    CHECK(xqvm::save_module(compiled.value().module, module_path).has_value());
    CHECK(xqvm::save_profile(profile, profile_path).has_value());
    auto loaded_module = xqvm::load_module(module_path);
    auto loaded_profile = xqvm::load_profile(profile_path);
    CHECK(loaded_module.has_value());
    CHECK(loaded_profile.has_value());
    if (loaded_module && loaded_profile) {
        auto original_bytes = xqvm::serialize_module(compiled.value().module);
        auto loaded_bytes = xqvm::serialize_module(loaded_module.value());
        CHECK(original_bytes.has_value());
        CHECK(loaded_bytes.has_value());
        if (original_bytes && loaded_bytes)
            CHECK(original_bytes.value() == loaded_bytes.value());
        CHECK(loaded_profile.value().seed() == profile.seed());
        CHECK(loaded_profile.value().fingerprint() == profile.fingerprint());
    }

    ignored.clear();
    CHECK(std::filesystem::remove(module_path, ignored));
    CHECK(!ignored);
    ignored.clear();
    CHECK(std::filesystem::remove(profile_path, ignored));
    CHECK(!ignored);
}

void test_all_physical_schemas_and_profile_diversity() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x0DDBA11C0FFEE123ULL);
    std::array<bool, 256> used_codes{};
    for (std::size_t index = 0; index < xqvm::kPrimitiveSchemaCount; ++index) {
        const auto kind = static_cast<xqvm::PrimitiveKind>(index);
        const xqvm::Byte code = profile.primitives().encode(kind);
        CHECK(!used_codes[code]);
        used_codes[code] = true;
        auto decoded = profile.primitives().decode(code);
        CHECK(decoded.has_value());
        if (decoded)
            CHECK(decoded.value() == kind);
    }
    const auto unused = std::find(used_codes.begin(), used_codes.end(), false);
    CHECK(unused != used_codes.end());
    if (unused != used_codes.end()) {
        CHECK(!profile.primitives()
                   .decode(static_cast<xqvm::Byte>(std::distance(used_codes.begin(), unused)))
                   .has_value());
    }

    auto compiled = xqvm::compile_machine_ir(all_schema_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;
    auto disassembly = xqvm::disassemble_module(compiled.value().module, profile);
    CHECK(disassembly.has_value());
    if (disassembly) {
        for (std::string_view name : xqvm::kPrimitiveNames) {
            CHECK(disassembly.value().find("= " + std::string(name)) != std::string::npos);
        }
    }

    auto machine = xqvm::Machine::create(compiled.value().module, profile);
    CHECK(machine.has_value());
    if (!machine)
        return;
    for (xqvm::Word input : std::array<xqvm::Word, 6>{
             0,
             1,
             0x1020304050607080ULL,
             0x7FFFFFFFFFFFFFFFULL,
             0x8000000000000000ULL,
             0xFFFFFFFFFFFFFFFFULL,
         }) {
        auto result = machine.value().run(std::span<const xqvm::Word>(&input, 1));
        CHECK(result.ok());
        if (result.ok())
            CHECK(*result.value == all_schema_reference(input));
    }

    const auto second_profile = xqvm::EncodingProfile::from_seed(0x0DDBA11C0FFEE124ULL);
    auto second = xqvm::compile_machine_ir(all_schema_ir(), second_profile);
    CHECK(second.has_value());
    if (!second)
        return;
    CHECK(compiled.value().module.code != second.value().module.code);
    CHECK(compiled.value().module.initial_carriers != second.value().module.initial_carriers);
    const xqvm::Word input = 0xAABBCCDDEEFF0011ULL;
    auto second_machine = xqvm::Machine::create(second.value().module, second_profile);
    CHECK(second_machine.has_value());
    if (second_machine) {
        const auto result = second_machine.value().run(std::span<const xqvm::Word>(&input, 1));
        CHECK(result.ok());
        if (result.ok())
            CHECK(*result.value == all_schema_reference(input));
    }
}

void test_boolean_argument_canonicalization() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x424F4F4C45414E31ULL);
    auto compiled = xqvm::compile_machine_ir(boolean_argument_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;
    auto machine = xqvm::Machine::create(compiled.value().module, profile);
    CHECK(machine.has_value());
    if (!machine)
        return;

    for (const auto& [input, expected] : std::array<std::pair<xqvm::Word, xqvm::Word>, 4>{
             std::pair{xqvm::Word{0}, xqvm::Word{22}},
             std::pair{xqvm::Word{1}, xqvm::Word{11}},
             std::pair{xqvm::Word{2}, xqvm::Word{11}},
             std::pair{~xqvm::Word{0}, xqvm::Word{11}},
         }) {
        const auto result = machine.value().run(std::span<const xqvm::Word>(&input, 1));
        CHECK(result.ok());
        if (result.ok())
            CHECK(*result.value == expected);
    }
}

void test_operator_boundary_matrix() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x4F50455241544F52ULL);
    constexpr std::array<std::pair<xqvm::Word, xqvm::Word>, 10> inputs{
        std::pair{xqvm::Word{0}, xqvm::Word{0}},
        std::pair{xqvm::Word{1}, xqvm::Word{1}},
        std::pair{xqvm::Word{0x7FFFFFFFFFFFFFFFULL}, xqvm::Word{1}},
        std::pair{xqvm::Word{0x8000000000000000ULL}, xqvm::Word{1}},
        std::pair{xqvm::Word{0xFEDCBA9876543210ULL}, xqvm::Word{0}},
        std::pair{xqvm::Word{0xFEDCBA9876543210ULL}, xqvm::Word{31}},
        std::pair{xqvm::Word{0xFEDCBA9876543210ULL}, xqvm::Word{63}},
        std::pair{xqvm::Word{0xFEDCBA9876543210ULL}, xqvm::Word{64}},
        std::pair{xqvm::Word{0xFEDCBA9876543210ULL}, xqvm::Word{65}},
        std::pair{xqvm::Word{0xFFFFFFFFFFFFFFFFULL}, xqvm::Word{127}},
    };
    for (std::size_t index = 0;
         index <= static_cast<std::size_t>(xqvm::IntegerBinaryOperator::RotateRight); ++index) {
        const auto operation = static_cast<xqvm::IntegerBinaryOperator>(index);
        auto compiled = xqvm::compile_machine_ir(binary_operation_ir(operation), profile);
        CHECK(compiled.has_value());
        if (!compiled)
            continue;
        auto machine = xqvm::Machine::create(std::move(compiled.value().module), profile);
        CHECK(machine.has_value());
        if (!machine)
            continue;
        for (const auto& [left, right] : inputs) {
            const std::array<xqvm::Word, 2> arguments{left, right};
            const auto result = machine.value().run(arguments);
            CHECK(result.ok());
            if (result.ok())
                CHECK(*result.value == binary_reference(operation, left, right));
        }
    }

    constexpr std::array<std::pair<xqvm::Word, xqvm::Word>, 8> comparisons{
        std::pair{xqvm::Word{0}, xqvm::Word{0}},
        std::pair{xqvm::Word{0}, xqvm::Word{1}},
        std::pair{xqvm::Word{1}, xqvm::Word{0}},
        std::pair{xqvm::Word{0x7FFFFFFFFFFFFFFFULL}, xqvm::Word{0x8000000000000000ULL}},
        std::pair{xqvm::Word{0x8000000000000000ULL}, xqvm::Word{0x7FFFFFFFFFFFFFFFULL}},
        std::pair{xqvm::Word{0xFFFFFFFFFFFFFFFFULL}, xqvm::Word{0}},
        std::pair{xqvm::Word{0}, xqvm::Word{0xFFFFFFFFFFFFFFFFULL}},
        std::pair{xqvm::Word{0xFFFFFFFFFFFFFFFFULL}, xqvm::Word{0xFFFFFFFFFFFFFFFFULL}},
    };
    for (std::size_t index = 0;
         index <= static_cast<std::size_t>(xqvm::CompareCondition::SignedLessEqual); ++index) {
        const auto condition = static_cast<xqvm::CompareCondition>(index);
        auto compiled = xqvm::compile_machine_ir(comparison_ir(condition), profile);
        CHECK(compiled.has_value());
        if (!compiled)
            continue;
        auto machine = xqvm::Machine::create(std::move(compiled.value().module), profile);
        CHECK(machine.has_value());
        if (!machine)
            continue;
        for (const auto& [left, right] : comparisons) {
            const std::array<xqvm::Word, 2> arguments{left, right};
            const auto result = machine.value().run(arguments);
            CHECK(result.ok());
            if (result.ok())
                CHECK(*result.value ==
                      static_cast<xqvm::Word>(comparison_reference(condition, left, right)));
        }
    }
}

void test_origin_history_changes_physical_representation() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x51DEC4A77E57BEEFULL);
    auto compiled = xqvm::compile_machine_ir(constant_result_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;
    auto machine = xqvm::Machine::create(compiled.value().module, profile);
    CHECK(machine.has_value());
    if (!machine)
        return;

    const xqvm::Word first_input = 7;
    const xqvm::Word second_input = 9;
    auto first = machine.value().run(std::span<const xqvm::Word>(&first_input, 1));
    auto second = machine.value().run(std::span<const xqvm::Word>(&second_input, 1));
    CHECK(first.ok());
    CHECK(second.ok());
    if (!first.ok() || !second.ok())
        return;
    CHECK(*first.value == 42);
    CHECK(*second.value == 42);
    const auto return_begin = static_cast<std::size_t>(xqvm::kReturnValueCarrier);
    CHECK(!std::equal(first.final_state.carriers.begin() + return_begin,
                      first.final_state.carriers.begin() + return_begin + 3U,
                      second.final_state.carriers.begin() + return_begin));
}

void test_origin_ids_change_physical_layout() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x4F524947494E4D58ULL);
    auto baseline_ir = xqvm::build_factorial_ir();
    auto changed_ir = baseline_ir;
    std::get<xqvm::ArgumentAction>(changed_ir.blocks[0].actions[0]).output.origin = 5;

    auto baseline = xqvm::compile_machine_ir(baseline_ir, profile);
    auto changed = xqvm::compile_machine_ir(changed_ir, profile);
    CHECK(baseline.has_value());
    CHECK(changed.has_value());
    if (!baseline || !changed)
        return;
    CHECK(baseline.value().module.code != changed.value().module.code);
    CHECK(baseline.value().module.initial_carriers != changed.value().module.initial_carriers);
    CHECK(std::any_of(changed.value().sidecar.records.begin(),
                      changed.value().sidecar.records.end(), [](const xqvm::SidecarRecord& record) {
                          return record.origin == 5 && record.label == "n";
                      }));

    auto baseline_machine = xqvm::Machine::create(baseline.value().module, profile);
    auto changed_machine = xqvm::Machine::create(changed.value().module, profile);
    CHECK(baseline_machine.has_value());
    CHECK(changed_machine.has_value());
    if (!baseline_machine || !changed_machine)
        return;
    const xqvm::Word input = 6;
    const auto baseline_result =
        baseline_machine.value().run(std::span<const xqvm::Word>(&input, 1));
    const auto changed_result = changed_machine.value().run(std::span<const xqvm::Word>(&input, 1));
    CHECK(baseline_result.ok());
    CHECK(changed_result.ok());
    if (baseline_result.ok() && changed_result.ok()) {
        CHECK(*baseline_result.value == 720);
        CHECK(*changed_result.value == 720);
    }
}

void test_edge_history_reaches_target_block() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x4544474548495354ULL);
    auto compiled = xqvm::compile_machine_ir(equal_result_different_path_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;
    auto machine = xqvm::Machine::create(compiled.value().module, profile);
    CHECK(machine.has_value());
    if (!machine)
        return;

    const xqvm::Word left_input = 0;
    const xqvm::Word right_input = 1;
    auto left = machine.value().run(std::span<const xqvm::Word>(&left_input, 1));
    auto right = machine.value().run(std::span<const xqvm::Word>(&right_input, 1));
    CHECK(left.ok());
    CHECK(right.ok());
    if (!left.ok() || !right.ok())
        return;
    CHECK(*left.value == 42);
    CHECK(*right.value == 42);
    const auto begin = static_cast<std::size_t>(xqvm::kReturnValueCarrier);
    CHECK(!std::equal(left.final_state.carriers.begin() + begin,
                      left.final_state.carriers.begin() + begin + 3U,
                      right.final_state.carriers.begin() + begin));
}

void test_seed_and_input_matrix() {
    xqvm::Word seed = 0x4D41545249585345ULL;
    for (std::uint32_t profile_index = 0; profile_index < 32; ++profile_index) {
        seed = xqvm::mix_word(seed, profile_index, 0x454544434F4E5452ULL);
        const auto profile = xqvm::EncodingProfile::from_seed(seed);
        auto compiled = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
        CHECK(compiled.has_value());
        if (!compiled)
            continue;
        auto serialized = xqvm::serialize_module(compiled.value().module);
        CHECK(serialized.has_value());
        if (!serialized)
            continue;
        auto parsed = xqvm::parse_module(serialized.value());
        CHECK(parsed.has_value());
        if (!parsed)
            continue;
        auto machine = xqvm::Machine::create(std::move(parsed.value()), profile);
        CHECK(machine.has_value());
        if (!machine)
            continue;
        xqvm::Word expected = 1;
        for (xqvm::Word input = 0; input <= 12; ++input) {
            if (input > 1)
                expected *= input;
            auto result = machine.value().run(std::span<const xqvm::Word>(&input, 1));
            CHECK(result.ok());
            if (result.ok())
                CHECK(*result.value == expected);
        }
    }
}

void test_profile_format_contract() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x50524F46494C4531ULL);
    auto serialized = xqvm::serialize_profile(profile);
    CHECK(serialized.has_value());
    if (!serialized)
        return;

    CHECK(serialized.value().size() == 32);
    auto parsed = xqvm::parse_profile(serialized.value());
    CHECK(parsed.has_value());
    if (parsed)
        CHECK(parsed.value().fingerprint() == profile.fingerprint());

    auto reserved = serialized.value();
    reserved[28] = 1;
    auto reserved_result = xqvm::parse_profile(reserved);
    CHECK(!reserved_result.has_value());
    if (!reserved_result) {
        CHECK(reserved_result.error().code == xqvm::ErrorCode::InvalidModule);
        CHECK(reserved_result.error().offset == 28);
    }

    auto damaged = serialized.value();
    damaged[8] ^= 0x80U;
    auto checksum_result = xqvm::parse_profile(damaged);
    CHECK(!checksum_result.has_value());
    if (!checksum_result)
        CHECK(checksum_result.error().code == xqvm::ErrorCode::ChecksumMismatch);

    auto bad_magic = serialized.value();
    bad_magic[0] = 'Y';
    CHECK(!xqvm::parse_profile(bad_magic).has_value());
    CHECK(!xqvm::parse_profile(std::span<const xqvm::Byte>(serialized.value()).first(31))
               .has_value());

    auto wrong_fingerprint = serialized.value();
    const auto other = xqvm::EncodingProfile::from_seed(0x50524F46494C4532ULL);
    write_u64_for_test(wrong_fingerprint, 16, other.fingerprint());
    write_u32_for_test(wrong_fingerprint, 24, 0);
    write_u32_for_test(wrong_fingerprint, 24, crc32_for_test(wrong_fingerprint));
    auto fingerprint_result = xqvm::parse_profile(wrong_fingerprint);
    CHECK(!fingerprint_result.has_value());
    if (!fingerprint_result) {
        CHECK(fingerprint_result.error().code == xqvm::ErrorCode::InvalidModule);
        CHECK(fingerprint_result.error().offset == 16);
    }
}

void test_module_format_contract() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x4D4F44554C453031ULL);
    auto compiled = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;
    auto serialized = xqvm::serialize_module(compiled.value().module);
    CHECK(serialized.has_value());
    if (!serialized)
        return;
    CHECK(xqvm::parse_module(serialized.value()).has_value());

    auto reseal = [](std::vector<xqvm::Byte>& bytes) {
        write_u32_for_test(bytes, 40, 0);
        write_u32_for_test(bytes, 40, crc32_for_test(bytes));
    };

    auto flags = serialized.value();
    flags[8] = 1;
    CHECK(!xqvm::parse_module(flags).has_value());

    auto reserved = serialized.value();
    reserved[46] = 1;
    CHECK(!xqvm::parse_module(reserved).has_value());

    auto wrong_code_size = serialized.value();
    write_u32_for_test(wrong_code_size, 20,
                       static_cast<std::uint32_t>(compiled.value().module.code.size() + 1U));
    reseal(wrong_code_size);
    CHECK(!xqvm::parse_module(wrong_code_size).has_value());

    auto wrong_carrier_bytes = serialized.value();
    write_u32_for_test(wrong_carrier_bytes, 28,
                       static_cast<std::uint32_t>(compiled.value().module.initial_carriers.size() *
                                                      sizeof(xqvm::Word) -
                                                  sizeof(xqvm::Word)));
    reseal(wrong_carrier_bytes);
    CHECK(!xqvm::parse_module(wrong_carrier_bytes).has_value());

    auto excess_arguments = serialized.value();
    excess_arguments[44] = static_cast<xqvm::Byte>(xqvm::kMaximumArguments + 1U);
    excess_arguments[45] = 0;
    reseal(excess_arguments);
    CHECK(!xqvm::parse_module(excess_arguments).has_value());

    CHECK(!xqvm::parse_module(
               std::span<const xqvm::Byte>(serialized.value()).first(xqvm::kModuleHeaderSize - 1U))
               .has_value());
}

void test_memory_and_delayed_commit() {
    const auto profile = xqvm::EncodingProfile::from_seed(0xA5A5A5A55A5A5A5AULL);
    auto compiled = xqvm::compile_machine_ir(memory_roundtrip_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;
    auto machine = xqvm::Machine::create(compiled.value().module, profile);
    CHECK(machine.has_value());
    if (!machine)
        return;
    auto result = machine.value().run();
    CHECK(result.ok());
    if (result.ok())
        CHECK(*result.value == 0x1122334455667788ULL);

    xqvm::PrimitivePlan failing_plan;
    const xqvm::Continuation entry = profile.continuations().seal(0);
    failing_plan.nodes = {
        xqvm::LiteralPrimitive{0xAABBCCDDEEFF0011ULL},
        xqvm::LiteralPrimitive{64},
        xqvm::LiteralPrimitive{entry.lane},
        xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{entry.proof},
        xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{0},
    };
    failing_plan.carrier_writes = {
        {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4, 6}, {5, 7}, {32, 0},
    };
    failing_plan.memory_writes = {{8, 1, 0}};
    auto encoded = xqvm::encode_primitive_plan(failing_plan, profile, 0);
    CHECK(encoded.has_value());
    if (!encoded)
        return;

    xqvm::Module module;
    module.memory_size = 64;
    module.carrier_count = 33;
    module.profile_fingerprint = profile.fingerprint();
    module.code = std::move(encoded.value());
    module.initial_carriers.assign(33, 0);
    module.initial_carriers[32] = 0x55;
    module.initial_carriers[xqvm::kContinuationLaneCarrier] = entry.lane;
    module.initial_carriers[xqvm::kContinuationProofCarrier] = entry.proof;
    auto failing_machine = xqvm::Machine::create(std::move(module), profile);
    CHECK(failing_machine.has_value());
    if (!failing_machine)
        return;
    auto failed = failing_machine.value().run();
    CHECK(!failed.ok());
    CHECK(failed.final_state.carriers[32] == 0x55);
    CHECK(std::all_of(failed.final_state.memory.begin(), failed.final_state.memory.end(),
                      [](xqvm::Byte byte) { return byte == 0; }));

    xqvm::PrimitivePlan read_out_of_bounds;
    read_out_of_bounds.nodes = {
        xqvm::LiteralPrimitive{60},         xqvm::ReadMemory64Primitive{0},
        xqvm::LiteralPrimitive{entry.lane}, xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{0},          xqvm::LiteralPrimitive{entry.proof},
        xqvm::LiteralPrimitive{0},          xqvm::LiteralPrimitive{0},
    };
    read_out_of_bounds.carrier_writes = {
        {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4, 6}, {5, 7},
    };
    auto read_bytes = xqvm::encode_primitive_plan(read_out_of_bounds, profile, 0);
    CHECK(read_bytes.has_value());
    if (!read_bytes)
        return;
    xqvm::Module read_module;
    read_module.memory_size = 64;
    read_module.carrier_count = xqvm::kFirstAllocatedCarrier;
    read_module.profile_fingerprint = profile.fingerprint();
    read_module.code = std::move(read_bytes.value());
    read_module.initial_carriers.assign(read_module.carrier_count, 0);
    read_module.initial_carriers[xqvm::kContinuationLaneCarrier] = entry.lane;
    read_module.initial_carriers[xqvm::kContinuationProofCarrier] = entry.proof;
    auto read_machine = xqvm::Machine::create(read_module, profile);
    CHECK(read_machine.has_value());
    if (!read_machine)
        return;
    const auto read_failure = read_machine.value().run();
    CHECK(!read_failure.ok());
    CHECK(read_failure.fault.has_value());
    if (read_failure.fault)
        CHECK(read_failure.fault->code == xqvm::RuntimeFaultCode::MemoryBounds);
    CHECK(read_failure.final_state.carriers == read_module.initial_carriers);
    CHECK(std::all_of(read_failure.final_state.memory.begin(),
                      read_failure.final_state.memory.end(),
                      [](xqvm::Byte byte) { return byte == 0; }));
}

void test_continuation_tamper() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x0123456789ABCDEFULL);
    auto compiled = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;
    compiled.value().module.initial_carriers[xqvm::kContinuationProofCarrier + 1U] ^= xqvm::Word{1}
                                                                                      << 63U;
    CHECK(!xqvm::Machine::create(std::move(compiled.value().module), profile).has_value());

    const xqvm::Continuation entry = profile.continuations().seal(0);
    xqvm::Continuation invalid_next = entry;
    invalid_next.proof ^= xqvm::Word{1} << 63U;
    xqvm::PrimitivePlan plan;
    plan.nodes = {
        xqvm::LiteralPrimitive{invalid_next.lane},
        xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{invalid_next.proof},
        xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{0},
    };
    plan.carrier_writes = {
        {0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5},
    };
    auto encoded = xqvm::encode_primitive_plan(plan, profile, 0);
    CHECK(encoded.has_value());
    if (!encoded)
        return;
    xqvm::Module module;
    module.memory_size = 64;
    module.carrier_count = xqvm::kFirstAllocatedCarrier;
    module.profile_fingerprint = profile.fingerprint();
    module.code = std::move(encoded.value());
    module.initial_carriers.assign(module.carrier_count, 0);
    module.initial_carriers[xqvm::kContinuationLaneCarrier] = entry.lane;
    module.initial_carriers[xqvm::kContinuationProofCarrier] = entry.proof;
    auto machine = xqvm::Machine::create(std::move(module), profile);
    CHECK(machine.has_value());
    if (!machine)
        return;
    auto result = machine.value().run();
    CHECK(!result.ok());
    if (result.fault) {
        CHECK(result.fault->code == xqvm::RuntimeFaultCode::ContinuationProof);
        CHECK(result.final_state.islands_executed == 1);
    }
}

void test_continuation_requires_verified_boundary() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x424F554E44415259ULL);
    const xqvm::Continuation halt = profile.continuations().seal(xqvm::kHaltContinuation);

    xqvm::PrimitivePlan first;
    first.nodes = {
        xqvm::LiteralPrimitive{0}, xqvm::LiteralPrimitive{0}, xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{0}, xqvm::LiteralPrimitive{0}, xqvm::LiteralPrimitive{0},
    };
    first.carrier_writes = {
        {0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5},
    };

    xqvm::PrimitivePlan second;
    second.nodes = {
        xqvm::LiteralPrimitive{halt.lane},  xqvm::LiteralPrimitive{0}, xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{halt.proof}, xqvm::LiteralPrimitive{0}, xqvm::LiteralPrimitive{0},
    };
    second.carrier_writes = {
        {0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5},
    };

    const auto second_offset = static_cast<std::uint32_t>(xqvm::primitive_plan_size(first));
    const std::uint32_t interior_offset = second_offset + 1U;
    const xqvm::Continuation interior = profile.continuations().seal(interior_offset);
    std::get<xqvm::LiteralPrimitive>(first.nodes[0]).value = interior.lane;
    std::get<xqvm::LiteralPrimitive>(first.nodes[3]).value = interior.proof;

    auto first_bytes = xqvm::encode_primitive_plan(first, profile, 0);
    auto second_bytes = xqvm::encode_primitive_plan(second, profile, second_offset);
    CHECK(first_bytes.has_value());
    CHECK(second_bytes.has_value());
    if (!first_bytes || !second_bytes)
        return;

    xqvm::Module module;
    module.memory_size = 64;
    module.carrier_count = xqvm::kFirstAllocatedCarrier;
    module.profile_fingerprint = profile.fingerprint();
    module.code = std::move(first_bytes.value());
    module.code.insert(module.code.end(), second_bytes.value().begin(), second_bytes.value().end());
    module.initial_carriers.assign(module.carrier_count, 0);
    const xqvm::Continuation entry = profile.continuations().seal(0);
    module.initial_carriers[xqvm::kContinuationLaneCarrier] = entry.lane;
    module.initial_carriers[xqvm::kContinuationProofCarrier] = entry.proof;

    auto machine = xqvm::Machine::create(std::move(module), profile);
    CHECK(machine.has_value());
    if (!machine)
        return;
    auto result = machine.value().run();
    CHECK(!result.ok());
    CHECK(result.fault.has_value());
    if (result.fault) {
        CHECK(result.fault->code == xqvm::RuntimeFaultCode::IslandBounds);
        CHECK(result.fault->location == interior_offset);
        CHECK(result.fault->message.find("verified island boundary") != std::string::npos);
        CHECK(result.final_state.islands_executed == 1);
    }
}

void test_physical_stream_rejections() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x53545245414D4E47ULL);
    const xqvm::Continuation entry = profile.continuations().seal(0);
    const xqvm::Continuation halt = profile.continuations().seal(xqvm::kHaltContinuation);

    xqvm::PrimitivePlan base;
    base.nodes = {
        xqvm::LiteralPrimitive{halt.lane},  xqvm::LiteralPrimitive{0}, xqvm::LiteralPrimitive{0},
        xqvm::LiteralPrimitive{halt.proof}, xqvm::LiteralPrimitive{0}, xqvm::LiteralPrimitive{0},
    };
    base.carrier_writes = {
        {0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5},
    };

    auto make_module = [&](const xqvm::PrimitivePlan& plan) -> xqvm::Result<xqvm::Module> {
        auto encoded = xqvm::encode_primitive_plan(plan, profile, 0);
        if (!encoded)
            return encoded.error();
        xqvm::Module module;
        module.memory_size = 64;
        module.carrier_count = xqvm::kFirstAllocatedCarrier;
        module.profile_fingerprint = profile.fingerprint();
        module.code = std::move(encoded.value());
        module.initial_carriers.assign(module.carrier_count, 0);
        module.initial_carriers[xqvm::kContinuationLaneCarrier] = entry.lane;
        module.initial_carriers[xqvm::kContinuationProofCarrier] = entry.proof;
        return module;
    };

    auto valid = make_module(base);
    CHECK(valid.has_value());
    if (!valid)
        return;
    auto valid_machine = xqvm::Machine::create(valid.value(), profile);
    CHECK(valid_machine.has_value());
    if (valid_machine) {
        const auto result = valid_machine.value().run();
        CHECK(result.ok());
        if (result.ok())
            CHECK(*result.value == 0);

        xqvm::RunOptions exact_limit;
        exact_limit.max_islands = 1;
        const auto exact = valid_machine.value().run({}, exact_limit);
        CHECK(exact.ok());
        if (exact.ok())
            CHECK(exact.final_state.islands_executed == 1);

        xqvm::RunOptions zero_limit;
        zero_limit.max_islands = 0;
        const auto zero = valid_machine.value().run({}, zero_limit);
        CHECK(!zero.ok());
        if (zero.fault)
            CHECK(zero.fault->code == xqvm::RuntimeFaultCode::ResourceLimit);
    }

    auto missing_continuation = base;
    missing_continuation.carrier_writes.pop_back();
    auto missing_module = make_module(missing_continuation);
    CHECK(missing_module.has_value());
    if (missing_module)
        CHECK(!xqvm::verify_physical_module(missing_module.value(), profile).has_value());

    auto duplicate_target = base;
    duplicate_target.carrier_writes.push_back({0, 0});
    auto duplicate_module = make_module(duplicate_target);
    CHECK(duplicate_module.has_value());
    if (duplicate_module)
        CHECK(!xqvm::verify_physical_module(duplicate_module.value(), profile).has_value());

    auto abi_write = base;
    abi_write.carrier_writes.push_back({xqvm::kArgumentCarrierBase, 0});
    auto abi_module = make_module(abi_write);
    CHECK(abi_module.has_value());
    if (abi_module)
        CHECK(!xqvm::verify_physical_module(abi_module.value(), profile).has_value());

    auto unknown = valid.value();
    std::array<bool, 256> encoded_codes{};
    for (std::size_t index = 0; index < xqvm::kPrimitiveSchemaCount; ++index) {
        encoded_codes[profile.primitives().encode(static_cast<xqvm::PrimitiveKind>(index))] = true;
    }
    const auto unused = std::find(encoded_codes.begin(), encoded_codes.end(), false);
    CHECK(unused != encoded_codes.end());
    if (unused != encoded_codes.end()) {
        const auto plain = static_cast<xqvm::Byte>(std::distance(encoded_codes.begin(), unused));
        unknown.code[12] = profile.mask_code_byte(plain, 12);
        CHECK(!xqvm::verify_physical_module(unknown, profile).has_value());
    }

    auto truncated = valid.value();
    truncated.code.pop_back();
    CHECK(!xqvm::verify_physical_module(truncated, profile).has_value());

    auto overlapping = base;
    overlapping.nodes.push_back(xqvm::LiteralPrimitive{0});
    overlapping.nodes.push_back(xqvm::LiteralPrimitive{0x1122334455667788ULL});
    overlapping.memory_writes = {{8, 6, 7}, {1, 6, 7}};
    auto overlapping_module = make_module(overlapping);
    CHECK(overlapping_module.has_value());
    if (!overlapping_module)
        return;
    auto overlapping_machine = xqvm::Machine::create(overlapping_module.value(), profile);
    CHECK(overlapping_machine.has_value());
    if (!overlapping_machine)
        return;
    const auto overlap_result = overlapping_machine.value().run();
    CHECK(!overlap_result.ok());
    if (overlap_result.fault)
        CHECK(overlap_result.fault->code == xqvm::RuntimeFaultCode::WriteConflict);
    CHECK(overlap_result.final_state.carriers == overlapping_module.value().initial_carriers);
    CHECK(std::all_of(overlap_result.final_state.memory.begin(),
                      overlap_result.final_state.memory.end(),
                      [](xqvm::Byte byte) { return byte == 0; }));
}

void test_plan_rejects_forward_edges() {
    xqvm::PrimitivePlan plan;
    plan.nodes = {xqvm::AddPrimitive{0, 0}};
    CHECK(!xqvm::verify_primitive_plan(plan, 32).has_value());

    xqvm::PrimitivePlan invalid_compare;
    invalid_compare.nodes = {
        xqvm::LiteralPrimitive{1},
        xqvm::LiteralPrimitive{2},
        xqvm::ComparePrimitive{
            static_cast<xqvm::CompareCondition>(0xFFU), // NOLINT
            0,
            1,
        },
    };
    CHECK(!xqvm::verify_primitive_plan(invalid_compare, 32).has_value());
}

void test_module_resource_limits() {
    const auto profile = xqvm::EncodingProfile::from_seed(0x5245534F55524345ULL);
    auto compiled = xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile);
    CHECK(compiled.has_value());
    if (!compiled)
        return;

    auto oversized = compiled.value().module;
    oversized.memory_size = xqvm::ModuleLimits{}.max_memory_size + 1U;
    CHECK(!xqvm::validate_module(oversized).has_value());
    CHECK(!xqvm::serialize_module(oversized).has_value());
    CHECK(!xqvm::verify_physical_module(oversized, profile).has_value());
    CHECK(!xqvm::disassemble_module(oversized, profile).has_value());
    CHECK(!xqvm::Machine::create(oversized, profile).has_value());

    xqvm::ModuleLimits extended;
    extended.max_memory_size = oversized.memory_size;
    CHECK(xqvm::validate_module(oversized, extended).has_value());
    CHECK(xqvm::serialize_module(oversized, extended).has_value());

    xqvm::CompileOptions options;
    options.memory_size = xqvm::ModuleLimits{}.max_memory_size + 1U;
    CHECK(!xqvm::compile_machine_ir(xqvm::build_factorial_ir(), profile, options).has_value());
}

} // namespace

int run_tests() {
    test_family_algebra();
    test_ir_contracts();
    test_end_to_end();
    test_compilation_reproducibility();
    test_sidecar_file_contract();
    test_artifact_file_roundtrip();
    test_all_physical_schemas_and_profile_diversity();
    test_boolean_argument_canonicalization();
    test_operator_boundary_matrix();
    test_origin_history_changes_physical_representation();
    test_origin_ids_change_physical_layout();
    test_edge_history_reaches_target_block();
    test_seed_and_input_matrix();
    test_profile_format_contract();
    test_module_format_contract();
    test_memory_and_delayed_commit();
    test_continuation_tamper();
    test_continuation_requires_verified_boundary();
    test_physical_stream_rejections();
    test_plan_rejects_forward_edges();
    test_module_resource_limits();
    if (failures != 0) {
        std::cerr << failures << " test checks failed\n";
        return 1;
    }
    std::cout << "all xqvm contract tests passed\n";
    return 0;
}

int main() noexcept {
    try {
        return run_tests();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "unexpected test exception: %s\n", exception.what());
        return 1;
    } catch (...) {
        std::fputs("unexpected non-standard test exception\n", stderr);
        return 1;
    }
}
