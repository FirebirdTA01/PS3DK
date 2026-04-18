/*
 * cgcomp-v2 — Phase 8 Cg → NV40 compiler driver.
 *
 * Stage 1: front-end skeleton.  Parses a .cg shader through the
 * vita-cg-compiler donor front-end (preprocessor + lexer + parser) and
 * dumps the AST.  No NV40 lowering or codegen yet — those land in
 * Stage 2.
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
#include "builtin_shader_header_api.h"

namespace
{

// Stage 1 carries its own minimal context.  We deliberately do NOT pull
// in the donor's CompilerContext because compiler_options.h transitively
// drags the USSE back-end types (UsseProgram lives in lowering.h).
// Our NV40-targeted context will live in a separate header in Stage 2.
struct CgcV2Context
{
    std::string              inputFile;
    std::string              entryName = "main";
    std::vector<std::string> includeDirs;
    bool                     noStdLib = false;

    enum class Profile { Unknown, FragmentRsx, VertexRsx };
    Profile profile = Profile::VertexRsx;
};

void usage()
{
    std::fprintf(stderr,
        "cgcomp-v2 (Phase 8 stage 1) — Cg parser front-end\n"
        "\n"
        "Usage: cgcomp-v2 [options] <shader.cg>\n"
        "\n"
        "Options:\n"
        "  -p, --profile <name>   sce_fp_rsx | sce_vp_rsx (default: sce_vp_rsx)\n"
        "  -e, --entry <name>     Entry function (default: main)\n"
        "  -I <dir>               Add include directory\n"
        "  --no-stdlib            Skip the embedded Cg standard-library header\n"
        "  --dump-ast             Print the parsed AST to stdout\n"
        "  -h, --help             Show this message\n"
        "\n"
        "Stage 1 produces no shader output — only AST inspection.\n");
}

std::string slurpFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        std::fprintf(stderr, "cgcomp-v2: cannot open %s\n", path.c_str());
        std::exit(1);
    }
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::string runPreprocessor(const std::string& sourceCode,
                            const CgcV2Context& ctx)
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
    return pp.process(composed.str(), ctx.inputFile);
}

}  // namespace

int main(int argc, char** argv)
{
    CgcV2Context ctx;
    bool dumpAst = true;  // Stage 1 default — that's the deliverable

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
                ctx.profile = CgcV2Context::Profile::FragmentRsx;
            else if (p == "sce_vp_rsx" || p == "sce_vp_psp2")
                ctx.profile = CgcV2Context::Profile::VertexRsx;
            else
            {
                std::fprintf(stderr, "cgcomp-v2: unknown profile '%s'\n", p.c_str());
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
        else if (!arg.empty() && arg[0] == '-')
        {
            std::fprintf(stderr, "cgcomp-v2: unknown option '%s'\n", arg.c_str());
            usage();
            return 1;
        }
        else
        {
            if (!ctx.inputFile.empty())
            {
                std::fprintf(stderr, "cgcomp-v2: only one input file allowed\n");
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
        std::fprintf(stderr, "cgcomp-v2: %d parse error(s).\n", errorCount);
        return 1;
    }

    if (!ast)
    {
        std::fprintf(stderr, "cgcomp-v2: parse produced no AST.\n");
        return 1;
    }

    if (dumpAst)
    {
        std::cout << "-- AST ----------------------------------------------------------\n";
        AstPrinter printer;
        printer.print(*ast);
        std::cout << "----------------------------------------------------------------\n";
    }

    if (ast->entryPoint)
    {
        std::cout << "Entry point: " << ast->entryPoint->name << "\n";
    }
    return 0;
}
