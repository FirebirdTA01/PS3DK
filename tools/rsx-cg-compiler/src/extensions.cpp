/*
 * rsx-cg-compiler — named extensions: the registry and the one detector.
 */

#include "extensions.h"

#include <cstring>
#include <stdexcept>

namespace rsx_cg
{

namespace
{

const ExtensionInfo kExtensions[] = {
    { Extension::Bom, "bom",
      "accept one leading UTF-8 byte order mark (EF BB BF) in the main source "
      "and in #include files; the reference refuses it" },
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
