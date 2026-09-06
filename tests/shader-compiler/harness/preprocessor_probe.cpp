// t_e5fced4c: reach the preprocessor's public interface directly.
//
// Two of its properties cannot be observed through the compiler's command
// line at all: setNoLineMarkers() is never called by main.cpp, and the value
// __FILE__ expands to is a string the shader language then rejects, so the
// diagnostic that follows quotes nothing.  Both have already been broken
// once - the markers generated for a line discontinuity ignored the option,
// and an #include left __FILE__ naming the child after it returned - and
// both were found by a driver like this one rather than by any test in
// tests/shader-compiler.  This is that driver, kept.
//
// It prints the preprocessed text verbatim on stdout and nothing else, so
// the guard can diff against an exact expected string: a property asserted
// loosely here would miss the next case of exactly this kind.
#include "preprocessor.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int usage(const char* argv0)
{
    std::fprintf(stderr,
                 "usage: %s [--no-markers] [-I <dir>] <source>\n"
                 "  Prints the preprocessed source on stdout.\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv)
{
    bool noMarkers = false;
    std::string includeDir;
    std::string sourcePath;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--no-markers")
        {
            noMarkers = true;
        }
        else if (arg == "-I")
        {
            if (i + 1 >= argc) return usage(argv[0]);
            includeDir = argv[++i];
        }
        else if (!sourcePath.empty())
        {
            return usage(argv[0]);
        }
        else
        {
            sourcePath = arg;
        }
    }

    if (sourcePath.empty()) return usage(argv[0]);

    // An unreadable input must be REPORTED, not returned as a successful
    // run whose output happened to be empty.  A wrong path or an
    // unreadable file is otherwise indistinguishable from a working
    // preprocessor, and the guard's negatives depend on empty output
    // being a failure it can attribute rather than a result.
    std::ifstream in(sourcePath, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "cannot open %s\n", sourcePath.c_str());
        return 3;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.eof() && in.fail())
    {
        std::fprintf(stderr, "cannot read %s\n", sourcePath.c_str());
        return 3;
    }
    const std::string source = buffer.str();
    if (source.empty())
    {
        std::fprintf(stderr, "%s is empty\n", sourcePath.c_str());
        return 3;
    }

    Preprocessor pp;
    if (!includeDir.empty()) pp.addIncludePath(includeDir);
    pp.setNoLineMarkers(noMarkers);

    try
    {
        std::cout << pp.process(source, sourcePath);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "preprocessor threw: %s\n", e.what());
        return 4;
    }
    return 0;
}
