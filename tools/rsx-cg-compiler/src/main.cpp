/*
 * rsx-cg-compiler — Phase 8 Cg → RSX (NV40) compiler driver.
 *
 * Stage 2 (in progress): front-end + IR pipeline wired.  Parses through
 * preprocessor + lexer + parser, runs semantic analysis, builds the
 * hardware-agnostic IRModule.  NV40 lowering + ucode emit land next.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ast.h"
#include "parser.h"
#include "preprocessor.h"
#include "semantic.h"
#include "ir.h"
#include "ir_builder.h"
#include "builtin_shader_header_api.h"
#include "nv40/nv40_emit.h"
#include "nv40/nv40_if_convert.h"
#include "compile_options.h"
#include "sony_container_fp.h"
#include "sony_container_vp.h"

namespace
{

// Stage 1 carries its own minimal context.  We deliberately do NOT pull
// in the donor's CompilerContext because compiler_options.h transitively
// drags the USSE back-end types (UsseProgram lives in lowering.h).
// Our NV40-targeted context will live in a separate header in Stage 2.
struct CompilerContext
{
    std::string              inputFile;
    std::string              entryName = "main";
    std::vector<std::string> includeDirs;
    std::string              containerOutPath;  // --emit-container <path>
    bool                     noStdLib = false;

    enum class Profile { Unknown, FragmentRsx, VertexRsx };
    Profile profile = Profile::VertexRsx;

    rsx_cg::CompileOptions   compileOpts;       // --O0..O3 / --fastmath etc.

    // Populated by runPreprocessor — Sony Cg pragma surface that the
    // lexer drops before it reaches the parser.
    std::vector<std::string> alphakillSamplers;
};

void usage()
{
    std::fprintf(stderr,
        "rsx-cg-compiler — Cg → RSX (NV40) compiler\n"
        "\n"
        "Usage: rsx-cg-compiler [options] <shader.cg>\n"
        "\n"
        "Options:\n"
        "  -p, --profile <name>   sce_fp_rsx | sce_vp_rsx (default: sce_vp_rsx)\n"
        "  -e, --entry <name>     Entry function (default: main)\n"
        "  -I <dir>               Add include directory\n"
        "  --no-stdlib            Skip the embedded Cg standard-library header\n"
        "  --emit-container <p>   Write the Sony .vpo/.fpo container to <p> (binary)\n"
        "  --dump-ast             Print the parsed AST to stdout\n"
        "  --dump-ir              Print the generated IR module to stdout\n"
        "  -h, --help             Show this message\n"
        "\n"
        "Optimization (FP only in current sce-cgc; VP insensitive):\n"
        "  -O0 / -O1 / -O2 / -O3  Optimization level (default: -O2)\n"
        "  --fastmath             Enable fast-math optimizations (default)\n"
        "  --nofastmath           Disable fast-math optimizations\n");
}

std::string slurpFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        std::fprintf(stderr, "rsx-cg-compiler: cannot open %s\n", path.c_str());
        std::exit(1);
    }
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::string runPreprocessor(const std::string& sourceCode,
                            CompilerContext& ctx)
{
    std::ostringstream composed;

    if (!ctx.noStdLib)
    {
        const std::string virtName = GetBuiltinShaderHeaderName();
        const std::string virtText = GetBuiltinShaderHeaderSource();
        composed << "#line 1 \"" << virtName << "\"\n" << virtText << "\n";
    }

    composed << "#line 1 \"" << ctx.inputFile << "\"\n" << sourceCode;

    Preprocessor pp;
    for (const auto& dir : ctx.includeDirs)
    {
        pp.addIncludePath(dir);
    }
    std::string out = pp.process(composed.str(), ctx.inputFile);
    ctx.alphakillSamplers = pp.alphakillSamplers();
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    CompilerContext ctx;
    bool dumpAst = false;
    bool dumpIr  = false;

    int i = 1;
    while (i < argc)
    {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            usage();
            return 0;
        }
        else if ((arg == "-p" || arg == "--profile") && i + 1 < argc)
        {
            const std::string p = argv[++i];
            if (p == "sce_fp_rsx" || p == "sce_fp_psp2")
                ctx.profile = CompilerContext::Profile::FragmentRsx;
            else if (p == "sce_vp_rsx" || p == "sce_vp_psp2")
                ctx.profile = CompilerContext::Profile::VertexRsx;
            else
            {
                std::fprintf(stderr, "rsx-cg-compiler: unknown profile '%s'\n", p.c_str());
                return 1;
            }
        }
        else if ((arg == "-e" || arg == "--entry") && i + 1 < argc)
        {
            ctx.entryName = argv[++i];
        }
        else if (arg == "-I" && i + 1 < argc)
        {
            ctx.includeDirs.emplace_back(argv[++i]);
        }
        else if (arg == "--no-stdlib")
        {
            ctx.noStdLib = true;
        }
        else if (arg == "--dump-ast")
        {
            dumpAst = true;
        }
        else if (arg == "--dump-ir")
        {
            dumpIr = true;
        }
        else if (arg == "--emit-container" && i + 1 < argc)
        {
            ctx.containerOutPath = argv[++i];
        }
        else if (arg == "-O0" || arg == "--O0")
        {
            ctx.compileOpts.opt = rsx_cg::OptLevel::O0;
        }
        else if (arg == "-O1" || arg == "--O1")
        {
            ctx.compileOpts.opt = rsx_cg::OptLevel::O1;
        }
        else if (arg == "-O2" || arg == "--O2")
        {
            ctx.compileOpts.opt = rsx_cg::OptLevel::O2;
        }
        else if (arg == "-O3" || arg == "--O3")
        {
            ctx.compileOpts.opt = rsx_cg::OptLevel::O3;
        }
        else if (arg == "--fastmath")
        {
            ctx.compileOpts.fastmath = true;
        }
        else if (arg == "--nofastmath")
        {
            ctx.compileOpts.fastmath = false;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::fprintf(stderr, "rsx-cg-compiler: unknown option '%s'\n", arg.c_str());
            usage();
            return 1;
        }
        else
        {
            if (!ctx.inputFile.empty())
            {
                std::fprintf(stderr, "rsx-cg-compiler: only one input file allowed\n");
                return 1;
            }
            ctx.inputFile = arg;
        }
        ++i;
    }

    if (ctx.inputFile.empty())
    {
        usage();
        return 1;
    }

    const std::string sourceCode   = slurpFile(ctx.inputFile);
    const std::string preprocessed = runPreprocessor(sourceCode, ctx);

    std::vector<ParseError> parseErrors;
    auto ast = parseShaderSource(preprocessed, ctx.inputFile, &parseErrors);

    int errorCount = 0;
    for (const auto& err : parseErrors)
    {
        std::fprintf(stderr, "%s: %s: %s\n",
                     err.loc.toString().c_str(),
                     err.isWarning ? "warning" : "error",
                     err.message.c_str());
        if (!err.isWarning) ++errorCount;
    }
    if (errorCount > 0)
    {
        std::fprintf(stderr, "rsx-cg-compiler: %d parse error(s).\n", errorCount);
        return 1;
    }

    if (!ast)
    {
        std::fprintf(stderr, "rsx-cg-compiler: parse produced no AST.\n");
        return 1;
    }

    if (dumpAst)
    {
        std::cout << "-- AST ----------------------------------------------------------\n";
        AstPrinter printer;
        printer.print(*ast);
        std::cout << "----------------------------------------------------------------\n";
    }

    SemanticAnalyzer semantic;
    const ShaderStage stage =
        (ctx.profile == CompilerContext::Profile::FragmentRsx)
            ? ShaderStage::Fragment
            : ShaderStage::Vertex;
    semantic.setShaderStage(stage);
    semantic.setEntryPoint(ctx.entryName);

    const bool semanticOk = semantic.analyze(*ast);
    for (const auto& diag : semantic.diagnostics())
    {
        std::cerr << diag.toString() << std::endl;
    }
    if (!semanticOk)
    {
        std::fprintf(stderr, "rsx-cg-compiler: semantic analysis failed (%u error(s)).\n",
                     semantic.errorCount());
        return 1;
    }

    IRBuilder irBuilder;
    auto irModule = irBuilder.build(*ast, semantic);
    if (irBuilder.hasErrors() || !irModule)
    {
        for (const auto& err : irBuilder.errors())
        {
            std::fprintf(stderr, "%s\n", err.c_str());
        }
        std::fprintf(stderr, "rsx-cg-compiler: IR generation failed.\n");
        return 1;
    }

    // Run NV40-specific IR transforms before back-end lowering.
    // Currently: collapse simple if-else diamonds into Select so the
    // existing FP emit path handles them without IF/ELSE/ENDIF.
    nv40::convertSimpleIfElse(*irModule);

    if (dumpIr)
    {
        std::cout << "-- IR -----------------------------------------------------------\n";
        std::cout << irModule->toString();
        std::cout << "----------------------------------------------------------------\n";
    }

    if (ast->entryPoint)
    {
        std::cout << "Entry point: " << ast->entryPoint->name << "\n";
    }
    std::cout << "Stage: "
              << (stage == ShaderStage::Fragment ? "fragment" : "vertex")
              << "\n";
    std::cout << "IR functions: " << irModule->functions.size() << "\n";

    nv40::UcodeOutput  ucode;
    nv40::FpAttributes fpAttrs;
    nv40::VpAttributes vpAttrs;
    if (stage == ShaderStage::Fragment)
    {
        nv40::FpEmitResult r = nv40::emitFragmentProgramEx(
            *irModule, ctx.entryName, ctx.compileOpts);
        ucode   = std::move(r.ucode);
        fpAttrs = r.attrs;
    }
    else
    {
        nv40::VpEmitResult r = nv40::emitVertexProgramEx(
            *irModule, ctx.entryName, ctx.compileOpts);
        ucode   = std::move(r.ucode);
        vpAttrs = r.attrs;
    }

    for (const auto& diag : ucode.diagnostics)
    {
        std::fprintf(stderr, "%s\n", diag.c_str());
    }

    if (!ucode.ok)
    {
        std::cout << "NV40 emit: not ready (skeleton stage).\n";
        return 0;
    }

    if (!ctx.containerOutPath.empty())
    {
        std::vector<uint8_t>     containerBytes;
        std::vector<std::string> diagnostics;
        bool                     containerOk = false;

        if (stage == ShaderStage::Fragment)
        {
            sony::ContainerOptions copts;
            copts.alphakillSamplers = ctx.alphakillSamplers;
            sony::ContainerResult cr = sony::emitFragmentContainer(
                *irModule, ctx.entryName, ucode.words, fpAttrs, copts);
            containerBytes = std::move(cr.bytes);
            diagnostics    = std::move(cr.diagnostics);
            containerOk    = cr.ok;
        }
        else
        {
            sony::VpContainerResult cr = sony::emitVertexContainer(
                *irModule, ctx.entryName, ucode.words, vpAttrs);
            containerBytes = std::move(cr.bytes);
            diagnostics    = std::move(cr.diagnostics);
            containerOk    = cr.ok;
        }

        for (const auto& d : diagnostics)
            std::fprintf(stderr, "%s\n", d.c_str());

        if (!containerOk)
        {
            std::fprintf(stderr,
                "rsx-cg-compiler: container emit failed for %s\n",
                ctx.inputFile.c_str());
            return 1;
        }
        std::ofstream of(ctx.containerOutPath, std::ios::binary | std::ios::trunc);
        if (!of)
        {
            std::fprintf(stderr,
                "rsx-cg-compiler: cannot open %s for writing\n",
                ctx.containerOutPath.c_str());
            return 1;
        }
        of.write(reinterpret_cast<const char*>(containerBytes.data()),
                 static_cast<std::streamsize>(containerBytes.size()));
        if (!of)
        {
            std::fprintf(stderr, "rsx-cg-compiler: write failed\n");
            return 1;
        }
        return 0;
    }

    std::cout << "NV40 ucode words: " << ucode.words.size() << "\n";
    for (size_t w = 0; w < ucode.words.size(); ++w)
    {
        if ((w % 4) == 0) std::printf("  %3zu:", w / 4);
        std::printf(" %08x", ucode.words[w]);
        if ((w % 4) == 3) std::printf("\n");
    }
    return 0;
}
