#ifndef RSX_CG_VP_UNIFORM_STRUCT_LAYOUT_H
#define RSX_CG_VP_UNIFORM_STRUCT_LAYOUT_H

#include "array_uniforms.h"
#include <optional>
#include <set>

namespace rsx_cg {

struct UniformStructLeaf {
    std::string path;
    IRTypeInfo type;
    uint32_t offset = 0;
    uint32_t registerRows = 1;
    std::vector<IRValueID> elementGlobals;
    // Unread leaves still occupy the stride but have UNBOUND records.
    std::set<uint32_t> referencedElements;
};

struct VpUniformStructShape {
    IRValueID root = InvalidIRValue;
    uint32_t extent = 0;
    uint32_t stride = 0;
    std::vector<UniformStructLeaf> leaves;
};

struct VpUniformStructLayout {
    VpUniformStructShape shape;
    uint32_t firstRegister = 0;
    uint32_t registerCount = 0;
};
using VpUniformStructLayouts = std::map<IRValueID, VpUniformStructLayout>;

inline std::set<IRValueID> vpStructReadGlobals(const IRModule& module, const IRFunction& entry)
{
    auto read = vpReadUniforms(entry);
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& ip : block->instructions) {
            if (!ip || ip->op != IROp::LoadUniform || !ip->uniformStructRead) continue;
            int element = ip->componentIndex;
            const bool constant = ip->arrayIndexKind == IRInstruction::ArrayIndexKind::Constant ||
                (!ip->operands.empty() && foldConstantIndex(entry, ip->operands[0], element));
            if (!constant || element < 0) continue;
            for (const auto& global : module.globals) {
                if (!global.uniformStructMember) continue;
                const auto& origin = *global.uniformStructMember;
                if (origin.root == ip->uniformSource && origin.element == uint32_t(element) &&
                    origin.leaf == ip->uniformStructRead->leaf) read.insert(global.valueId);
            }
        }
    }
    return read;
}

inline std::optional<std::map<IRValueID, VpUniformStructShape>>
classifyVpUniformStructShapes(const IRModule& module, const IRFunction& entry,
                             std::string& diagnostic)
{
    std::map<IRValueID, const IRGlobal*> globals;
    for (const auto& global : module.globals) globals.emplace(global.valueId, &global);
    std::map<IRValueID, VpUniformStructShape> shapes;
    const auto fail = [&](const std::string& why) -> decltype(classifyVpUniformStructShapes(module, entry, diagnostic)) {
        diagnostic = "vp-struct-array-layout: " + why;
        return std::nullopt;
    };
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& ip : block->instructions) {
            if (!ip || ip->op != IROp::LoadUniform || !ip->uniformStructRead) continue;
            int folded = 0;
            if (ip->arrayIndexKind == IRInstruction::ArrayIndexKind::Constant ||
                (ip->arrayIndexKind == IRInstruction::ArrayIndexKind::Dynamic &&
                 !ip->operands.empty() && foldConstantIndex(entry, ip->operands[0], folded))) continue;
            if (ip->arrayIndexKind != IRInstruction::ArrayIndexKind::Dynamic || ip->operands.size() != 1)
                return fail("invalid dynamic read");
            const auto it = globals.find(ip->uniformSource);
            if (it == globals.end() || !it->second->uniformStructRoot)
                return fail("missing aggregate declaration");
            const auto& info = *it->second->uniformStructRoot;
            if (!info.dynamicRefusalReason.empty()) return fail(info.dynamicRefusalReason);
            if (!info.extent || !info.leafCount || uint64_t(info.extent) * info.leafCount > 512)
                return fail("aggregate extent exceeds the constant register file");
            if (shapes.count(ip->uniformSource)) continue;
            VpUniformStructShape s;
            s.root = ip->uniformSource;
            s.extent = info.extent;
            s.stride = info.leafCount;
            s.leaves.resize(info.leafCount);
            for (uint32_t leaf = 0; leaf < info.leafCount; ++leaf) {
                s.leaves[leaf].offset = leaf;
                s.leaves[leaf].elementGlobals.resize(info.extent, InvalidIRValue);
            }
            shapes.emplace(s.root, std::move(s));
        }
    }
    for (const auto& global : module.globals) {
        if (!global.uniformStructMember) continue;
        const auto& origin = *global.uniformStructMember;
        auto si = shapes.find(origin.root);
        if (si == shapes.end()) continue;
        auto& s = si->second;
        if (origin.element >= s.extent || origin.leaf >= s.leaves.size()) return fail("invalid member origin");
        auto& leaf = s.leaves[origin.leaf];
        if (leaf.elementGlobals[origin.element] != InvalidIRValue) return fail("duplicate member origin");
        if (global.type.isArray() || global.type.isMatrix() || global.explicitRegisterBank ||
            !global.semanticName.empty()) return fail("unsupported dynamic member shape/binding");
        if (origin.element == 0) {
            leaf.path = origin.path;
            leaf.type = global.type;
        }
        leaf.elementGlobals[origin.element] = global.valueId;
    }
    for (const auto& [root, shape] : shapes) {
        for (const auto& leaf : shape.leaves) {
            for (IRValueID id : leaf.elementGlobals) {
                if (id == InvalidIRValue) return fail("missing member origin");
                const auto& global = *globals.at(id);
                if (global.uniformStructMember->path != leaf.path ||
                    global.type.baseType != leaf.type.baseType ||
                    global.type.elementType != leaf.type.elementType ||
                    global.type.vectorSize != leaf.type.vectorSize)
                    return fail("inconsistent element member types");
            }
        }
    }
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& ip : block->instructions) {
            if (!ip) continue;
            const auto markGlobal = [&](IRValueID id) {
                const auto gi = globals.find(id);
                if (gi == globals.end() || !gi->second->uniformStructMember) return;
                const auto& origin = *gi->second->uniformStructMember;
                auto si = shapes.find(origin.root);
                if (si != shapes.end()) si->second.leaves[origin.leaf].referencedElements.insert(origin.element);
            };
            for (auto id : ip->operands) markGlobal(id);
            if (ip->op != IROp::LoadUniform) continue;
            if (!ip->uniformStructRead) { markGlobal(ip->uniformSource); continue; }
            auto si = shapes.find(ip->uniformSource);
            if (si == shapes.end()) continue;
            auto& s = si->second;
            const auto& read = *ip->uniformStructRead;
            if (read.leaf >= s.leaves.size() || read.row != 0) return fail("invalid leaf/row read");
            int element = ip->componentIndex;
            const bool constant = ip->arrayIndexKind == IRInstruction::ArrayIndexKind::Constant ||
                (!ip->operands.empty() && foldConstantIndex(entry, ip->operands[0], element));
            if (constant) {
                if (element < 0 || uint32_t(element) >= s.extent) return fail("constant index out of range");
                s.leaves[read.leaf].referencedElements.insert(static_cast<uint32_t>(element));
            } else {
                for (uint32_t k = 0; k < s.extent; ++k) s.leaves[read.leaf].referencedElements.insert(k);
            }
        }
    }
    return shapes;
}

inline std::optional<VpUniformStructLayout>
bindVpUniformStructLayout(const VpUniformStructShape& shape, uint32_t first,
                          std::string& diagnostic)
{
    const uint64_t count = uint64_t(shape.extent) * shape.stride;
    if (!count || count > 512 || first >= 512 || count > 512 - first) {
        diagnostic = "vp-struct-array-layout: block exceeds the constant register file";
        return std::nullopt;
    }
    return VpUniformStructLayout{shape, first, static_cast<uint32_t>(count)};
}

inline std::optional<uint32_t>
vpUniformStructRegister(const VpUniformStructLayout& layout, uint32_t element,
                        uint32_t leaf, uint32_t row, std::string& diagnostic)
{
    if (element < layout.shape.extent && leaf < layout.shape.leaves.size()) {
        const auto& member = layout.shape.leaves[leaf];
        const uint64_t offset = uint64_t(element) * layout.shape.stride + member.offset + row;
        if (row < member.registerRows && offset < layout.registerCount &&
            uint64_t(layout.firstRegister) + offset < 512)
            return static_cast<uint32_t>(layout.firstRegister + offset);
    }
    diagnostic = "vp-struct-array-layout: member address out of range";
    return std::nullopt;
}

} // namespace rsx_cg
#endif
