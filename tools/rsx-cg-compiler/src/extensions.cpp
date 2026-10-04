/*
 * rsx-cg-compiler — named extensions: the registry and the one detector.
 */

#include "extensions.h"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace rsx_cg
{

namespace
{

const ExtensionInfo kExtensions[] = {
    { Extension::Bom, "bom",
      "accept one leading UTF-8 byte order mark (EF BB BF) in the main source "
      "and in #include files; the reference refuses it" },
    { Extension::DeclaratorTypes, "declarator-types",
      "a later declarator in a list keeps the declared type; the reference gives it "
      "the type last named in an earlier declarator" },
    { Extension::StaticParameters, "static-parameters",
      "accept helper static, static const, and const static parameters; "
      "preserve const and refuse static parameters on the selected entry" },
    { Extension::GlslFunctions, "glsl-functions",
      "accept fract, mix, and floor-remainder mod on numeric scalars/vectors; "
      "preserve source functions and boolean mix selection" },
    { Extension::GlslTypes, "glsl-types",
      "accept vec/ivec/bvec/dvec2..4 and mat2..4/matRxC type names; "
      "retain Cg row-major matrix shapes, scalar splats, and conversion limits" },
    { Extension::Utf16, "utf16",
      "accept a source file whose first two bytes are a UTF-16 byte order mark "
      "(FF FE = UTF-16LE, FE FF = UTF-16BE).  Disabled by default, a UTF-16-marked "
      "file is refused.  Enabled, the whole file is transcoded to UTF-8 before "
      "the lexer sees it.  Odd byte count, an unpaired surrogate, or a NUL after "
      "the BOM still refuses." },
};

constexpr std::size_t kExtensionCount = sizeof(kExtensions) / sizeof(kExtensions[0]);
static_assert(kExtensionCount == static_cast<std::size_t>(Extension::Count),
              "every Extension enumerator has exactly one table row");

bool hasLeadingUtf8Bom(const std::string& text)
{
    return text.size() >= 3 &&
           static_cast<unsigned char>(text[0]) == 0xEF &&
           static_cast<unsigned char>(text[1]) == 0xBB &&
           static_cast<unsigned char>(text[2]) == 0xBF;
}

// The two leading-byte UTF-16 BOMs, in the byte order they appear.
// FF FE is UTF-16LE; FE FF is UTF-16BE.  Both encode code point U+FEFF;
// the byte order in the file IS the encoding's own declaration, so the
// ordering is not inferred.
bool hasLeadingUtf16Bom(const std::string& text, bool& is_le)
{
    if (text.size() < 2)
        return false;
    const unsigned char a = static_cast<unsigned char>(text[0]);
    const unsigned char b = static_cast<unsigned char>(text[1]);
    if (a == 0xFF && b == 0xFE)
    {
        is_le = true;
        return true;
    }
    if (a == 0xFE && b == 0xFF)
    {
        is_le = false;
        return true;
    }
    return false;
}

// UTF-32 BOMs: FF FE 00 00 (UTF-32 LE) and 00 00 FE FF (UTF-32 BE).
// Both are four-byte signatures and must be caught BEFORE the two-byte
// UTF-16 check, because the leading two bytes of the UTF-32 LE BOM
// (FF FE) also match the UTF-16 LE BOM signature.  A UTF-32 file is not
// something this compiler supports or extends; the diagnostic says so
// directly, with no flag to enable.
bool hasLeadingUtf32Bom(const std::string& text, bool& is_le)
{
    if (text.size() < 4)
        return false;
    const unsigned char a = static_cast<unsigned char>(text[0]);
    const unsigned char b = static_cast<unsigned char>(text[1]);
    const unsigned char c = static_cast<unsigned char>(text[2]);
    const unsigned char d = static_cast<unsigned char>(text[3]);
    // FF FE 00 00  = UTF-32 LE
    if (a == 0xFF && b == 0xFE && c == 0x00 && d == 0x00)
    {
        is_le = true;
        return true;
    }
    // 00 00 FE FF  = UTF-32 BE
    if (a == 0x00 && b == 0x00 && c == 0xFE && d == 0xFF)
    {
        is_le = false;
        return true;
    }
    return false;
}

// Transcode a UTF-16 payload (no BOM; the BOM's two bytes have already
// been stripped) to UTF-8, byte order chosen by is_le.
//
// STRICT rules (the director's): odd byte count refuses; an unpaired
// surrogate refuses; a NUL code unit refuses (a C source cannot carry one).
// No unmarked-encoding guessing, no silent substitution.
//
// Returns true and replaces `out` on success.  On failure it returns false
// and classifies the defect in `defect` (0 = a NUL code unit, 1 = an
// unpaired surrogate) and in `bad_index` (that code unit's zero-based index),
// so the caller names which thing broke rather than a bare "invalid".
bool utf16ToUtf8Strict(const std::string& payload, bool is_le,
                       std::string& out, unsigned& defect,
                       std::size_t& bad_index)
{
    out.clear();
    defect = 0;
    bad_index = std::string::npos;
    if (payload.size() % 2 != 0)
        return false;

    const std::size_t n = payload.size() / 2;
    out.reserve(payload.size());  // upper bound on the UTF-8 size we can emit

    for (std::size_t i = 0; i < n; ++i)
    {
        const unsigned char lo = payload[is_le ? 2 * i     : 2 * i + 1];
        const unsigned char hi = payload[is_le ? 2 * i + 1 : 2 * i     ];
        const unsigned int   cp = static_cast<unsigned int>((hi << 8) | lo);

        if (cp == 0x0000)
        {
            defect = 0;
            bad_index = i;
            return false;
        }

        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            if (i + 1 >= n)
            {
                defect = 1;
                bad_index = i;
                return false;
            }
            const unsigned char lo2 = payload[is_le ? 2 * (i + 1)     : 2 * (i + 1) + 1];
            const unsigned char hi2 = payload[is_le ? 2 * (i + 1) + 1 : 2 * (i + 1)     ];
            const unsigned int   cp2 = static_cast<unsigned int>((hi2 << 8) | lo2);
            if (cp2 < 0xDC00 || cp2 > 0xDFFF)
            {
                defect = 1;
                bad_index = i;
                return false;
            }
            const unsigned int code = 0x10000u +
                ((cp   - 0xD800u) << 10) +
                ((cp2 - 0xDC00u));
            // code is always in U+10000..U+10FFFF, the 4-byte UTF-8 form:
            // F0 90-9F 80-BF 80-BF 80-BF.  (A 5-byte form does not exist in
            // modern UTF-8; the surrogate-pair range is exactly this.)
            out.push_back(static_cast<char>(0xF0 | ((code >> 18) & 0x07u)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80 | ((code >>  6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80 |  (code        & 0x3Fu)));
            ++i;  // that pair of code units is one code point, already consumed
            continue;
        }
        if (cp >= 0xDC00 && cp <= 0xDFFF)
        {
            defect = 1;
            bad_index = i;
            return false;
        }

        if (cp < 0x80u)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800u)
        {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1Fu)));
            out.push_back(static_cast<char>(0x80 | (cp        & 0x3Fu)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0Fu)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6)  & 0x3Fu)));
            out.push_back(static_cast<char>(0x80 |  (cp        & 0x3Fu)));
        }
    }
    return true;
}

}  // namespace

const ExtensionInfo* allExtensions(std::size_t& count)
{
    count = kExtensionCount;
    return kExtensions;
}

const ExtensionInfo* findExtension(const std::string& name)
{
    for (const ExtensionInfo& info : kExtensions)
    {
        if (name == info.name)
            return &info;
    }
    return nullptr;
}

std::string enableFlag(Extension id)
{
    return std::string(kExtensionFlagPrefix) + kExtensions[static_cast<std::size_t>(id)].name;
}

void admitSourceText(std::string& text, const std::string& path,
                     const ExtensionSet& enabled)
{
    // A file can only BE one encoding, so the BOMs are mutually exclusive.
    // Check the longest signature first to avoid prefix overlap:
    //   UTF-32 (4 bytes) before UTF-16 (2 bytes): the leading two bytes of
    //   the UTF-32 LE BOM (FF FE) also match the UTF-16 LE BOM, so without
    //   the UTF-32 check first the file would be mis-decoded as UTF-16.
    //   UTF-16 (2 bytes) before UTF-8 (3 bytes): neither overlap is possible
    //   because the first bytes differ.
    // Then the UTF-8 mark.

    // UTF-32: not supported, no flag to enable, explicit refusal in both modes.
    // (The UTF-32 LE BOM's first two bytes FF FE also match the UTF-16 LE
    // BOM, so this check must precede the UTF-16 check or the file would
    // be mis-decoded.)
    bool  utf32_le = false;
    if (hasLeadingUtf32Bom(text, utf32_le))
    {
        const char* which = utf32_le ? "UTF-32LE" : "UTF-32BE";
        throw std::runtime_error(
            path + ":1:1: error: file begins with a " + which +
            " byte order mark (FF FE 00 00 or 00 00 FE FF); this compiler "
            "accepts UTF-16 (FF FE / FE FF) and UTF-8 (EF BB BF) source only");
    }

    // Leading UTF-16 byte order mark: FF FE (UTF-16LE) or FE FF (UTF-16BE).
    // Off: the same located, pasteable-flag refusal the UTF-8 BOM gets.
    // On: the whole file is transcoded to UTF-8.  A file that carries a
    // UTF-16 mark but whose payload is not valid UTF-16 (odd length, an
    // unpaired surrogate, or a NUL) still refuses, and the diagnostic names
    // the specific defect rather than guessing.
    bool  is_le = false;
    if (hasLeadingUtf16Bom(text, is_le))
    {
        if (!enabled.has(Extension::Utf16))
        {
            throw std::runtime_error(
                path + ":1:1: error: file begins with a UTF-16 byte order mark "
                "(FF FE = UTF-16LE or FE FF = UTF-16BE); this compiler is "
                "opt-in for that and declines it unless the extension is "
                "enabled: " +
                enableFlag(Extension::Utf16) + " (see " + kListExtensionsFlag + ")");
        }
        const std::string payload = text.substr(2);
        std::string utf8;
        unsigned   defect = 0;
        std::size_t bad_index = std::string::npos;
        if (!utf16ToUtf8Strict(payload, is_le, utf8, defect, bad_index))
        {
            // defect 0 + npos  = odd byte count; defect 0 + index = a NUL;
            // defect 1 + index = an unpaired surrogate.  Name the exact
            // defect instead of a bare "invalid".
            const std::string what =
                (defect == 1)
                    ? "an unpaired surrogate at code index " + std::to_string(bad_index)
                    : (bad_index == std::string::npos
                           ? "an odd number of bytes after the byte order mark"
                           : "a U+0000 (NUL) at code index " + std::to_string(bad_index));
            throw std::runtime_error(
                path + ":1:1: error: with --extension=utf16 this file is not "
                "valid UTF-16 (" + what + "); the extension refuses to guess");
        }
        text.swap(utf8);
        // A UTF-16 file IS one encoding: exactly one leading mark, consumed
        // here.  Do NOT fall through to the UTF-8 BOM detector.  If the
        // payload's first code point was itself a mark (U+FEFF), it decoded
        // to a leading EF BB BF in the text - that is a SECOND mark, which is
        // an ordinary unknown-character error for the lexer, in both modes
        // and regardless of whether --extension=bom is also enabled (a second
        // mark is never consumable).  This keeps the single-leading-mark
        // contract the BOM test pins, and stops the utf8-bom extension from
        // consuming a mark that belongs to a different encoding.
        return;
    }

    // Leading UTF-8 BOM.  Exactly one mark, at offset 0 of this file's own
    // bytes.  A second mark, a mark anywhere later, and a mark inside a
    // comment are none of this detector's business: the first two reach the
    // lexer's unknown-character error at their real location in both modes,
    // and the third is stripped with the comment as it always was.
    if (hasLeadingUtf8Bom(text))
    {
        if (!enabled.has(Extension::Bom))
        {
            throw std::runtime_error(
                path + ":1:1: error: file begins with a UTF-8 byte order mark "
                "(EF BB BF); the reference compiler refuses it, and so does "
                "this compiler unless the extension is enabled: " +
                enableFlag(Extension::Bom) + " (see " + kListExtensionsFlag + ")");
        }
        text.erase(0, 3);
    }
}

}  // namespace rsx_cg
