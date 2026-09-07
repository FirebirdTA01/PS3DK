#ifndef RSX_CG_COMPILER_EXTENSIONS_H
#define RSX_CG_COMPILER_EXTENSIONS_H

/*
 * rsx-cg-compiler — named extensions (t_6f3fa9c3).
 *
 * An extension is a deliberate departure from the reference compiler.  Every
 * one is OFF by default, so an unflagged compile is reference-compatible in
 * what it accepts and in the bytes it emits; the default's only departure is
 * the diagnostic, which names the flag that would enable the construct it
 * found.  That diagnostic is the reason the surface here is so small and so
 * exact: it tells the user what to type, so the spelling it prints and the
 * spelling the parser accepts are composed from the SAME constant.
 *
 *   --extension=<name>    enable one named extension; repeatable
 *   --list-extensions     print the supported names
 *
 * Rules the director set (t_083035ac): no blanket "all", unknown names fail,
 * a hint comes only from a detector written for that construct - never from
 * a generic error - and correctness refusals stay on in every mode.  An
 * extension is admissible only where its enabled output reduces to something
 * the reference already covers (the BOM'd source compiles to the bytes of
 * the plain source), which is what its test then pins.
 *
 * This file lives beside the driver, not under donor/: the policy is ours,
 * the donor frontend is shared with the Vita compiler, and their reference
 * makes different choices (it accepts a BOM).
 */

#include <string>

namespace rsx_cg
{

enum class Extension
{
    // Accept one leading UTF-8 byte order mark (EF BB BF) in the main
    // source and in #include files.  The reference refuses it.
    Bom = 0,

    Count
};

struct ExtensionInfo
{
    Extension   id;
    const char* name;     // the spelling after --extension=
    const char* summary;  // one line, printed by --list-extensions
};

// The prefix every enable flag is spelled with.  The arg parser matches on
// it and the disabled-extension diagnostic composes its hint from it, so the
// code cannot print a flag the parser does not accept.
constexpr const char* kExtensionFlagPrefix = "--extension=";
constexpr const char* kListExtensionsFlag  = "--list-extensions";

// The table, in the order --list-extensions prints it.
const ExtensionInfo* allExtensions(std::size_t& count);

// Exact, case-sensitive lookup.  nullptr for an unknown name.
const ExtensionInfo* findExtension(const std::string& name);

// "--extension=bom": the flag that enables `id`, for diagnostics and help.
std::string enableFlag(Extension id);

struct ExtensionSet
{
    bool has(Extension id) const { return enabled_[static_cast<std::size_t>(id)]; }
    void enable(Extension id)    { enabled_[static_cast<std::size_t>(id)] = true; }

private:
    bool enabled_[static_cast<std::size_t>(Extension::Count)] = {};
};

// The one place source bytes are examined for extension-only constructs.
// Called at BOTH points a .cg file enters the compiler - the file named on
// the command line and an #include - on the raw text of that file alone,
// before anything is composed in front of it and before the lexer sees it.
//
// A construct whose extension is enabled is rewritten to the form the
// reference accepts (a leading BOM is removed).  A construct whose extension
// is disabled throws std::runtime_error carrying a located diagnostic that
// names the exact enable flag; the driver's existing handler turns that into
// exit 1 with no output artifact.  Text with no such construct is untouched.
//
// Detection keys on the exact bytes of the construct at the place the
// extension defines - a BOM is the three bytes EF BB BF at offset 0 - never
// on a lexer error class.  A leading U+00A9 reaches the lexer's unknown-
// character error exactly as a BOM did before this existed, and must keep
// doing so with no hint: a hint from an unrelated error is the thing the
// director ruled out.
void admitSourceText(std::string& text, const std::string& path,
                     const ExtensionSet& enabled);

}  // namespace rsx_cg

#endif  /* RSX_CG_COMPILER_EXTENSIONS_H */
