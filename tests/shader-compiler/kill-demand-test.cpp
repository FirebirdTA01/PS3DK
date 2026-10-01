// Exercise the actual scheduler boundary, including a CC-only dependency.
// Explicit member access avoids adding a hook to the shipping compiler.
#include <iostream>
#include "../../tools/rsx-cg-compiler/src/nv40/nv40_general_lowering.cpp"
using namespace nv40::detail;
template<class Tag, typename Tag::Type Member> struct Access {
    friend typename Tag::Type member(Tag) { return Member; }
};
struct ProgramTag {
    using Type = VirtualProgram GeneralBuilder::*;
    friend Type member(ProgramTag);
};
struct OrderTag {
    using Type = void (GeneralBuilder::*)();
    friend Type member(OrderTag);
};
template struct Access<ProgramTag, &GeneralBuilder::program_>;
template struct Access<OrderTag, &GeneralBuilder::applyOrderingPass>;

int main()
{
    int failed = 0;
    const char* names[] = {"literal", "input", "temp", "CC-temp", "CC-input"};
    for (int kind = 0; kind < 5; ++kind) {
        IRFunction entry("main"); IRModule module;
        GeneralBuilder builder(GeneralProfile::Fragment, entry, module);
        auto& p = builder.*member(ProgramTag{});
        VInstr load;
        load.op = VOp::Mov; load.dst.index = 7;
        load.srcs[0] = inputSrc(NVFX_FP_OP_INPUT_SRC_TC(0));
        VInstr kill;
        kill.op = VOp::Kil; kill.dst.none = true;
        kill.srcs[0].kind = VSrcKind::Literal;
        kill.srcs[0].literal = {1.f, 1.f, 1.f, 1.f};
        kill.srcs[0].literalLanes = 1;
        if (kind == 1) kill.srcs[0] = load.srcs[0];
        if (kind == 2) kill.srcs[0] = tempSrc(7);
        if (kind == 2 || kind == 3) p.instrs.push_back(load);
        if (kind == 3 || kind == 4) {
            VInstr cc;
            cc.op = VOp::Sgt; cc.dst.none = true; cc.ccUpdate = true;
            cc.srcs[0] = kind == 3 ? tempSrc(7) : load.srcs[0];
            cc.srcs[1] = kill.srcs[0];
            p.instrs.push_back(cc);
            kill.srcs = {}; kill.predicate = 5;
        }
        p.instrs.push_back(kill);
        VInstr output;
        output.op = VOp::Mov; output.dst.output = true; output.dst.index = 0;
        output.srcs[0] = inputSrc(NVFX_FP_OP_INPUT_SRC_TC(1));
        p.instrs.push_back(output);
        (builder.*member(OrderTag{}))();
        size_t k = p.instrs.size(), o = k;
        for (size_t i = 0; i < p.instrs.size(); ++i) {
            if (p.instrs[i].op == VOp::Kil) k = i;
            if (p.instrs[i].dst.output) o = i;
        }
        const bool needsTemp = kind == 2 || kind == 3;
        const bool ok = !p.loweringFailed && k < p.instrs.size() &&
                        o < p.instrs.size() && (needsTemp ? k < o : o < k);
        std::cout << (ok ? "PASS " : "FAIL ")
                  << names[kind]
                  << " kill=" << k << " output=" << o << '\n';
        failed += !ok;
    }
    std::cout << "kill demand: 5 tests, " << 5-failed << " pass, " << failed << " fail\n";
    return failed ? 1 : 0;
}
