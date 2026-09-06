#include "builtin_shader_header_api.h"
#include <string>
#include <string_view>

// Automatic embedding of builtin_shader_header.h
//
// This file no longer contains a manually duplicated raw string.
// Instead, a pre-build step (added to the .vcxproj) generates
// 'builtin_shader_header_embedded.inc' from the editable header
// 'builtin_shader_header.h' and we simply include it.
//
// Edit: builtin_shader_header.h
// Build: the pre-build step regenerates builtin_shader_header_embedded.inc
// Use : call GetBuiltinShaderHeaderSource()/View() inside the compiler.
//
// If the generated .inc is missing (first build or build step failed),
// we fall back to a minimal placeholder so the build still succeeds.

#ifndef BUILTIN_HEADER_EMBED_INC
#define BUILTIN_HEADER_EMBED_INC "builtin_shader_header_embedded.inc"
#endif

namespace {
#if __has_include(BUILTIN_HEADER_EMBED_INC)
#include BUILTIN_HEADER_EMBED_INC
#else
// Fallback (should only appear if pre-build step failed).
// Important: keep this free of leading '#' preprocessor directives,
// so MSVC's preprocessor won't mis-scan it inside a disabled branch.
static constexpr char kBuiltinHeaderVerbatim[] =
R"__VSHDR__(/* Fallback Vita builtin header (generation failed).
             Version: 0
             This minimal header is only used when the pre-build step did not
             generate builtin_shader_header_embedded.inc. */
)__VSHDR__";
#endif

// This name is user-visible: main.cpp emits it as the `#line 1 "<name>"`
// marker ahead of the header text, and since t_1366b9b9 the lexer reads
// that marker, so a diagnostic raised inside this header is reported
// against this string.  It was the donor's own file name, which named
// the wrong console in the wrong project's output; a neutral label says
// what it is without pretending to be a file anyone can open.
static constexpr char kVirtualHeaderName[] = "<builtin>";
} // namespace

const char* GetBuiltinShaderHeaderName()
{
    return kVirtualHeaderName;
}

std::string_view GetBuiltinShaderHeaderView()
{
    return std::string_view(kBuiltinHeaderVerbatim, sizeof(kBuiltinHeaderVerbatim) - 1);
}

std::string GetBuiltinShaderHeaderSource()
{
    return std::string(GetBuiltinShaderHeaderView());
}