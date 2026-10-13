#pragma once
// A select between two MATRIX values (an if/else that assigns a float2x2 on
// both arms, crt-royale's pixel_to_tangent_video_uv) became one Select of
// matrix type, which the lowering cannot resolve: a matrix value is only
// ever consumed row by row.  Rewrite it as a MatConstruct of per-row selects
// over the two arms' rows, the shape the lowering already handles (a
// MatConstruct of row vectors).  The condition is kept as is; a vector
// condition is left alone (the lowering keeps refusing it by name).

#include "ir.h"

#include <memory>
#include <vector>

namespace rsx_cg {

inline bool splitMatrixSelects(IRFunction& fn)
{
    bool changed = false;
    for (auto& block : fn.blocks) {
        if (!block) continue;
        auto& insts = block->instructions;
        for (size_t i = 0; i < insts.size(); ++i) {
            IRInstruction* inst = insts[i].get();
            if (!inst || inst->op != IROp::Select || inst->operands.size() != 3 ||
                !inst->resultType.isMatrix() || inst->resultType.arraySize != 0 ||
                inst->resultType.elementType != IRType::Float32)
                continue;
            const IRTypeInfo matrixType = inst->resultType;
            const int rows = matrixType.matrixRows, cols = matrixType.matrixCols;
            if (rows < 1 || rows > 4 || cols < 1 || cols > 4) continue;
            const IRTypeInfo rowType = cols == 1 ? IRTypeInfo::Float() :
                                       cols == 2 ? IRTypeInfo::Float2() :
                                       cols == 3 ? IRTypeInfo::Float3() : IRTypeInfo::Float4();
            std::vector<std::unique_ptr<IRInstruction>> added;
            std::vector<IRValueID> rowValues;
            for (int r = 0; r < rows; ++r) {
                const IRValueID index = fn.createConstant(IRTypeInfo::Int(), static_cast<int32_t>(r))->id;
                auto a = std::make_unique<IRInstruction>(IROp::VecExtract, fn.allocateValueId(), rowType);
                a->operands = {inst->operands[1], index};
                auto b = std::make_unique<IRInstruction>(IROp::VecExtract, fn.allocateValueId(), rowType);
                b->operands = {inst->operands[2], index};
                auto s = std::make_unique<IRInstruction>(IROp::Select, fn.allocateValueId(), rowType);
                s->operands = {inst->operands[0], a->result, b->result};
                rowValues.push_back(s->result);
                added.push_back(std::move(a));
                added.push_back(std::move(b));
                added.push_back(std::move(s));
            }
            auto construct = std::make_unique<IRInstruction>(IROp::MatConstruct, inst->result, matrixType);
            construct->operands = rowValues;
            const size_t n = added.size();
            insts[i] = std::move(construct);
            insts.insert(insts.begin() + static_cast<std::ptrdiff_t>(i),
                         std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
            i += n;
            changed = true;
        }
    }
    return changed;
}

} // namespace rsx_cg
