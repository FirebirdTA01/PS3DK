// Exercise the real emitter with deliberately invalid final allocations.
// Including its implementation keeps test access out of the shipping API.
// Unused lowering/VP functions are discarded by the test link.
#include <iostream>
#include "../../tools/rsx-cg-compiler/src/nv40/nv40_general_lowering.cpp"

int main()
{
    using namespace nv40::detail;
    int failures = 0;
    const auto run = [&](const char* name, int dst, int condition, int thenValue,
                         bool halfCondition, bool expectOk,
                         const char* detail, bool halfDestination = false) {
        VirtualProgram program;
        VInstr select;
        select.op = VOp::SelPred;
        select.dst.index = 79;
        select.dst.phys = dst;
        select.dst.fp16 = halfDestination;
        select.srcs[0] = tempSrc(11);
        select.srcs[0].phys = condition;
        select.srcs[0].fp16 = halfCondition;
        select.srcs[1] = tempSrc(25);
        select.srcs[1].phys = thenValue;
        select.srcs[2] = tempSrc(30);
        select.srcs[2].phys = 8; // else may alias dst: read and write are atomic.
        program.instrs.push_back(select);
        VInstr store;
        store.dst.output = true;
        store.dst.index = 0;
        store.srcs[0] = tempSrc(79);
        store.srcs[0].phys = dst;
        program.instrs.push_back(store);
        IRFunction entry("main");
        const auto output = emitFragmentVirtual(program, entry, nullptr);
        std::string diagnostic;
        for (const auto& message : output.diagnostics) diagnostic += message + "\n";
        const bool correct = expectOk
            ? output.ok && !output.words.empty()
            : !output.ok && output.words.empty() &&
              diagnostic.find("t_3603033d") != std::string::npos &&
              diagnostic.find("SelPred alloc[0]") != std::string::npos &&
              diagnostic.find(detail) != std::string::npos;
        if (!correct) {
            std::cerr << "FAIL: " << name << " ok=" << output.ok
                      << " words=" << output.words.size() << " " << diagnostic;
            ++failures;
        }
    };
    run("safe including else alias", 8, 5, 1, false, true, "");
    run("condition alias", 8, 8, 1, false, false, "src0 v11 R8");
    run("then alias", 8, 5, 8, false, false, "src1 v25 R8");
    run("half condition aliases full slot", 8, 17, 1, true, false, "src0 v11 H17");
    run("half destination aliases full source", 17, 8, 1, false, false, "src0 v11 R8", true);
    run("missing destination", -1, 5, 1, false, false, "dst v79 unassigned");
    run("missing source", 8, -1, 1, false, false, "src0 v11 unassigned");
    if (failures) return 1;
    std::cout << "selpred-emission-guard-test: PASS (7 cases)\n";
}
