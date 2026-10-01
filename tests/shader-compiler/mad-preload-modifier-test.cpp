// Unary source modifiers currently become MOVs before MAD legalization.
// Inject at that boundary to exercise the cached second read itself, without
// adding a test hook or changing access in the shipping compiler.
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
struct LegalizeTag {
    using Type = void (GeneralBuilder::*)();
    friend Type member(LegalizeTag);
};
// Explicit template arguments are exempt from member access checking.
template struct Access<ProgramTag, &GeneralBuilder::program_>;
template struct Access<LegalizeTag, &GeneralBuilder::legalizeInputOperands>;

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string kind = argv[1];
    if (kind != "neg" && kind != "abs") return 2;
    IRFunction entry("main");
    IRModule module;
    GeneralBuilder builder(GeneralProfile::Fragment, entry, module);
    auto& program = builder.*member(ProgramTag{});
    VInstr mad;
    mad.op = VOp::Mad;
    mad.dst.index = 100;
    mad.dst.writemask = 7;
    mad.srcs[0] = inputSrc(NVFX_FP_OP_INPUT_SRC_TC(2));
    mad.srcs[1] = mad.srcs[0];
    mad.srcs[1].swizzle = {3, 3, 3, 3};
    mad.srcs[1].neg = kind == "neg";
    mad.srcs[1].abs = kind == "abs";
    mad.srcs[2].kind = VSrcKind::Literal;
    mad.srcs[2].literal = {0.125f, 0.25f, 0.5f, 0.f};
    mad.srcs[2].literalLanes = 3;
    program.instrs.push_back(mad);
    (builder.*member(LegalizeTag{}))();
    if (program.loweringFailed || program.instrs.size() != 2) return 3;
    const auto& load = program.instrs[0];
    const auto& result = program.instrs[1];
    if (load.op != VOp::Mov || load.dst.writemask != 15 ||
        load.srcs[0].neg || load.srcs[0].abs ||
        load.srcs[0].swizzle != std::array<uint8_t, 4>{0, 1, 2, 3} ||
        result.op != VOp::Mad || result.srcs[0].kind != VSrcKind::Temp ||
        result.srcs[1].kind != VSrcKind::Temp ||
        result.srcs[0].index != load.dst.index ||
        result.srcs[1].index != load.dst.index ||
        result.srcs[1].swizzle != mad.srcs[1].swizzle ||
        result.srcs[1].neg != mad.srcs[1].neg ||
        result.srcs[1].abs != mad.srcs[1].abs) return 4;
    VInstr alpha;
    alpha.op = VOp::Mov;
    alpha.dst.index = 100;
    alpha.dst.writemask = 8;
    alpha.srcs[0].kind = VSrcKind::Literal;
    alpha.srcs[0].literal = {1.f, 1.f, 1.f, 1.f};
    program.instrs.push_back(alpha);
    for (auto& vi : program.instrs) {
        vi.dst.phys = vi.dst.index == 100 ? 0 : vi.dst.index + 1;
        for (auto& src : vi.srcs)
            if (src.kind == VSrcKind::Temp)
                src.phys = src.index == 100 ? 0 : src.index + 1;
    }
    const auto output = emitFragmentVirtual(program, entry, nullptr);
    if (!output.ok || output.words.empty()) return 5;
    // Already halfword-swapped words, serialized by the Python checker.
    for (auto word : output.words) std::cout << word << '\n';
}
