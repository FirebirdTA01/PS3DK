// Exercise the shipping emitter with invalid logical scalar operand lanes.
// Include the implementation to keep injection out of the compiler's API.
#include <iostream>
#include "../../tools/rsx-cg-compiler/src/nv40/nv40_general_lowering.cpp"

// Keep the same behavioral test buildable against the pre-validator source:
// that parent has no demand metadata and accepts the legal controls anyway.
// A renamed candidate field fails safely: explicit-demand legal controls then
// use the destination mask and fail the validator instead of weakening it.
template<class Instruction>
auto setDemandMask(Instruction& instruction, int mask, int)
    -> decltype(instruction.scalarSourceDemandMask = uint8_t(mask), void())
{
    instruction.scalarSourceDemandMask = static_cast<uint8_t>(mask);
}
template<class Instruction>
void setDemandMask(Instruction&, int, ...) {}

int main()
{
    using namespace nv40::detail;
    int failures = 0;
    int cases = 0;
    const auto run = [&](const char* name, VOp op, int mask,
                         std::array<uint8_t, 4> scalarSwizzle, bool expectOk,
                         int scale = 0, int demandMask = 0) {
        ++cases;
        VirtualProgram program;
        VInstr instruction;
        instruction.op = op;
        instruction.dst.output = true;
        instruction.dst.index = 0;
        instruction.dst.writemask = mask;
        instruction.fpScale = scale;
        setDemandMask(instruction, demandMask, 0);
        instruction.srcs[0] = inputSrc(4);
        const bool division = op == VOp::DivR || op == VOp::DivSqrt;
        if (division) {
            instruction.srcs[1] = tempSrc(3);
            instruction.srcs[1].phys = 3;
        }
        instruction.srcs[division ? 1 : 0].swizzle = scalarSwizzle;
        program.instrs.push_back(instruction);
        IRFunction entry("main");
        const auto output = emitFragmentVirtual(program, entry, nullptr);
        const std::string expected = std::string("nv40-general-fp: ") +
            vOpName(op) + " scalar src" + (division ? "1" : "0") +
            " requests distinct components across result lanes; refusing";
        const bool correct = expectOk
            ? output.ok && !output.words.empty()
            : !output.ok && output.words.empty() &&
              std::find(output.diagnostics.begin(), output.diagnostics.end(), expected) !=
                  output.diagnostics.end();
        if (!correct) {
            std::cerr << "FAIL: " << vOpName(op) << " " << name
                      << " ok=" << output.ok << " words=" << output.words.size() << '\n';
            for (const auto& diagnostic : output.diagnostics)
                std::cerr << diagnostic << '\n';
            ++failures;
        }
    };
    for (VOp op : {VOp::Rcp, VOp::Rsq, VOp::Sin, VOp::Cos, VOp::Lg2,
                   VOp::Ex2, VOp::DivR, VOp::DivSqrt}) {
        run("distinct lanes", op, 15, {0, 1, 2, 3}, false);
        run("legal broadcast", op, 15, {2, 2, 2, 2}, true);
        run("legal single y", op, 2, {3, 3, 3, 3}, true);
        run("unmarked single y requests a different component", op, 2,
            {0, 1, 2, 3}, false);
        run("explicit scalar x result placed in y", op, 2,
            {0, 1, 2, 3}, true, 0, 1);
        run("legal sparse yz", op, 6, {2, 2, 2, 0}, true);
        run("sparse yz reads different component than hardware x", op, 6,
            {0, 2, 2, 0}, false);
    }
    // Vector operations do not have the scalar operand restriction.
    run("vector MOV", VOp::Mov, 15, {0, 1, 2, 3}, true);
    run("scaled vector DIVSQR", VOp::DivSqrt, 7, {2, 2, 2, 2}, true, 2);
    run("scaled invalid denominator", VOp::DivSqrt, 7, {0, 1, 2, 3}, false, 2);
    run("scaled vector MOV", VOp::Mov, 15, {0, 1, 2, 3}, true, 1);
    run("explicit scalar exponent broadcast", VOp::Ex2, 7,
        {0, 1, 2, 3}, true, 0, 1);
    run("explicit preloaded denominator", VOp::DivR, 7,
        {0, 1, 2, 3}, true, 0, 1);
    if (failures) return 1;
    std::cout << "scalar-emission-guard-test: PASS (" << cases << " cases)\n";
}
