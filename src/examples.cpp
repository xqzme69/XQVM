#include "xqvm/examples.hpp"

namespace xqvm {

MachineFunction build_factorial_ir() {
    MachineFunction function;
    function.name = "factorial";
    function.entry = 0;
    function.argument_domains = {ValueDomain::Integer};

    MachineBlock entry;
    entry.id = 0;
    entry.origin = 1;
    entry.label = "entry";
    entry.actions = {
        ArgumentAction{IrValue{1, ValueDomain::Integer, 10, "n"}, 0},
        ConstantAction{IrValue{2, ValueDomain::Integer, 11, "one"}, 1},
    };
    entry.terminator = JumpTerminator{BlockEdge{1, {1, 2}}};

    MachineBlock loop;
    loop.id = 1;
    loop.origin = 2;
    loop.label = "loop";
    loop.parameters = {
        IrValue{3, ValueDomain::Integer, 12, "remaining"},
        IrValue{4, ValueDomain::Integer, 13, "accumulator"},
    };
    loop.actions = {
        ConstantAction{IrValue{5, ValueDomain::Integer, 14, "loop.one"}, 1},
        CompareAction{
            IrValue{6, ValueDomain::Boolean, 15, "remaining.le.one"},
            CompareCondition::UnsignedLessEqual,
            3,
            5,
        },
    };
    loop.terminator = BranchTerminator{
        6,
        BlockEdge{3, {4}},
        BlockEdge{2, {3, 4}},
    };

    MachineBlock body;
    body.id = 2;
    body.origin = 3;
    body.label = "body";
    body.parameters = {
        IrValue{7, ValueDomain::Integer, 16, "body.remaining"},
        IrValue{8, ValueDomain::Integer, 17, "body.accumulator"},
    };
    body.actions = {
        IntegerBinaryAction{
            IrValue{9, ValueDomain::Integer, 18, "next.accumulator"},
            IntegerBinaryOperator::Multiply,
            8,
            7,
        },
        ConstantAction{IrValue{10, ValueDomain::Integer, 19, "body.one"}, 1},
        IntegerBinaryAction{
            IrValue{11, ValueDomain::Integer, 20, "next.remaining"},
            IntegerBinaryOperator::Subtract,
            7,
            10,
        },
    };
    body.terminator = JumpTerminator{BlockEdge{1, {11, 9}}};

    MachineBlock exit;
    exit.id = 3;
    exit.origin = 4;
    exit.label = "exit";
    exit.parameters = {
        IrValue{12, ValueDomain::Integer, 21, "result"},
    };
    exit.terminator = ReturnTerminator{12};

    function.blocks = {
        std::move(entry),
        std::move(loop),
        std::move(body),
        std::move(exit),
    };
    return function;
}

} // namespace xqvm
