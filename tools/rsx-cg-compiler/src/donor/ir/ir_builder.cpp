#include <algorithm>
#include <vector>
#include "ir_builder.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <functional>

namespace
{
constexpr float kPiDiv180 = 0.017453292519943295769f;
constexpr float k180DivPi = 57.295779513082320876f;

std::optional<float> angleConversionScale(const std::string& name)
{
    if (name == "radians") return kPiDiv180;
    if (name == "degrees") return k180DivPi;
    return std::nullopt;
}
}

// ============================================================================
// Constructor/Destructor
// ============================================================================

IRBuilder::IRBuilder() = default;
IRBuilder::~IRBuilder() = default;

// ============================================================================
// Error Handling
// ============================================================================

void IRBuilder::error(const std::string& msg)
{
    errors_.push_back("IR generation error: " + msg);
}

void IRBuilder::error(const SourceLocation& loc, const std::string& msg)
{
    errors_.push_back(loc.toString() + ": IR generation error: " + msg);
}

std::string IRBuilder::makeLabel(const std::string& prefix)
{
    return prefix + std::to_string(labelCounter_++);
}

// ============================================================================
// Main Build Entry Point
// ============================================================================

// Evaluate a file-scope const's initialiser to floats (t_4584aa27).
// Deliberately NARROW: a scalar literal, or a constructor whose arguments
// are all scalar literals, with an optional leading unary minus.  Anything
// else returns false and the caller REFUSES the shader.
//
// Narrow rather than a general constant folder on purpose: the initialisers
// real shaders use are nearly all of this shape, and a half-right folder
// would put wrong numbers into containers that are otherwise well formed -
// the same class of silent damage this evaluator exists to remove.
namespace {

struct ConstEvalScalar
{
    enum class Kind { Int, UInt, Float, Bool };
    Kind kind = Kind::Int;
    int64_t i = 0;
    uint64_t u = 0;
    double f = 0.0;
    bool b = false;

    static ConstEvalScalar fromInt(int64_t val)
    {
        ConstEvalScalar s;
        s.kind = Kind::Int;
        s.i = val;
        return s;
    }

    static ConstEvalScalar fromUInt(uint64_t val)
    {
        ConstEvalScalar s;
        s.kind = Kind::UInt;
        s.u = val;
        return s;
    }

    static ConstEvalScalar fromFloat(double val)
    {
        ConstEvalScalar s;
        s.kind = Kind::Float;
        s.f = val;
        return s;
    }

    static ConstEvalScalar fromBool(bool val)
    {
        ConstEvalScalar s;
        s.kind = Kind::Bool;
        s.b = val;
        return s;
    }

    bool isTruthy() const
    {
        switch (kind)
        {
        case Kind::Int:   return i != 0;
        case Kind::UInt:  return u != 0;
        case Kind::Float: return f != 0.0;
        case Kind::Bool:  return b;
        }
        return false;
    }

    double asDouble() const
    {
        switch (kind)
        {
        case Kind::Int:   return static_cast<double>(i);
        case Kind::UInt:  return static_cast<double>(u);
        case Kind::Float: return f;
        case Kind::Bool:  return b ? 1.0 : 0.0;
        }
        return 0.0;
    }

    int64_t asInt64() const
    {
        switch (kind)
        {
        case Kind::Int:   return i;
        case Kind::UInt:  return static_cast<int64_t>(u);
        case Kind::Float: return static_cast<int64_t>(f);
        case Kind::Bool:  return b ? 1 : 0;
        }
        return 0;
    }
};

static bool convertScalar(const ConstEvalScalar& in, BaseType targetType, ConstEvalScalar& out)
{
    switch (targetType)
    {
    case BaseType::Bool:
        out = ConstEvalScalar::fromBool(in.isTruthy());
        return true;

    case BaseType::Int:
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            out = ConstEvalScalar::fromInt(static_cast<int32_t>(in.i));
            return true;
        case ConstEvalScalar::Kind::UInt:
            out = ConstEvalScalar::fromInt(static_cast<int32_t>(static_cast<uint32_t>(in.u)));
            return true;
        case ConstEvalScalar::Kind::Float: {
            double d = in.f;
            if (std::isnan(d) || std::isinf(d) || d < -2147483648.0 || d >= 2147483648.0)
                out = ConstEvalScalar::fromInt(-2147483648LL);
            else
                out = ConstEvalScalar::fromInt(static_cast<int32_t>(d));
            return true;
        }
        case ConstEvalScalar::Kind::Bool:
            out = ConstEvalScalar::fromInt(in.b ? 1 : 0);
            return true;
        }
        return false;

    case BaseType::UInt:
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            out = ConstEvalScalar::fromUInt(static_cast<uint32_t>(in.i));
            return true;
        case ConstEvalScalar::Kind::UInt:
            out = ConstEvalScalar::fromUInt(static_cast<uint32_t>(in.u));
            return true;
        case ConstEvalScalar::Kind::Float: {
            double d = in.f;
            if (std::isnan(d) || std::isinf(d))
                out = ConstEvalScalar::fromUInt(0);
            else if (d < 0.0)
                out = ConstEvalScalar::fromUInt(static_cast<uint32_t>(static_cast<int64_t>(d)));
            else
                out = ConstEvalScalar::fromUInt(static_cast<uint32_t>(static_cast<uint64_t>(std::fmod(d, 4294967296.0))));
            return true;
        }
        case ConstEvalScalar::Kind::Bool:
            out = ConstEvalScalar::fromUInt(in.b ? 1 : 0);
            return true;
        }
        return false;

    case BaseType::Short:
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            out = ConstEvalScalar::fromInt(static_cast<int16_t>(in.i));
            return true;
        case ConstEvalScalar::Kind::UInt:
            out = ConstEvalScalar::fromInt(static_cast<int16_t>(static_cast<uint16_t>(in.u)));
            return true;
        case ConstEvalScalar::Kind::Float: {
            double d = in.f;
            int32_t i32 = (std::isnan(d) || std::isinf(d) || d < -2147483648.0 || d >= 2147483648.0)
                              ? static_cast<int32_t>(-2147483648LL)
                              : static_cast<int32_t>(d);
            out = ConstEvalScalar::fromInt(static_cast<int16_t>(i32));
            return true;
        }
        case ConstEvalScalar::Kind::Bool:
            out = ConstEvalScalar::fromInt(in.b ? 1 : 0);
            return true;
        }
        return false;

    case BaseType::UShort:
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            out = ConstEvalScalar::fromUInt(static_cast<uint16_t>(in.i));
            return true;
        case ConstEvalScalar::Kind::UInt:
            out = ConstEvalScalar::fromUInt(static_cast<uint16_t>(in.u));
            return true;
        case ConstEvalScalar::Kind::Float: {
            double d = in.f;
            uint32_t u32 = (std::isnan(d) || std::isinf(d))
                               ? 0
                               : (d < 0.0 ? static_cast<uint32_t>(static_cast<int64_t>(d))
                                          : static_cast<uint32_t>(static_cast<uint64_t>(std::fmod(d, 4294967296.0))));
            out = ConstEvalScalar::fromUInt(static_cast<uint16_t>(u32));
            return true;
        }
        case ConstEvalScalar::Kind::Bool:
            out = ConstEvalScalar::fromUInt(in.b ? 1 : 0);
            return true;
        }
        return false;

    case BaseType::Char:
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            out = ConstEvalScalar::fromInt(static_cast<int8_t>(in.i));
            return true;
        case ConstEvalScalar::Kind::UInt:
            out = ConstEvalScalar::fromInt(static_cast<int8_t>(static_cast<uint8_t>(in.u)));
            return true;
        case ConstEvalScalar::Kind::Float: {
            double d = in.f;
            int32_t i32 = (std::isnan(d) || std::isinf(d) || d < -2147483648.0 || d >= 2147483648.0)
                              ? static_cast<int32_t>(-2147483648LL)
                              : static_cast<int32_t>(d);
            out = ConstEvalScalar::fromInt(static_cast<int8_t>(i32));
            return true;
        }
        case ConstEvalScalar::Kind::Bool:
            out = ConstEvalScalar::fromInt(in.b ? 1 : 0);
            return true;
        }
        return false;

    case BaseType::UChar:
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            out = ConstEvalScalar::fromUInt(static_cast<uint8_t>(in.i));
            return true;
        case ConstEvalScalar::Kind::UInt:
            out = ConstEvalScalar::fromUInt(static_cast<uint8_t>(in.u));
            return true;
        case ConstEvalScalar::Kind::Float: {
            double d = in.f;
            uint32_t u32 = (std::isnan(d) || std::isinf(d))
                               ? 0
                               : (d < 0.0 ? static_cast<uint32_t>(static_cast<int64_t>(d))
                                          : static_cast<uint32_t>(static_cast<uint64_t>(std::fmod(d, 4294967296.0))));
            out = ConstEvalScalar::fromUInt(static_cast<uint8_t>(u32));
            return true;
        }
        case ConstEvalScalar::Kind::Bool:
            out = ConstEvalScalar::fromUInt(in.b ? 1 : 0);
            return true;
        }
        return false;

    case BaseType::Float:
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            out = ConstEvalScalar::fromFloat(static_cast<double>(in.i));
            return true;
        case ConstEvalScalar::Kind::UInt:
            out = ConstEvalScalar::fromFloat(static_cast<double>(in.u));
            return true;
        case ConstEvalScalar::Kind::Float:
            out = in;
            return true;
        case ConstEvalScalar::Kind::Bool:
            out = ConstEvalScalar::fromFloat(in.b ? 1.0 : 0.0);
            return true;
        }
        return false;

    case BaseType::Half:
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            out = ConstEvalScalar::fromFloat(static_cast<double>(IRUtils::roundToHalf(static_cast<float>(in.i))));
            return true;
        case ConstEvalScalar::Kind::UInt:
            out = ConstEvalScalar::fromFloat(static_cast<double>(IRUtils::roundToHalf(static_cast<float>(in.u))));
            return true;
        case ConstEvalScalar::Kind::Float:
            out = ConstEvalScalar::fromFloat(static_cast<double>(IRUtils::roundToHalf(static_cast<float>(in.f))));
            return true;
        case ConstEvalScalar::Kind::Bool:
            out = ConstEvalScalar::fromFloat(in.b ? 1.0 : 0.0);
            return true;
        }
        return false;

    case BaseType::Fixed: {
        double d = 0.0;
        switch (in.kind)
        {
        case ConstEvalScalar::Kind::Int:
            d = static_cast<double>(in.i);
            break;
        case ConstEvalScalar::Kind::UInt:
            d = static_cast<double>(in.u);
            break;
        case ConstEvalScalar::Kind::Float:
            d = in.f;
            break;
        case ConstEvalScalar::Kind::Bool:
            d = in.b ? 1.0 : 0.0;
            break;
        }
        // Clamps to [-2.0, 2.0 - 2^-10] = [-2.0, 1.9990234375]
        const double maxFixed = 2.0 - (1.0 / 1024.0); // 1.9990234375
        const double minFixed = -2.0;
        if (d > maxFixed) d = maxFixed;
        else if (d < minFixed) d = minFixed;
        // Quantize to 10 fractional bits, ties round toward +infinity
        d = std::floor(d * 1024.0 + 0.5) / 1024.0;
        out = ConstEvalScalar::fromFloat(d);
        return true;
    }

    default:
        return false;
    }
}

static bool evaluateConstScalar(const ExprNode* e, ConstEvalScalar& out)
{
    if (!e) return false;
    if (e->kind == ExprKind::Literal)
    {
        const auto* lit = static_cast<const LiteralExpr*>(e);
        switch (lit->literalKind)
        {
        case LiteralExpr::LiteralKind::Int:
            out = ConstEvalScalar::fromInt(std::get<int64_t>(lit->value));
            return true;
        case LiteralExpr::LiteralKind::Bool:
            out = ConstEvalScalar::fromBool(std::get<bool>(lit->value));
            return true;
        case LiteralExpr::LiteralKind::Float:
            out = ConstEvalScalar::fromFloat(std::get<double>(lit->value));
            return true;
        default:
            return false;
        }
    }
    if (e->kind == ExprKind::Unary)
    {
        const auto* u = static_cast<const UnaryExpr*>(e);
        ConstEvalScalar operandVal;
        if (!evaluateConstScalar(u->operand.get(), operandVal))
            return false;
        switch (u->op)
        {
        case UnaryOp::LogicalNot:
            out = ConstEvalScalar::fromBool(!operandVal.isTruthy());
            return true;
        case UnaryOp::Negate:
            switch (operandVal.kind)
            {
            case ConstEvalScalar::Kind::Int:
                out = ConstEvalScalar::fromInt(-operandVal.i);
                return true;
            case ConstEvalScalar::Kind::UInt:
                out = ConstEvalScalar::fromUInt(static_cast<uint32_t>(-static_cast<int64_t>(operandVal.u & 0xFFFFFFFFULL)));
                return true;
            case ConstEvalScalar::Kind::Float:
                out = ConstEvalScalar::fromFloat(-operandVal.f);
                return true;
            case ConstEvalScalar::Kind::Bool:
                return false;
            }
            return false;
        case UnaryOp::BitwiseNot:
            switch (operandVal.kind)
            {
            case ConstEvalScalar::Kind::Int:
                out = ConstEvalScalar::fromInt(~operandVal.i);
                return true;
            case ConstEvalScalar::Kind::UInt:
                out = ConstEvalScalar::fromUInt(~operandVal.u & 0xFFFFFFFFULL);
                return true;
            case ConstEvalScalar::Kind::Float:
            case ConstEvalScalar::Kind::Bool:
                return false;
            }
            return false;
        default:
            return false;
        }
    }
    if (e->kind == ExprKind::Constructor)
    {
        const auto* ctor = static_cast<const ConstructorExpr*>(e);
        if (ctor->arguments.size() != 1 || !ctor->constructedType)
            return false;
        ConstEvalScalar argVal;
        if (!evaluateConstScalar(ctor->arguments[0].get(), argVal))
            return false;
        return convertScalar(argVal, ctor->constructedType->baseType, out);
    }
    if (e->kind == ExprKind::Cast)
    {
        const auto* cast = static_cast<const CastExpr*>(e);
        if (!cast->targetType)
            return false;
        ConstEvalScalar argVal;
        if (!evaluateConstScalar(cast->operand.get(), argVal))
            return false;
        return convertScalar(argVal, cast->targetType->baseType, out);
    }
    return false;
}

} // namespace

bool IRBuilder::evaluateConstInitializerTyped(const ExprNode* init,
                                             const TypeNode* declType,
                                             std::vector<float>& floatOut,
                                             std::vector<int64_t>& intOut)
{
    floatOut.clear();
    intOut.clear();
    if (!init)
        return false;

    // First try scalar evaluation
    ConstEvalScalar scalarVal;
    if (evaluateConstScalar(init, scalarVal))
    {
        ConstEvalScalar converted;
        if (declType && !convertScalar(scalarVal, declType->baseType, converted))
            return false;
        else if (!declType)
            converted = scalarVal;

        floatOut.push_back(static_cast<float>(converted.asDouble()));
        intOut.push_back(converted.asInt64());
        return true;
    }

    // Vector / multi-argument constructor evaluation
    if (init->kind == ExprKind::Constructor)
    {
        const auto* ctor = static_cast<const ConstructorExpr*>(init);
        if (ctor->arguments.empty() || ctor->arguments.size() > 4 || !ctor->constructedType)
            return false;

        const BaseType elemType = ctor->constructedType->baseType;
        for (const auto& a : ctor->arguments)
        {
            ConstEvalScalar aVal;
            if (!evaluateConstScalar(a.get(), aVal))
                return false;
            ConstEvalScalar converted;
            if (!convertScalar(aVal, elemType, converted))
                return false;

            if (declType && declType->baseType != elemType)
            {
                ConstEvalScalar declConverted;
                if (!convertScalar(converted, declType->baseType, declConverted))
                    return false;
                converted = declConverted;
            }

            floatOut.push_back(static_cast<float>(converted.asDouble()));
            intOut.push_back(converted.asInt64());
        }
        return true;
    }

    return false;
}

bool IRBuilder::evaluateConstInitializer(const ExprNode* init,
                                         std::vector<float>& out)
{
    std::vector<int64_t> dummyInt;
    return evaluateConstInitializerTyped(init, nullptr, out, dummyInt);
}

bool IRBuilder::evaluateConstIntInitializer(const ExprNode* init,
                                            std::vector<int64_t>& out)
{
    std::vector<float> dummyFloat;
    return evaluateConstInitializerTyped(init, nullptr, dummyFloat, out);
}

std::unique_ptr<IRModule> IRBuilder::build(TranslationUnit& unit, const SemanticAnalyzer& semantic)
{
    semantic_ = &semantic;
    module_ = std::make_unique<IRModule>(unit.filename);
    depthDecodeUniforms_.clear();

    // Set shader stage
    module_->shaderStage = semantic.shaderInfo().stage;
    module_->entryPointName = semantic.shaderInfo().entryPointName;

    // Build globals (uniforms, attributes, etc.)
    buildGlobals(unit);

    functionDefinitionsByName_.clear();
    for (auto& decl : unit.declarations)
    {
        if (decl->kind != DeclKind::Function)
            continue;
        auto* funcDecl = static_cast<FunctionDecl*>(decl.get());
        if (!funcDecl->isPrototype() && !funcDecl->isIntrinsic)
            functionDefinitionsByName_[funcDecl->name].push_back(funcDecl);
    }

    // Build functions
    for (auto& decl : unit.declarations)
    {
        if (decl->kind == DeclKind::Function)
        {
            auto* funcDecl = static_cast<FunctionDecl*>(decl.get());
            if (!funcDecl->isPrototype() && !funcDecl->isIntrinsic)
            {
                buildFunction(funcDecl);
            }
        }
    }

    // Set entry point
    module_->entryPoint = module_->getFunction(module_->entryPointName);

    return std::move(module_);
}

// ============================================================================
// Global Building
// ============================================================================

void IRBuilder::buildGlobals(TranslationUnit& unit)
{
    for (auto& decl : unit.declarations)
    {
        if (decl->kind == DeclKind::Variable)
        {
            auto* varDecl = static_cast<VarDecl*>(decl.get());

            // Cg implicitly treats file-scope variables with no explicit
            // storage qualifier as `uniform` (e.g. `float4x4 gMtx;`).
            if (varDecl->storage == StorageQualifier::None)
                varDecl->storage = StorageQualifier::Uniform;

            // A file-scope declaration's initialiser is evaluated here, and
            // it means two different things depending on the qualifier:
            //
            //   const   - every reference FOLDS to the value, so the value
            //             must be known or the fold is a zero (t_4584aa27).
            //   uniform - the value is the parameter's COMPILED DEFAULT,
            //             written into the same inline const block that a
            //             runtime patch later overwrites.  So `uniform
            //             float4 c = float4(1,0,0,1);` works unpatched AND
            //             stays patchable by name, which is what the
            //             reference compiler does (t_3bf3ce95).
            //
            // A uniform WITHOUT an initialiser is untouched: no compiled
            // default, blocks stay zero-filled, exactly as before.
            std::vector<float> constInit;
            std::vector<int64_t> constIntInit;
            const bool isFileScopeConst =
                varDecl->storage == StorageQualifier::Const;
            const bool isInitialisedUniform =
                varDecl->storage == StorageQualifier::Uniform &&
                varDecl->initializer != nullptr;
            if (isFileScopeConst || isInitialisedUniform)
            {
                if (!evaluateConstInitializerTyped(varDecl->initializer.get(),
                                                   varDecl->type.get(),
                                                   constInit,
                                                   constIntInit))
                {
                    // REFUSE rather than drop it.  Silently emitting zero for
                    // a value we could not evaluate is the defect this fix
                    // exists to remove, and a wrong constant is invisible in
                    // a container that is otherwise well formed.  A uniform
                    // is no safer: a compiled default of zero where the
                    // source says otherwise is a shader that renders wrong
                    // until something patches it.
                    error(varDecl->loc,
                          std::string("file-scope ") +
                          (isFileScopeConst ? "const '" : "uniform '") +
                          varDecl->name +
                          "' has an initialiser this compiler cannot evaluate; "
                          "refusing rather than compiling it as zero");
                    constInit.clear();
                    constIntInit.clear();
                }
            }

            IRGlobal global;
            global.name = varDecl->name;
            global.type = getIRType(varDecl->type.get());
            global.valueId = module_->allocateGlobalId();
            global.storage = varDecl->storage;

            if (!varDecl->semantic.isEmpty())
            {
                global.semanticName = varDecl->semantic.name;
                global.semanticIndex = varDecl->semantic.index;
            }

            // Carry explicit `: register(CN)` binding into the global so
            // the const allocator can pin the matrix/vector to the
            // requested constant register instead of the default.
            if (varDecl->semantic.hasExplicitRegister())
            {
                global.explicitRegisterBank  = varDecl->semantic.explicitRegisterBank;
                global.explicitRegisterIndex = varDecl->semantic.explicitRegisterIndex;
            }

            // Cg broadcasts a scalar initialiser across a vector: `float4 c
            // = 1.0;` and `float4 c = float4(1.0);` both mean (1,1,1,1),
            // and the frontend accepts them.  The evaluator above reports
            // what the EXPRESSION held, which is one component, so without
            // this the compiled default is (1,0,0,0) - right in x and zero
            // everywhere else (review finding, codex).  Widen to the
            // DECLARED component count, which is the width a runtime patch
            // of this parameter writes and the width the fold needs.
            if (constInit.size() == 1)
            {
                const int declared = global.type.componentCount();
                if (declared > 1)
                    constInit.assign(static_cast<size_t>(declared), constInit[0]);
            }
            else if (!constInit.empty() &&
                     constInit.size() !=
                         static_cast<size_t>(global.type.componentCount()))
            {
                // Any other count mismatch is a form this narrow evaluator
                // does not understand well enough to widen.  Refuse rather
                // than pad with zeros, for the same reason as above.
                error(varDecl->loc,
                      "file-scope '" + varDecl->name + "' has a " +
                      std::to_string(constInit.size()) +
                      "-component initialiser for a " +
                      std::to_string(global.type.componentCount()) +
                      "-component declaration; refusing rather than "
                      "padding it with zeros");
                constInit.clear();
                constIntInit.clear();
            }

            if (constIntInit.size() == 1)
            {
                const int declared = global.type.componentCount();
                if (declared > 1)
                    constIntInit.assign(static_cast<size_t>(declared), constIntInit[0]);
            }
            global.initialIntValues = constIntInit;
            global.initialValue = constInit;

            module_->addGlobal(global);

            // Record the global's value id against its declaration so
            // the lowering can resolve the source const-bank slot.  Other
            // globals stay in nameToValue_ (existing behaviour).
            declToValue_[varDecl] = global.valueId;
            // Uniforms are kept OUT of nameToValue_ so buildIdentifierExpr
            // falls through to findGlobal() and emits a LoadUniform.  A
            // file-scope CONST is kept out for the same reason and a
            // different destination: its reference materialises an
            // IRConstant from the initialiser recorded on the global.
            //
            // It used to be mapped to `global.valueId` - a value id with no
            // IRConstant behind it, because IRGlobal carried no initialiser.
            // Every reference then resolved to a value that was never
            // populated and the constant emitted as ZERO: `const float K =
            // 7.5; uv.x * K` compiled to `uv.x * 0.0`, on both paths, with
            // no diagnostic (t_4584aa27).  A local const never had this
            // problem because buildVarDeclStmt maps the name to
            // buildExpr(initialiser), which is a real IRConstant.
            //
            // File-scope `const` (including `static const` and `const static`,
            // both mapped to StorageQualifier::Const by the parser) folds to its
            // initializer so its value materializes as a typed IRConstant
            // rather than an unbacked uniform load (t_65e1b7fa).
            // Bare file-scope `static` is deliberately excluded here: static
            // variables in Cg are mutable, and folding an initial value at
            // every read would silently drop writes from helpers or other
            // functions (review finding, codex).
            const bool foldableConst =
                varDecl->storage == StorageQualifier::Const &&
                (!global.initialValue.empty() || !global.initialIntValues.empty());
            if (varDecl->storage != StorageQualifier::Uniform && !foldableConst)
            {
                nameToValue_[varDecl->name] = global.valueId;
            }

            // For uniform struct types, also flatten members into separate globals
            // so that "u_data.color" and "u_data.scale" get their own uniform slots
            if (varDecl->storage == StorageQualifier::Uniform &&
                varDecl->type && varDecl->type->baseType == BaseType::Struct)
            {
                const std::vector<StructField>* fields = getStructFields(varDecl->type.get());
                if (fields)
                {
                    std::string prefix = varDecl->name;
                    for (const auto& field : *fields)
                    {
                        std::string qualifiedName = prefix + "." + field.name;
                        IRTypeInfo fieldIRType = getIRType(field.type.get());

                        IRGlobal memberGlobal;
                        memberGlobal.name = qualifiedName;
                        memberGlobal.type = fieldIRType;
                        memberGlobal.valueId = module_->allocateGlobalId();
                        memberGlobal.storage = StorageQualifier::Uniform;

                        module_->addGlobal(memberGlobal);
                        nameToValue_[qualifiedName] = memberGlobal.valueId;
                    }
                }
            }
        }
        else if (decl->kind == DeclKind::Buffer)
        {
            // Buffer declarations (BUFFER[N]) - flatten struct members into individual globals
            // e.g., "u_perVFrame : BUFFER[0]" with struct containing u_viewMatrix, u_projectionMatrix
            // becomes globals: "u_perVFrame.u_viewMatrix", "u_perVFrame.u_projectionMatrix"
            auto* bufferDecl = static_cast<BufferDecl*>(decl.get());
            CgType bufferType = semantic_->resolveType(bufferDecl->type.get());

            if (bufferType.isStruct())
            {
                std::string prefix = bufferDecl->name;

                // Flatten each struct field into a separate global
                for (const auto& field : bufferType.structFields())
                {
                    std::string qualifiedName = prefix + "." + field.name;
                    IRTypeInfo fieldIRType = getIRType(field.type.get());

                    IRGlobal global;
                    global.name = qualifiedName;
                    global.type = fieldIRType;
                    global.valueId = module_->allocateGlobalId();
                    global.storage = StorageQualifier::Uniform;  // Buffer members act as uniforms
                    global.semanticName = "BUFFER";
                    global.semanticIndex = bufferDecl->bufferIndex;

                    module_->addGlobal(global);

                    // Map the qualified name to value for member access lookups
                    nameToValue_[qualifiedName] = global.valueId;
                }
            }
        }
    }
}

// A FRAGMENT entry's `out` parameter with NO semantic binds to COLOR.
//
// The reference compiler does this - its parameter table for a shader
// declaring `out float4 oColor` with no semantic reads
// "out.UNDEFINED: COLOR0", the declared semantic undefined and the resource
// COLOR0 - and without it we dropped the whole shader (t_a15ec129).  The
// store was gated on the parameter HAVING a semantic, so no StoreOutput was
// emitted, everything that only fed it went with it, and the container came
// out with eleven instructions, none of them writing the output register:
// exit 0, no diagnostic, an input mask naming three varyings the ucode never
// read.  Four corpus shaders were mismatching on that for two days.
//
// Fragment only, and entry only.  A vertex `out` with no semantic has no such
// default - the reference does not give it one - and a non-entry function's
// out parameter is an ordinary reference argument, not a shader output.
bool IRBuilder::isDefaultedFragmentOutput(const ParamDecl* param) const
{
    if (!param || !param->semantic.isEmpty()) return false;
    if (param->storage != StorageQualifier::Out &&
        param->storage != StorageQualifier::InOut) return false;
    if (!module_ || module_->shaderStage != ShaderStage::Fragment) return false;
    if (!currentFunctionDecl_ ||
        currentFunctionDecl_->name != module_->entryPointName) return false;
    return true;
}

// ============================================================================
// Function Building
// ============================================================================

void IRBuilder::buildFunction(FunctionDecl* decl)
{
    currentFunction_ = module_->createFunction(decl->name);
    currentFunctionDecl_ = decl;
    currentFunction_->returnType = getIRType(decl->returnType.get());
    currentFunction_->isEntryPoint = (decl->name == module_->entryPointName);

    auto collectReturnOutputs = [&](auto& self, TypeNode* sType, const std::string& pathPrefix) -> void {
        const auto* fields = getStructFields(sType);
        if (!fields) return;
        for (const auto& field : *fields) {
            const std::string fieldPath = pathPrefix.empty() ? field.name : (pathPrefix + "." + field.name);
            if (!field.semantic.isEmpty()) {
                if (getStructFields(field.type.get())) {
                    continue;
                }
                IRParameter output{};
                output.name = fieldPath;
                output.type = getIRType(field.type.get());
                output.valueId = InvalidIRValue;
                output.storage = StorageQualifier::Out;
                output.semanticName = field.semantic.name;
                output.rawSemanticName = field.semantic.rawName;
                output.semanticIndex = field.semantic.index;
                currentFunction_->returnOutputs.push_back(std::move(output));
            } else if (getStructFields(field.type.get())) {
                self(self, field.type.get(), fieldPath);
            }
        }
    };
    if (decl->returnType) {
        collectReturnOutputs(collectReturnOutputs, decl->returnType.get(), "");
    }

    // Build parameters
    for (auto& param : decl->parameters)
    {
        IRParameter irParam;
        irParam.name = param->name;
        irParam.type = getIRType(param->type.get());
        irParam.valueId = currentFunction_->allocateValueId();
        irParam.storage = param->storage;
        irParam.explicitRegisterBank = param->semantic.explicitRegisterBank;
        irParam.explicitRegisterIndex = param->semantic.explicitRegisterIndex;

        if (!param->semantic.isEmpty())
        {
            irParam.semanticName    = param->semantic.name;
            irParam.rawSemanticName = param->semantic.rawName;
            irParam.semanticIndex   = param->semantic.index;
            irParam.inferredSemantic = param->semantic.inferred;
        }
        else if (isDefaultedFragmentOutput(param.get()))
        {
            irParam.semanticName     = "COLOR";
            irParam.rawSemanticName  = "COLOR";
            irParam.semanticIndex    = 0;
            irParam.inferredSemantic = true;
        }

        currentFunction_->parameters.push_back(irParam);

        // A parameter that shadows a file-scope variable is a scope like a
        // local's (t_3af598c8): the global keeps its own binding in the
        // stash - unassigned at entry - so an inlined helper naming it
        // reads the global, and a helper's write to it lands in the stash,
        // not on the parameter.  Before this the entry function's parameter
        // was the one binding the flat map could not tell from the global.
        stashShadowedGlobal(param->name);
        // Map parameter to value
        declToValue_[param.get()] = irParam.valueId;
        nameToValue_[param->name] = irParam.valueId;
    }

    // Create entry block
    currentBlock_ = currentFunction_->createBlock("entry");

    // Build function body
    if (decl->body)
    {
        buildBlockStmt(decl->body.get());
    }

    // Ensure function has a terminator
    if (!currentBlock_->hasTerminator())
    {
        if (currentFunction_->returnType.baseType == IRType::Void)
        {
            emitReturn(InvalidIRValue);
        }
        else
        {
            // Return undefined value (shouldn't happen in well-formed code)
            auto inst = std::make_unique<IRInstruction>(IROp::Undef,
                currentFunction_->allocateValueId(), currentFunction_->returnType);
            currentBlock_->addInstruction(std::move(inst));
            emitReturn(currentFunction_->nextValueId - 1);
        }
    }

    // Clean up
    declToValue_.clear();
    nameToValue_.clear();
    undefinedFieldBases_.clear();
    scope_.clear();   // every per-scope map, in one place (ScopeState)
    currentFunction_ = nullptr;
    currentFunctionDecl_ = nullptr;
    currentBlock_ = nullptr;
}

// ============================================================================
// Statement Building
// ============================================================================

void IRBuilder::buildStmt(StmtNode* stmt)
{
    if (!stmt) return;

    switch (stmt->kind)
    {
    case StmtKind::Block:
        buildBlockStmt(static_cast<BlockStmt*>(stmt));
        break;
    case StmtKind::If:
        buildIfStmt(static_cast<IfStmt*>(stmt));
        break;
    case StmtKind::For:
        buildForStmt(static_cast<ForStmt*>(stmt));
        break;
    case StmtKind::While:
        buildWhileStmt(static_cast<WhileStmt*>(stmt));
        break;
    case StmtKind::DoWhile:
        buildDoWhileStmt(static_cast<DoWhileStmt*>(stmt));
        break;
    case StmtKind::Switch:
        buildSwitchStmt(static_cast<SwitchStmt*>(stmt));
        break;
    case StmtKind::Return:
        buildReturnStmt(static_cast<ReturnStmt*>(stmt));
        break;
    case StmtKind::Break:
        buildBreakStmt(static_cast<BreakStmt*>(stmt));
        break;
    case StmtKind::Continue:
        buildContinueStmt(static_cast<ContinueStmt*>(stmt));
        break;
    case StmtKind::Discard:
        buildDiscardStmt(static_cast<DiscardStmt*>(stmt));
        break;
    case StmtKind::Expr:
        buildExprStmt(static_cast<ExprStmt*>(stmt));
        break;
    case StmtKind::Decl:
        buildDeclStmt(static_cast<DeclStmt*>(stmt));
        break;
    default:
        error("Unknown statement kind");
        break;
    }
}

void IRBuilder::buildBlockStmt(BlockStmt* stmt)
{
    for (auto& s : stmt->statements)
    {
        buildStmt(s.get());
    }
}

std::unordered_map<std::string, IRValueID> IRBuilder::ScopeState::fold() const
{
    std::unordered_map<std::string, IRValueID> keys = names;
    for (const auto& kv : arrays)
        for (size_t i = 0; i < kv.second.size(); ++i)
            if (kv.second[i] != InvalidIRValue)
                keys[kv.first + "[" + std::to_string(i) + "]"] = kv.second[i];
    for (const auto& kv : shadowedGlobals)
        if (kv.second != InvalidIRValue) keys[kv.first + "@"] = kv.second;
    for (const auto& kv : shadowedGlobalArrays)
        for (size_t i = 0; i < kv.second.size(); ++i)
            if (kv.second[i] != InvalidIRValue)
                keys[kv.first + "@[" + std::to_string(i) + "]"] = kv.second[i];
    return keys;
}

void IRBuilder::ScopeState::unfold(std::unordered_map<std::string, IRValueID>& joined)
{
    for (auto it = joined.begin(); it != joined.end(); )
    {
        const std::string& key = it->first;
        const size_t at = key.find('@');
        const size_t br = key.find('[');
        if (at != std::string::npos)
        {
            const std::string name = key.substr(0, at);
            if (key.size() == at + 1)
                shadowedGlobals[name] = it->second;
            else
            {
                const size_t idx = static_cast<size_t>(std::stoul(key.substr(at + 2)));
                auto& vec = shadowedGlobalArrays[name];
                if (vec.size() <= idx) vec.resize(idx + 1, InvalidIRValue);
                vec[idx] = it->second;
            }
            it = joined.erase(it);
            continue;
        }
        if (br != std::string::npos)
        {
            const std::string name = key.substr(0, br);
            const size_t idx = static_cast<size_t>(std::stoul(key.substr(br + 1)));
            auto& vec = arrays[name];
            if (vec.size() <= idx) vec.resize(idx + 1, InvalidIRValue);
            vec[idx] = it->second;
            it = joined.erase(it);
            continue;
        }
        names[key] = it->second;
        ++it;
    }
}

void IRBuilder::buildIfStmt(IfStmt* stmt)
{
    // Evaluate condition
    IRValueID condValue = buildExpr(stmt->condition.get());

    // Create blocks
    IRBasicBlock* thenBlock = currentFunction_->createBlock(makeLabel("if.then"));
    IRBasicBlock* elseBlock = stmt->elseBranch
        ? currentFunction_->createBlock(makeLabel("if.else"))
        : nullptr;
    IRBasicBlock* mergeBlock = currentFunction_->createBlock(makeLabel("if.end"));

    // Emit conditional branch
    emitCondBranch(condValue, thenBlock, elseBlock ? elseBlock : mergeBlock);

    // Snapshot nameToValue_ before either branch runs so we can detect
    // which variables get redefined in then/else and insert merge-time
    // Selects.  Without this, code like `if (cond) c = c * tex;` leaves
    // nameToValue_["c"] pointing at the THEN-only SSA value, which is
    // undefined when the false branch is taken — invalid SSA.
    // ONE snapshot of the whole per-scope state (ScopeState, ir_builder.h -
    // the three-boundary table).  Names, array elements and the shadowed-
    // global stashes all join through the same Select path under their
    // fold() keys: a name is its own key, an element "a[i]", a stashed
    // global "G@", a stashed element "B@[i]".  History of the omissions
    // this replaces: t_cf17f501 (arrays never joined), t_7a4e3b36 review
    // (the stashes added and missed the same way).
    const ScopeState preIf = scope_;
    auto preIfMap = preIf.fold();

    // Build then block
    currentBlock_ = thenBlock;
    buildStmt(stmt->thenBranch.get());
    const bool thenTerminated = currentBlock_->hasTerminator();
    if (!thenTerminated)
    {
        emitBranch(mergeBlock);
    }
    auto postThenMap = scope_.fold();

    // Build else block
    bool elseTerminated = false;
    std::unordered_map<std::string, IRValueID> postElseMap;
    if (stmt->elseBranch)
    {
        scope_ = preIf;  // restore before processing else
        currentBlock_ = elseBlock;
        buildStmt(stmt->elseBranch.get());
        elseTerminated = currentBlock_->hasTerminator();
        if (!elseTerminated)
        {
            emitBranch(mergeBlock);
        }
        postElseMap = scope_.fold();
    }

    // Continue from merge block.  Insert Select(cond, thenVal, elseVal)
    // for each key that diverged.  When a branch terminated early
    // (return / break / continue inside the body), its map doesn't reach
    // the merge - fall back to the pre-if value for that side so the merge
    // keeps the not-terminated branch's update.
    currentBlock_ = mergeBlock;
    scope_ = preIf;
    nameToValue_ = preIfMap;   // the join works over the folded keys; unfold() puts them back

    // A file-scope value that no path bound before this if - a promoted
    // array element, or a stashed global the caller shadows - has no pre-if
    // SSA value: its pre-if value IS THE UNIFORM.  A branch that writes it
    // must select against that load, not take the written arm
    // unconditionally, so the load is materialised here at the merge
    // (value-correct; the reference may place its read elsewhere, so this
    // is a byte-identity question, not a correctness one).  A LOCAL element
    // with no pre-if value keeps the name-join rule: the other path is
    // undefined in Cg and the written arm is taken.  The loads take SSA ids
    // and those ids are the join's sort key, so they are allocated in
    // SORTED key order, never in a hash map's order (t_56ff2244 class).
    std::vector<std::string> unbound;
    for (const auto* post : {&postThenMap, &postElseMap})
        for (const auto& kv : *post)
        {
            const std::string& key = kv.first;
            if (preIfMap.count(key)) continue;
            const size_t at = key.find('@'), br = key.find('[');
            if (at == std::string::npos && br == std::string::npos) continue;   // a plain name
            const std::string base = key.substr(0, at != std::string::npos ? at : br);
            IRGlobal* global = module_->findGlobal(base);
            if (!global) continue;                                             // a local array
            if (at == std::string::npos && !global->type.isArray()) continue;
            unbound.push_back(key);
        }
    std::sort(unbound.begin(), unbound.end());
    unbound.erase(std::unique(unbound.begin(), unbound.end()), unbound.end());
    for (const std::string& key : unbound)
    {
        const size_t at = key.find('@'), br = key.find('[');
        const std::string base = key.substr(0, at != std::string::npos ? at : br);
        const long index = br == std::string::npos ? -1L : std::stol(key.substr(br + 1));
        IRGlobal* global = module_->findGlobal(base);
        IRTypeInfo ty = global->type;
        if (index >= 0) ty.arraySize = 0;
        const IRValueID loadId = currentFunction_->allocateValueId();
        auto load = std::make_unique<IRInstruction>(IROp::LoadUniform, loadId, ty);
        load->targetName = base;
        if (index >= 0)
        {
            load->componentIndex = static_cast<int>(index);
            load->arrayIndexKind = IRInstruction::ArrayIndexKind::Constant;
        }
        load->loc = stmt->loc;
        currentBlock_->addInstruction(std::move(load));
        preIfMap[key] = loadId;
        nameToValue_[key] = loadId;
    }


    auto valueOrPre = [&](const std::unordered_map<std::string, IRValueID>& m,
                          const std::string& name) -> IRValueID
    {
        auto it = m.find(name);
        if (it != m.end()) return it->second;
        auto pre = preIfMap.find(name);
        return (pre != preIfMap.end()) ? pre->second : InvalidIRValue;
    };

    auto getValueType = [&](IRValueID id) -> IRTypeInfo
    {
        if (id == InvalidIRValue) return IRTypeInfo::Void();
        // Constants live in the function's value map.
        if (IRValue* v = currentFunction_->getValue(id))
            return v->type;
        // Parameters do NOT: their ids are allocated and pushed onto
        // `parameters` only, so a merge whose arm is a raw input (`if
        // (c.x > k) r = c; else r = d;`) resolved to Void here, the
        // Select was typed Void, and the general path masked its write
        // to one lane and broadcast lane x into every channel
        // (t_7b20ffdc, measured 2026-09-02 against the reference; the
        // default path happened not to read the width).  The comment
        // above used to claim this branch checked parameters; it never
        // had.
        for (const IRParameter& param : currentFunction_->parameters)
            if (param.valueId == id) return param.type;
        // Then walk instructions for a result match.
        for (const auto& bp : currentFunction_->blocks)
        {
            if (!bp) continue;
            for (const auto& ip : bp->instructions)
            {
                if (ip && ip->result == id) return ip->resultType;
            }
        }
        return IRTypeInfo::Void();
    };

    // Collect every name that appears in either post-map.
    //
    // THE ORDER OF THIS LOOP IS PART OF THE COMPILER'S OUTPUT.  It decides the
    // order the join's Select instructions are emitted in, which decides SSA
    // numbering, the virtual program, and register allocation.  Iterating an
    // unordered_set here made that order the standard library's string-hash
    // bucket order, so an MSVC-built compiler and a gcc-built compiler emitted
    // DIFFERENT PROGRAMS from the same source - measured on 2026-09-06, three
    // of 421 corpus shaders (t_56ff2244).  Never iterate an unordered container
    // into emission.
    //
    // The key is the SMALLEST valid SSA id among a name's pre-if, then and else
    // values: the EARLIEST DEFINITION THAT REACHES THIS JOIN.  Ids are handed
    // out in source order, so this is declaration order: an uninitialised
    // local is still BOUND TO A VALUE at its declaration, so the
    // declaration's id is the smallest one reaching the join - measured,
    // by swapping two declarations and watching the emitted order follow.  It is deliberately NOT a literal declaration-order
    // counter: such a counter would have to be maintained at every site that
    // binds a name (seventeen of them here), and a site missed later would
    // silently reintroduce an arbitrary order for exactly one kind of variable.
    // This key reads only values the join already holds, so it cannot be
    // incomplete.  Do not "improve" it into a declaration counter.
    //
    // Not the then-value alone and not the else-value alone: a name assigned in
    // both arms has two ids, and keying on one arm lets the two arms order the
    // same pair of names oppositely.
    //
    // ONE CASE WHERE THIS IS NOT DECLARATION ORDER TO THE LETTER, so nobody
    // reads it as a promise it does not make: globals are allocated from a
    // separate id space above kGlobalIdBase, so a join over a file-scope global
    // and a local orders the LOCAL first whatever the source says.  Stable and
    // deterministic, which is the requirement here; just not literal source
    // order across that boundary.
    std::unordered_set<std::string> seen;
    for (const auto& kv : postThenMap) seen.insert(kv.first);
    for (const auto& kv : postElseMap) seen.insert(kv.first);

    struct JoinName
    {
        std::string name;
        IRValueID   thenVal;
        IRValueID   elseVal;
        IRValueID   key;
    };
    std::vector<JoinName> joinNames;
    joinNames.reserve(seen.size());

    for (const std::string& name : seen)
    {
        IRValueID preVal = InvalidIRValue;
        if (auto pre = preIfMap.find(name); pre != preIfMap.end())
            preVal = pre->second;

        IRValueID thenVal = thenTerminated ? preVal : valueOrPre(postThenMap, name);
        IRValueID elseVal;
        if (stmt->elseBranch)
            elseVal = elseTerminated ? preVal : valueOrPre(postElseMap, name);
        else
            elseVal = preVal;

        if (thenVal == InvalidIRValue) thenVal = preVal;
        if (elseVal == InvalidIRValue) elseVal = preVal;

        IRValueID key = InvalidIRValue;
        for (IRValueID candidate : {preVal, thenVal, elseVal})
            if (candidate != InvalidIRValue &&
                (key == InvalidIRValue || candidate < key))
                key = candidate;
        if (key == InvalidIRValue)
        {
            // A name with no value on ANY path emits nothing and records
            // nothing - the loop below would take its thenVal == elseVal
            // branch and skip it.  This is not a broken invariant: an early
            // return leaves a joined name (output.color in
            // fp_cf_early_return_f) with no value reaching the join at all.
            // Measured: refusing here instead cost SEVEN corpus shaders.
            // It has no key because it has no definition, and it needs none,
            // because ordering only matters for names that emit a Select.
            continue;
        }
        joinNames.push_back({name, thenVal, elseVal, key});
    }

    // TIE-BREAK ON THE NAME, and it is not decoration.  SSA ids are unique per
    // VALUE, not per NAME: two names that alias the same pre-if value share a
    // minimum reaching id, so the key alone can tie.  A comparator that leaves
    // ties unordered would settle them by the order this vector was built in -
    // which came from an unordered_set - and the hole would survive the fix in
    // exactly the shape it is being closed in.  Names are unique within a join,
    // so (key, name) is a total order and the result cannot depend on input
    // order.
    std::sort(joinNames.begin(), joinNames.end(),
              [](const JoinName& a, const JoinName& b) {
                  if (a.key != b.key) return a.key < b.key;
                  return a.name < b.name;
              });

    for (const JoinName& joined : joinNames)
    {
        const std::string& name = joined.name;
        const IRValueID thenVal = joined.thenVal;
        const IRValueID elseVal = joined.elseVal;
        if (thenVal == elseVal)
        {
            // Both branches converge on the same SSA value (or only one
            // branch redefined and the other kept pre-if).  Just record
            // the final value — no Select needed.
            if (thenVal != InvalidIRValue)
                nameToValue_[name] = thenVal;
            continue;
        }

        IRTypeInfo selType = getValueType(thenVal);
        if (selType.baseType == IRType::Void)
            selType = getValueType(elseVal);

        IRValueID selId = currentFunction_->allocateValueId();
        auto sel = std::make_unique<IRInstruction>(
            IROp::Select, selId, selType);
        sel->addOperand(condValue);
        sel->addOperand(thenVal);
        sel->addOperand(elseVal);
        currentBlock_->addInstruction(std::move(sel));
        nameToValue_[name] = selId;
    }

    // Put the joined values back where they live (fold/unfold, ScopeState).
    scope_.unfold(nameToValue_);
}

namespace {

bool stmtContainsBreakOrContinue(StmtNode* stmt)
{
    if (!stmt) return false;
    switch (stmt->kind)
    {
    case StmtKind::Break:
    case StmtKind::Continue:
        return true;
    case StmtKind::Block: {
        auto* b = static_cast<BlockStmt*>(stmt);
        for (auto& s : b->statements)
            if (stmtContainsBreakOrContinue(s.get())) return true;
        return false;
    }
    case StmtKind::If: {
        auto* i = static_cast<IfStmt*>(stmt);
        return stmtContainsBreakOrContinue(i->thenBranch.get()) ||
               stmtContainsBreakOrContinue(i->elseBranch.get());
    }
    // Inner loops/switch shadow the enclosing loop's break/continue
    // targets, so anything found there does not escape.
    case StmtKind::For:
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::Switch:
        return false;
    default:
        return false;
    }
}

bool exprIsIntLiteral(ExprNode* e, int32_t* out)
{
    bool negate = false;
    while (e && e->kind == ExprKind::Unary)
    {
        auto* u = static_cast<UnaryExpr*>(e);
        if (u->op != UnaryOp::Negate) return false;
        negate = !negate;
        e = u->operand.get();
    }
    if (!e || e->kind != ExprKind::Literal) return false;
    auto* lit = static_cast<LiteralExpr*>(e);
    if (lit->literalKind != LiteralExpr::LiteralKind::Int) return false;
    int64_t v = std::get<int64_t>(lit->value);
    *out = static_cast<int32_t>(negate ? -v : v);
    return true;
}

}  // namespace

bool IRBuilder::tryUnrollStaticFor(ForStmt* stmt)
{
    if (!stmt->init || !stmt->condition || !stmt->increment || !stmt->body)
        return false;

    std::string varName;
    int32_t curVal = 0;

    // Init: `int i = K` (DeclStmt) or `i = K` (ExprStmt assignment).
    if (stmt->init->kind == StmtKind::Decl)
    {
        auto* declStmt = static_cast<DeclStmt*>(stmt->init.get());
        // The unrolled form reasons about ONE induction variable; a
        // multi-declarator init (`for (int i = 0, n = 4; ...)`) is declined
        // here rather than silently unrolled on the first of them.
        if (declStmt->declarations.size() != 1)
            return false;
        const auto& initDecl = declStmt->declarations.front();
        if (!initDecl || initDecl->kind != DeclKind::Variable)
            return false;
        auto* var = static_cast<VarDecl*>(initDecl.get());
        if (!var->initializer) return false;
        if (!exprIsIntLiteral(var->initializer.get(), &curVal)) return false;
        varName = var->name;
    }
    else if (stmt->init->kind == StmtKind::Expr)
    {
        auto* exprStmt = static_cast<ExprStmt*>(stmt->init.get());
        if (!exprStmt->expr || exprStmt->expr->kind != ExprKind::Binary)
            return false;
        auto* be = static_cast<BinaryExpr*>(exprStmt->expr.get());
        if (be->op != BinaryOp::Assign) return false;
        if (be->left->kind != ExprKind::Identifier) return false;
        varName = static_cast<IdentifierExpr*>(be->left.get())->name;
        if (!exprIsIntLiteral(be->right.get(), &curVal)) return false;
    }
    else
    {
        return false;
    }

    // Condition: `varName CMP int_literal`.
    if (stmt->condition->kind != ExprKind::Binary) return false;
    auto* cond = static_cast<BinaryExpr*>(stmt->condition.get());
    BinaryOp cmpOp = cond->op;
    if (cmpOp != BinaryOp::Less && cmpOp != BinaryOp::LessEqual &&
        cmpOp != BinaryOp::Greater && cmpOp != BinaryOp::GreaterEqual &&
        cmpOp != BinaryOp::Equal && cmpOp != BinaryOp::NotEqual)
        return false;
    if (cond->left->kind != ExprKind::Identifier) return false;
    if (static_cast<IdentifierExpr*>(cond->left.get())->name != varName)
        return false;
    int32_t cmpVal = 0;
    if (!exprIsIntLiteral(cond->right.get(), &cmpVal)) return false;

    // Increment: `i++`, `++i`, `i--`, `--i`, `i += K`, `i -= K`.
    int32_t incDelta = 0;
    auto* incExpr = stmt->increment.get();
    if (incExpr->kind == ExprKind::Unary)
    {
        auto* un = static_cast<UnaryExpr*>(incExpr);
        if (un->operand->kind != ExprKind::Identifier) return false;
        if (static_cast<IdentifierExpr*>(un->operand.get())->name != varName)
            return false;
        switch (un->op)
        {
        case UnaryOp::PreIncrement:
        case UnaryOp::PostIncrement: incDelta = 1; break;
        case UnaryOp::PreDecrement:
        case UnaryOp::PostDecrement: incDelta = -1; break;
        default: return false;
        }
    }
    else if (incExpr->kind == ExprKind::Binary)
    {
        auto* be = static_cast<BinaryExpr*>(incExpr);
        if (be->left->kind != ExprKind::Identifier) return false;
        if (static_cast<IdentifierExpr*>(be->left.get())->name != varName)
            return false;
        int32_t k = 0;
        if (!exprIsIntLiteral(be->right.get(), &k)) return false;
        if (be->op == BinaryOp::AddAssign) incDelta = k;
        else if (be->op == BinaryOp::SubAssign) incDelta = -k;
        else return false;
    }
    else
    {
        return false;
    }
    if (incDelta == 0) return false;

    // Body must not break/continue out of this loop level.
    if (stmtContainsBreakOrContinue(stmt->body.get())) return false;

    auto evalCmp = [&](int32_t lhs, int32_t rhs) -> bool {
        switch (cmpOp)
        {
        case BinaryOp::Less:         return lhs <  rhs;
        case BinaryOp::LessEqual:    return lhs <= rhs;
        case BinaryOp::Greater:      return lhs >  rhs;
        case BinaryOp::GreaterEqual: return lhs >= rhs;
        case BinaryOp::Equal:        return lhs == rhs;
        case BinaryOp::NotEqual:     return lhs != rhs;
        default:                     return false;
        }
    };

    constexpr int kMaxUnroll = 64;
    int32_t simulated = curVal;
    int trip = 0;
    while (evalCmp(simulated, cmpVal))
    {
        ++trip;
        if (trip > kMaxUnroll) return false;
        simulated += incDelta;
    }

    for (int iter = 0; iter < trip; ++iter)
    {
        nameToValue_[varName] = createConstant(curVal);
        buildStmt(stmt->body.get());
        if (currentBlock_->hasTerminator()) return true;
        curVal += incDelta;
    }
    nameToValue_[varName] = createConstant(curVal);
    return true;
}

void IRBuilder::buildForStmt(ForStmt* stmt)
{
    // Static-count loops with literal init/condition/increment and no
    // break/continue are fully unrolled into straight-line IR. This
    // matches the reference compiler's lowering and lets the existing emit pipeline
    // handle each iteration as ordinary scalar code.
    if (tryUnrollStaticFor(stmt)) return;

    // Build initializer
    if (stmt->init)
    {
        buildStmt(stmt->init.get());
    }

    // Create blocks
    IRBasicBlock* condBlock = currentFunction_->createBlock(makeLabel("for.cond"));
    IRBasicBlock* bodyBlock = currentFunction_->createBlock(makeLabel("for.body"));
    IRBasicBlock* incBlock = currentFunction_->createBlock(makeLabel("for.inc"));
    IRBasicBlock* endBlock = currentFunction_->createBlock(makeLabel("for.end"));

    // Push loop context
    loopStack_.push({incBlock, endBlock});

    // Jump to condition
    emitBranch(condBlock);

    // Build condition
    currentBlock_ = condBlock;
    if (stmt->condition)
    {
        IRValueID condValue = buildExpr(stmt->condition.get());
        emitCondBranch(condValue, bodyBlock, endBlock);
    }
    else
    {
        // No condition = infinite loop
        emitBranch(bodyBlock);
    }

    // Build body
    currentBlock_ = bodyBlock;
    buildStmt(stmt->body.get());
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(incBlock);
    }

    // Build increment
    currentBlock_ = incBlock;
    if (stmt->increment)
    {
        buildExpr(stmt->increment.get());
    }
    emitBranch(condBlock);

    // Pop loop context
    loopStack_.pop();

    // Continue from end block
    currentBlock_ = endBlock;
}

void IRBuilder::buildWhileStmt(WhileStmt* stmt)
{
    // Create blocks
    IRBasicBlock* condBlock = currentFunction_->createBlock(makeLabel("while.cond"));
    IRBasicBlock* bodyBlock = currentFunction_->createBlock(makeLabel("while.body"));
    IRBasicBlock* endBlock = currentFunction_->createBlock(makeLabel("while.end"));

    // Push loop context
    loopStack_.push({condBlock, endBlock});

    // Jump to condition
    emitBranch(condBlock);

    // Build condition
    currentBlock_ = condBlock;
    IRValueID condValue = buildExpr(stmt->condition.get());
    emitCondBranch(condValue, bodyBlock, endBlock);

    // Build body
    currentBlock_ = bodyBlock;
    buildStmt(stmt->body.get());
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(condBlock);
    }

    // Pop loop context
    loopStack_.pop();

    // Continue from end block
    currentBlock_ = endBlock;
}

void IRBuilder::buildDoWhileStmt(DoWhileStmt* stmt)
{
    // Create blocks
    IRBasicBlock* bodyBlock = currentFunction_->createBlock(makeLabel("dowhile.body"));
    IRBasicBlock* condBlock = currentFunction_->createBlock(makeLabel("dowhile.cond"));
    IRBasicBlock* endBlock = currentFunction_->createBlock(makeLabel("dowhile.end"));

    // Push loop context
    loopStack_.push({condBlock, endBlock});

    // Jump to body
    emitBranch(bodyBlock);

    // Build body
    currentBlock_ = bodyBlock;
    buildStmt(stmt->body.get());
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(condBlock);
    }

    // Build condition
    currentBlock_ = condBlock;
    IRValueID condValue = buildExpr(stmt->condition.get());
    emitCondBranch(condValue, bodyBlock, endBlock);

    // Pop loop context
    loopStack_.pop();

    // Continue from end block
    currentBlock_ = endBlock;
}

void IRBuilder::buildSwitchStmt(SwitchStmt* stmt)
{
    // Switch statements in shaders are relatively rare and complex to lower properly.
    // For now, we emit the switch expression and body as-is, treating it like a block.
    // TODO: Implement proper switch lowering with case value matching

    IRValueID switchValue = buildExpr(stmt->expr.get());
    (void)switchValue;  // Value would be used for case matching

    IRBasicBlock* endBlock = currentFunction_->createBlock(makeLabel("switch.end"));

    // Push loop context (for break)
    loopStack_.push({nullptr, endBlock});

    // For now, just build the body block directly
    // This won't handle case labels correctly, but allows basic compilation
    if (stmt->body)
    {
        buildBlockStmt(stmt->body.get());
    }

    // Pop loop context
    loopStack_.pop();

    // Branch to end if not already terminated
    if (!currentBlock_->hasTerminator())
    {
        emitBranch(endBlock);
    }

    // Continue from end block
    currentBlock_ = endBlock;
}

// Canonicalise a CONFIRMED vector-swizzle component to its xyzw spelling so
// `.rgb` and `.xyz` (and `.stp`) compose the SAME nameToValue_ key: a field
// written under one spelling and read or overwritten under another sees one
// key, and the map holds the last write by construction, using whatever
// branch/inline snapshot machinery the map already has (the branch control
// proves that path rather than assuming it).  Call this ONLY for a confirmed
// swizzle (MemberAccessExpr::isSwizzle); a real struct field named "rgb" or
// "a" keeps its own key, so two members never merge (t_c1d781ba).
//
// Only the LOWERCASE rgba/stpq spellings are mapped - the ones the reference
// treats as equivalent to xyzw (measured: it accepts .rgb and .xyz alike).
// An UPPERCASE output swizzle (.XYZ) is left unchanged: the reference rejects
// it with C1048 "invalid character in swizzle", and our compiler also refuses
// it, unchanged by this slice; that our validator accepts the uppercase
// spelling at all is a separate upstream divergence (t_373d4005).
static std::string canonicalizeSwizzleKey(const std::string& member)
{
    std::string out;
    out.reserve(member.size());
    for (char c : member)
    {
        switch (c)
        {
            case 'r': case 's': out += 'x'; break;
            case 'g': case 't': out += 'y'; break;
            case 'b': case 'p': out += 'z'; break;
            case 'a': case 'q': out += 'w'; break;
            default:            out += c;   break; // x/y/z/w and uppercase kept
        }
    }
    return out;
}

void IRBuilder::buildReturnStmt(ReturnStmt* stmt)
{
    if (stmt->value)
    {
        // For struct returns of a local variable (`return OUT;` where
        // `OUT` is a stack-temp struct populated by member assignments),
        // the StoreOutput emits are deferred to here so the outputs
        // land in struct-field declaration order — matching the
        // reference compiler's parameter-table ordering.  Source
        // statement order would otherwise leak through and reorder
        // the synthetic output entries.
        TypeNode* structType = nullptr;
        std::string baseName;
        if (stmt->value->kind == ExprKind::Identifier)
        {
            auto* retIdent = static_cast<IdentifierExpr*>(stmt->value.get());
            if (retIdent->resolvedDecl &&
                retIdent->resolvedDecl->kind == DeclKind::Variable)
            {
                auto* vd = static_cast<VarDecl*>(retIdent->resolvedDecl);
                if (vd->type && vd->type->baseType == BaseType::Struct)
                {
                    structType = vd->type.get();
                    baseName   = retIdent->name;
                }
            }
        }

        IRValueID retValue = buildExpr(stmt->value.get());

        // Non-struct return with a semantic on the return type itself
        // (e.g. `float4 main(...) : COLOR { return color; }`) — emit a
        // StoreOutput keyed off the function's return semantic.  The
        // entry-point parameter path covers the `out`-keyword shape;
        // this covers the value-return shape.
        if (!structType && currentFunctionDecl_ &&
            !currentFunctionDecl_->returnSemantic.isEmpty() &&
            currentFunction_->isEntryPoint)
        {
            const Semantic& sem = currentFunctionDecl_->returnSemantic;
            auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                InvalidIRValue, currentFunction_->returnType);
            inst->addOperand(retValue);
            inst->semanticName    = sem.name;
            inst->rawSemanticName = sem.rawName;
            inst->semanticIndex   = sem.index;
            currentBlock_->addInstruction(std::move(inst));
        }

        if (structType)
        {
            auto emitStructOutputs = [&](auto& self,
                                         const std::string& keyPrefix,
                                         const std::string& pathPrefix,
                                         TypeNode* sType) -> void {
                const std::vector<StructField>* fields = getStructFields(sType);
                if (!fields) return;

                for (const auto& field : *fields)
                {
                    const std::string fieldKey = keyPrefix + "." + field.name;
                    const std::string fieldPath = pathPrefix.empty() ? field.name : (pathPrefix + "." + field.name);

                    if (field.semantic.isEmpty())
                    {
                        if (field.type && field.type->baseType == BaseType::Struct)
                        {
                            self(self, fieldKey, fieldPath, field.type.get());
                        }
                        continue;
                    }

                    if (field.type && field.type->baseType == BaseType::Struct)
                    {
                        continue;
                    }

                    auto it = nameToValue_.find(fieldKey);
                    IRValueID fieldValue = InvalidIRValue;
                    if (it != nameToValue_.end())
                    {
                        fieldValue = it->second;
                        // Declaring a field makes it an lvalue, not an output
                        // write. Only an assignment replaces its undefined base.
                        if (undefinedFieldBases_.count(fieldValue)) continue;
                        std::fprintf(stderr,
                                     "[ir_builder] return field %s direct -> %%%u\n",
                                     fieldKey.c_str(),
                                     static_cast<unsigned>(fieldValue));
                    }
                    else
                    {
                        // Split struct-member writes such as
                        // `OUT.color.xyz = ...; OUT.color.w = 1;` keep
                        // their fully-qualified partial keys in
                        // `nameToValue_`.  Recompose the parent field
                        // here so `return OUT;` still emits a real
                        // StoreOutput for the whole struct member.
                        //
                        // We only need the vec4 case today; the
                        // ShadowMapping fragment shader writes
                        // `OUT.color.xyz` + `OUT.color.w`.
                        if (field.type && field.type->baseType == BaseType::Float &&
                            field.type->vectorSize == 4)
                        {
                            auto xyzIt = nameToValue_.find(fieldKey + ".xyz");
                            auto wIt   = nameToValue_.find(fieldKey + ".w");
                            if (xyzIt != nameToValue_.end() &&
                                wIt != nameToValue_.end())
                            {
                                IRValueID xyzValue = xyzIt->second;
                                if (auto aliasIt = identityPrefixSwizzleBase_.find(xyzValue);
                                    aliasIt != identityPrefixSwizzleBase_.end())
                                {
                                    std::fprintf(stderr,
                                                 "[ir_builder] unwrap %s.xyz alias %%%u -> %%%u\n",
                                                 fieldKey.c_str(),
                                                 static_cast<unsigned>(xyzValue),
                                                 static_cast<unsigned>(aliasIt->second));
                                    xyzValue = aliasIt->second;
                                }
                                IRTypeInfo fieldTy = getIRType(field.type.get());
                                // Keep the contiguous xyz producer as the
                                // base vector and layer the w=1 override as a
                                // single VecInsert.  This preserves the
                                // producer chain that the NV40 emitter can
                                // lower byte-exactly (`mul` → `mov w=1`),
                                // instead of forcing a synthetic vec
                                // reconstruction that later falls back to
                                // zero-fill on the shader backend.
                                auto inst = std::make_unique<IRInstruction>(
                                    IROp::VecInsert,
                                    currentFunction_->allocateValueId(),
                                    fieldTy);
                                inst->addOperand(xyzValue);
                                inst->addOperand(wIt->second);
                                inst->componentIndex = 3;
                                currentBlock_->addInstruction(std::move(inst));
                                fieldValue = currentFunction_->nextValueId - 1;
                            }
                        }
                    }
                    if (fieldValue == InvalidIRValue) continue;

                    // Stash the field's source type on the StoreOutput
                    // (in `resultType`) so the container emit can size
                    // the synthetic output param entry correctly — e.g.
                    // a `float2 texCoord : TEXCOORD0;` field gets a
                    // float2-typed param slot, not a float4.
                    IRTypeInfo fieldTy = getIRType(field.type.get());
                    auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                        InvalidIRValue, fieldTy);
                    inst->addOperand(fieldValue);
                    inst->semanticName    = field.semantic.name;
                    inst->rawSemanticName = field.semantic.rawName;
                    inst->semanticIndex   = field.semantic.index;
                    inst->fieldName       = fieldPath;
                    currentBlock_->addInstruction(std::move(inst));
                }
            };

            emitStructOutputs(emitStructOutputs, baseName, "", structType);
        }

        emitReturn(retValue);
    }
    else
    {
        emitReturn(InvalidIRValue);
    }
}

void IRBuilder::buildBreakStmt(BreakStmt* stmt)
{
    if (loopStack_.empty())
    {
        error(stmt->loc, "break statement outside of loop");
        return;
    }

    emitBranch(loopStack_.top().breakTarget);
}

void IRBuilder::buildContinueStmt(ContinueStmt* stmt)
{
    if (loopStack_.empty() || !loopStack_.top().continueTarget)
    {
        error(stmt->loc, "continue statement outside of loop");
        return;
    }

    emitBranch(loopStack_.top().continueTarget);
}

void IRBuilder::buildDiscardStmt(DiscardStmt* stmt)
{
    auto inst = std::make_unique<IRInstruction>(IROp::Discard, InvalidIRValue, IRTypeInfo::Void());
    currentBlock_->addInstruction(std::move(inst));
}

void IRBuilder::buildExprStmt(ExprStmt* stmt)
{
    if (stmt->expr)
    {
        buildExpr(stmt->expr.get());
    }
}

// A name the current function binds (a local, or a parameter) that is also
// a file-scope variable: the global's own binding moves aside into the
// stash so an inlined helper can still reach the GLOBAL by that name.
// Captured ONCE: a second binding of the same name (a nested declaration
// inside an inlined helper) must not replace the global's binding with the
// outer one's (review: codex, nested_local_shadow).  InvalidIRValue / an
// empty vector means "shadowed, never assigned": the helper's read then
// falls through to the global load.
void IRBuilder::stashShadowedGlobal(const std::string& name)
{
    if (name.empty() || !module_->findGlobal(name) || shadowedGlobals_.count(name))
        return;
    auto prior = nameToValue_.find(name);
    shadowedGlobals_[name] =
        prior != nameToValue_.end() ? prior->second : InvalidIRValue;
    auto priorArr = localArrayValues_.find(name);
    shadowedGlobalArrays_[name] =
        priorArr != localArrayValues_.end() ? priorArr->second : std::vector<IRValueID>{};
}

void IRBuilder::buildDeclStmt(DeclStmt* stmt)
{
    // Every declarator in the statement, in source order, so `float a, b;`
    // builds both and an initialiser list `float a = x, b = y;` evaluates
    // them left to right (t_a90b1ef1).
    for (const auto& declaration : stmt->declarations)
    {
    if (!declaration) continue;

    if (declaration->kind == DeclKind::Variable)
    {
        auto* varDecl = static_cast<VarDecl*>(declaration.get());

        // A local that shadows a file-scope variable takes the name; the
        // global's own binding (if the program assigned it) moves aside so
        // an inlined helper can still reach it by name.
        stashShadowedGlobal(varDecl->name);
        // Allocate a value for the variable
        IRValueID varId = currentFunction_->allocateValueId();
        declToValue_[varDecl] = varId;
        nameToValue_[varDecl->name] = varId;
        IRTypeInfo varType = getIRType(varDecl->type.get());
        if (varType.isArray() && varType.arraySize > 0)
            localArrayValues_[varDecl->name].assign(
                static_cast<size_t>(varType.arraySize), InvalidIRValue);

        // If there's an initializer, evaluate it
        if (varDecl->initializer)
        {
            IRValueID initValue = buildExpr(varDecl->initializer.get());
            // For now, just use the initializer value as the variable value
            nameToValue_[varDecl->name] = initValue;
            declToValue_[varDecl] = initValue;
        }
        else if (getStructFields(varDecl->type.get()))
        {
            // An uninitialised vector field is a local vector lvalue too.
            // Bind its undefined base at declaration, before either arm of
            // a branch snapshots the map. Swizzle assignment can then build
            // the ordinary VecInsert chain in source order, including
            // overlapping/reordered writes and rgba/stpq aliases. No value
            // or instruction defines this base: unwritten lanes stay absent.
            // Return reads the whole field binding rather than guessing its
            // value from a particular pair of partial-name keys (.xyz/.w).
            auto bindStructBases = [&](auto& self, const std::string& prefix, TypeNode* typeNode) -> void {
                if (const auto* fields = getStructFields(typeNode))
                {
                    for (const auto& field : *fields)
                    {
                        const std::string fieldName = prefix + "." + field.name;
                        if (field.type && field.type->vectorSize > 1)
                        {
                            const IRValueID base = currentFunction_->allocateValueId();
                            nameToValue_[fieldName] = base;
                            undefinedFieldBases_.insert(base);
                        }
                        else if (field.type && field.type->baseType == BaseType::Struct)
                        {
                            self(self, fieldName, field.type.get());
                        }
                    }
                }
            };
            bindStructBases(bindStructBases, varDecl->name, varDecl->type.get());
        }
    }
    }
}

// ============================================================================
// Expression Building
// ============================================================================

IRValueID IRBuilder::buildExpr(ExprNode* expr)
{
    if (!expr) return InvalidIRValue;

    switch (expr->kind)
    {
    case ExprKind::Literal:
        return buildLiteralExpr(static_cast<LiteralExpr*>(expr));
    case ExprKind::Identifier:
        return buildIdentifierExpr(static_cast<IdentifierExpr*>(expr));
    case ExprKind::Binary:
        return buildBinaryExpr(static_cast<BinaryExpr*>(expr));
    case ExprKind::Unary:
        return buildUnaryExpr(static_cast<UnaryExpr*>(expr));
    case ExprKind::Call:
        return buildCallExpr(static_cast<CallExpr*>(expr));
    case ExprKind::MemberAccess:
        return buildMemberAccessExpr(static_cast<MemberAccessExpr*>(expr));
    case ExprKind::Index:
        return buildIndexExpr(static_cast<IndexExpr*>(expr));
    case ExprKind::Ternary:
        return buildTernaryExpr(static_cast<TernaryExpr*>(expr));
    case ExprKind::Cast:
        return buildCastExpr(static_cast<CastExpr*>(expr));
    case ExprKind::Constructor:
        return buildConstructorExpr(static_cast<ConstructorExpr*>(expr));
    default:
        error(expr->loc, "Unknown expression kind");
        return InvalidIRValue;
    }
}

IRValueID IRBuilder::buildLiteralExpr(LiteralExpr* expr)
{
    switch (expr->literalKind)
    {
    case LiteralExpr::LiteralKind::Int:
        return createConstant(static_cast<int32_t>(std::get<int64_t>(expr->value)));
    case LiteralExpr::LiteralKind::Float:
        return createConstant(static_cast<float>(std::get<double>(expr->value)));
    case LiteralExpr::LiteralKind::Bool:
        return createConstant(std::get<bool>(expr->value));
    default:
        error(expr->loc, "Unknown literal kind");
        return InvalidIRValue;
    }
}

IRValueID IRBuilder::buildIdentifierExpr(IdentifierExpr* expr)
{
    // Look up in local names first
    auto it = nameToValue_.find(expr->name);
    if (it != nameToValue_.end())
    {
        return it->second;
    }

    // Look up in globals
    IRGlobal* global = module_->findGlobal(expr->name);
    if (global)
    {
        // A file-scope const folds to its initialiser here, where
        // currentFunction_ exists to own the IRConstant.  This is the
        // reference site for the same reason the uniform case is: the
        // global itself is not an SSA value.
        if (global->storage == StorageQualifier::Const &&
            (!global->initialValue.empty() || !global->initialIntValues.empty()))
        {
            if (!global->initialIntValues.empty())
            {
                if (global->initialIntValues.size() == 1)
                {
                    const int64_t raw = global->initialIntValues[0];
                    switch (global->type.baseType)
                    {
                    case IRType::Bool:
                        return createConstant(raw != 0);
                    case IRType::Int32:
                        return createConstant(static_cast<int32_t>(raw));
                    case IRType::UInt32:
                        return createConstant(static_cast<uint32_t>(raw));
                    default:
                        break;
                    }
                }
            }
            if (!global->initialValue.empty())
            {
                if (global->initialValue.size() == 1)
                {
                    const float raw = global->initialValue[0];
                    switch (global->type.baseType)
                    {
                    case IRType::Bool:
                        return createConstant(raw != 0.0f);
                    case IRType::Int32:
                        return createConstant(static_cast<int32_t>(raw));
                    case IRType::UInt32:
                        return createConstant(static_cast<uint32_t>(raw));
                    default:
                        return createConstant(global->type, raw);
                    }
                }
                return createConstant(global->type, global->initialValue, global->initialIntValues);
            }
        }

        // Emit load from global
        auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
            currentFunction_->allocateValueId(), global->type);
        inst->targetName = global->name;  // Use targetName for uniform name
        currentBlock_->addInstruction(std::move(inst));
        return currentFunction_->nextValueId - 1;
    }

    error(expr->loc, "Unknown identifier: " + expr->name);
    return InvalidIRValue;
}

// Constant-folding helpers.  Inputs are SSA value ids; outputs are
// fresh IRConstant ids when folding succeeds (or InvalidIRValue when
// it doesn't).  Folding handles float scalars + float vectors today —
// FragmentProgram.cg's `const float3 freq = 2.0 * 3.14159 / wave;`
// chain stays in the IRConstant domain throughout, so the back-end
// never sees the divide / multiply on a runtime value.

namespace {
// Extract the float components from an IRConstant.  For scalars,
// returns a single-element vector; for vec types, returns the
// stored components.  Returns false for non-constant or non-float
// types (bool / int / etc. — outside scope today).
bool extractFloatComponents(IRFunction& fn, IRValueID id,
                            std::vector<float>& out)
{
    IRValue* v = fn.getValue(id);
    if (!v) return false;
    auto* c = dynamic_cast<IRConstant*>(v);
    if (!c) return false;
    if (std::holds_alternative<float>(c->value))
    {
        out = { std::get<float>(c->value) };
        return true;
    }
    if (std::holds_alternative<std::vector<float>>(c->value))
    {
        out = std::get<std::vector<float>>(c->value);
        return true;
    }
    return false;
}

bool extractIntegerScalar(IRFunction& fn, IRValueID id, uint32_t& rawBits, bool& isUnsigned)
{
    IRValue* v = fn.getValue(id);
    auto* c = v ? dynamic_cast<IRConstant*>(v) : nullptr;
    if (!c) return false;
    isUnsigned = (c->type.baseType == IRType::UInt32);
    if (std::holds_alternative<int32_t>(c->value))
    {
        rawBits = static_cast<uint32_t>(std::get<int32_t>(c->value));
        return true;
    }
    if (std::holds_alternative<uint32_t>(c->value))
    {
        rawBits = std::get<uint32_t>(c->value);
        isUnsigned = true;
        return true;
    }
    if (std::holds_alternative<bool>(c->value))
    {
        rawBits = std::get<bool>(c->value) ? 1 : 0;
        return true;
    }
    return false;
}

bool extractIntScalar(IRFunction& fn, IRValueID id, int32_t& out)
{
    uint32_t raw = 0;
    bool isUnsigned = false;
    if (extractIntegerScalar(fn, id, raw, isUnsigned))
    {
        out = static_cast<int32_t>(raw);
        return true;
    }
    return false;
}

// Broadcast a 1-component vector to N components by repeating the
// scalar.  No-op when src.size() == n.  Returns false on length
// mismatch (e.g. mixing vec3 + vec4 with no obvious broadcast).
bool broadcastTo(std::vector<float>& v, size_t n)
{
    if (v.size() == n) return true;
    if (v.size() == 1)
    {
        v.resize(n, v[0]);
        return true;
    }
    return false;
}
}  // namespace

IRValueID IRBuilder::tryFoldBinaryOp(IROp op, const IRTypeInfo& resultType,
                                      IRValueID lhs, IRValueID rhs)
{
    uint32_t rawA = 0, rawB = 0;
    bool isUnsignedA = false, isUnsignedB = false;
    if (extractIntegerScalar(*currentFunction_, lhs, rawA, isUnsignedA) &&
        extractIntegerScalar(*currentFunction_, rhs, rawB, isUnsignedB))
    {
        const bool isUnsigned = isUnsignedA || isUnsignedB || resultType.baseType == IRType::UInt32;
        if (isUnsigned)
        {
            const uint32_t ua = rawA;
            const uint32_t ub = rawB;
            uint32_t ur = 0;
            bool valid = true;
            switch (op)
            {
            case IROp::Add: ur = ua + ub; break;
            case IROp::Sub: ur = ua - ub; break;
            case IROp::Mul: ur = ua * ub; break;
            case IROp::Div:
                if (ub == 0) return InvalidIRValue;
                ur = ua / ub;
                break;
            case IROp::Mod:
                if (ub == 0) return InvalidIRValue;
                ur = ua % ub;
                break;
            case IROp::And: ur = ua & ub; break;
            case IROp::Or:  ur = ua | ub; break;
            case IROp::Xor: ur = ua ^ ub; break;
            case IROp::Shl: ur = ua << (ub & 31); break;
            case IROp::Shr:
            case IROp::UShr: ur = ua >> (ub & 31); break;
            default: valid = false; break;
            }
            if (valid)
            {
                if (resultType.baseType == IRType::Int32)
                    return createConstant(static_cast<int32_t>(ur));
                if (resultType.baseType == IRType::Bool)
                    return createConstant(ur != 0);
                if (resultType.baseType == IRType::Float32 || resultType.baseType == IRType::Float16)
                    return createConstant(resultType, static_cast<float>(ur));
                return createConstant(ur);
            }
        }
        else
        {
            const int32_t ia = static_cast<int32_t>(rawA);
            const int32_t ib = static_cast<int32_t>(rawB);
            int32_t r = 0;
            bool valid = true;
            switch (op)
            {
            case IROp::Add: r = static_cast<int32_t>(static_cast<uint32_t>(ia) + static_cast<uint32_t>(ib)); break;
            case IROp::Sub: r = static_cast<int32_t>(static_cast<uint32_t>(ia) - static_cast<uint32_t>(ib)); break;
            case IROp::Mul: r = static_cast<int32_t>(static_cast<uint32_t>(ia) * static_cast<uint32_t>(ib)); break;
            case IROp::Div:
                if (ib == 0 || (ia == INT32_MIN && ib == -1)) return InvalidIRValue;
                r = ia / ib;
                break;
            case IROp::Mod:
                if (ib == 0 || (ia == INT32_MIN && ib == -1)) return InvalidIRValue;
                r = ia % ib;
                break;
            case IROp::And: r = ia & ib; break;
            case IROp::Or:  r = ia | ib; break;
            case IROp::Xor: r = ia ^ ib; break;
            case IROp::Shl:
                r = static_cast<int32_t>(static_cast<uint32_t>(ia) << (ib & 31));
                break;
            case IROp::Shr:
                r = ia >> (ib & 31);
                break;
            case IROp::UShr:
                r = static_cast<int32_t>(static_cast<uint32_t>(ia) >> (ib & 31));
                break;
            default:
                valid = false;
                break;
            }
            if (valid)
            {
                if (resultType.baseType == IRType::UInt32)
                    return createConstant(static_cast<uint32_t>(r));
                if (resultType.baseType == IRType::Bool)
                    return createConstant(r != 0);
                if (resultType.baseType == IRType::Float32 || resultType.baseType == IRType::Float16)
                    return createConstant(resultType, static_cast<float>(r));
                return createConstant(r);
            }
        }
    }

    std::vector<float> a, b;
    if (!extractFloatComponents(*currentFunction_, lhs, a)) return InvalidIRValue;
    if (!extractFloatComponents(*currentFunction_, rhs, b)) return InvalidIRValue;

    // Both operands are float constants.  Broadcasting scalar to vector
    // allows `vec4 * scalar`, `scalar + vec2`, etc., without emitting
    // runtime math for values already known at compile time.
    const size_t n = std::max(a.size(), b.size());
    if (!broadcastTo(a, n) || !broadcastTo(b, n))
        return InvalidIRValue;

    std::vector<float> r(n, 0.0f);
    for (size_t i = 0; i < n; ++i)
    {
        switch (op)
        {
        case IROp::Add: r[i] = a[i] + b[i]; break;
        case IROp::Sub: r[i] = a[i] - b[i]; break;
        case IROp::Mul: r[i] = a[i] * b[i]; break;
        case IROp::Div:
            if (b[i] == 0.0f) return InvalidIRValue;
            r[i] = a[i] / b[i];
            break;
        case IROp::Min: r[i] = std::fmin(a[i], b[i]); break;
        case IROp::Max: r[i] = std::fmax(a[i], b[i]); break;
        default: return InvalidIRValue;
        }
    }

    // Materialise the folded value as a fresh IRConstant.  Scalar
    // results stay as a single float; vector results carry the full
    // component list.
    if (n == 1)
        return createConstant(r[0]);
    return createConstant(resultType, r);
}

IRValueID IRBuilder::tryFoldUnaryOp(IROp op, const IRTypeInfo& resultType,
                                     IRValueID operand)
{
    uint32_t rawA = 0;
    bool isUnsignedA = false;
    if (extractIntegerScalar(*currentFunction_, operand, rawA, isUnsignedA))
    {
        if (op == IROp::Not)
        {
            if (isUnsignedA || resultType.baseType == IRType::UInt32)
                return createConstant(~rawA);
            return createConstant(static_cast<int32_t>(~rawA));
        }
        if (op == IROp::Neg)
        {
            if (isUnsignedA || resultType.baseType == IRType::UInt32)
                return createConstant(static_cast<uint32_t>(-static_cast<int64_t>(rawA)));
            return createConstant(static_cast<int32_t>(-rawA));
        }
        if (op == IROp::IntToFloat)
        {
            if (isUnsignedA)
                return createConstant(resultType, static_cast<float>(rawA));
            else
                return createConstant(resultType, static_cast<float>(static_cast<int32_t>(rawA)));
        }
    }

    std::vector<float> a;
    if (!extractFloatComponents(*currentFunction_, operand, a)) return InvalidIRValue;
    if (op == IROp::FloatToInt)
    {
        if (a.size() == 1)
        {
            if (resultType.baseType == IRType::Bool)
                return createConstant(a[0] != 0.0f);
            if (resultType.baseType == IRType::UInt32)
            {
                if (std::isnan(a[0]) || std::isinf(a[0]))
                    return createConstant(static_cast<uint32_t>(0));
                if (a[0] < 0.0f)
                    return createConstant(static_cast<uint32_t>(static_cast<int64_t>(a[0])));
                return createConstant(static_cast<uint32_t>(static_cast<uint64_t>(std::fmod(a[0], 4294967296.0f))));
            }
            if (std::isnan(a[0]) || std::isinf(a[0]) || a[0] < -2147483648.0f || a[0] >= 2147483648.0f)
                return createConstant(static_cast<int32_t>(-2147483648LL));
            return createConstant(static_cast<int32_t>(a[0]));
        }
        return InvalidIRValue;
    }
    std::vector<float> r(a.size(), 0.0f);
    for (size_t i = 0; i < a.size(); ++i)
    {
        switch (op)
        {
        case IROp::Neg:  r[i] = -a[i]; break;
        case IROp::Abs:  r[i] = std::fabs(a[i]); break;
        case IROp::Sqrt:
            if (a[i] < 0.0f) return InvalidIRValue;  // leave NaN to runtime
            r[i] = std::sqrt(a[i]); break;
        case IROp::RSqrt:
            if (a[i] <= 0.0f) return InvalidIRValue;
            r[i] = 1.0f / std::sqrt(a[i]); break;
        case IROp::Floor: r[i] = std::floor(a[i]); break;
        case IROp::Ceil:  r[i] = std::ceil(a[i]); break;
        case IROp::HalfToFloat:
        case IROp::FloatToHalf:
            r[i] = IRUtils::roundToHalf(a[i]);
            break;
        default: return InvalidIRValue;
        }
    }
    if (a.size() == 1)
        return createConstant(resultType, r[0]);
    return createConstant(resultType, r);
}

IRValueID IRBuilder::tryFoldVecConstruct(const IRTypeInfo& resultType,
                                          const std::vector<IRValueID>& args,
                                          std::optional<BaseType> baseTypeOverride)
{
    const bool floatResult = resultType.elementType == IRType::Float32 ||
                             resultType.elementType == IRType::Float16;
    const bool intResult = resultType.elementType == IRType::Int32 ||
                           resultType.elementType == IRType::UInt32 ||
                           resultType.elementType == IRType::Bool;
    if (!floatResult && !intResult)
        return InvalidIRValue;

    BaseType targetBase = BaseType::Float;
    if (baseTypeOverride.has_value())
    {
        targetBase = *baseTypeOverride;
    }
    else
    {
        switch (resultType.elementType)
        {
        case IRType::Bool:    targetBase = BaseType::Bool; break;
        case IRType::Int32:   targetBase = BaseType::Int; break;
        case IRType::UInt32:  targetBase = BaseType::UInt; break;
        case IRType::Float16: targetBase = BaseType::Half; break;
        case IRType::Float32: targetBase = BaseType::Float; break;
        default: return InvalidIRValue;
        }
    }

    auto extractScalars = [&](IRValueID id, std::vector<ConstEvalScalar>& out) -> bool
    {
        IRValue* v = currentFunction_->getValue(id);
        auto* c = v ? dynamic_cast<IRConstant*>(v) : nullptr;
        if (!c) return false;

        if (std::holds_alternative<int32_t>(c->value))
        {
            out.push_back(ConstEvalScalar::fromInt(std::get<int32_t>(c->value)));
            return true;
        }
        if (std::holds_alternative<uint32_t>(c->value))
        {
            out.push_back(ConstEvalScalar::fromUInt(std::get<uint32_t>(c->value)));
            return true;
        }
        if (std::holds_alternative<bool>(c->value))
        {
            out.push_back(ConstEvalScalar::fromBool(std::get<bool>(c->value)));
            return true;
        }
        if (std::holds_alternative<float>(c->value))
        {
            out.push_back(ConstEvalScalar::fromFloat(std::get<float>(c->value)));
            return true;
        }
        if (std::holds_alternative<std::vector<float>>(c->value))
        {
            const auto& vf = std::get<std::vector<float>>(c->value);
            for (size_t i = 0; i < vf.size(); ++i)
            {
                if (!c->intValues.empty() && i < c->intValues.size())
                {
                    if (c->type.elementType == IRType::Int32)
                        out.push_back(ConstEvalScalar::fromInt(c->intValues[i]));
                    else if (c->type.elementType == IRType::UInt32)
                        out.push_back(ConstEvalScalar::fromUInt(static_cast<uint64_t>(c->intValues[i])));
                    else if (c->type.elementType == IRType::Bool)
                        out.push_back(ConstEvalScalar::fromBool(c->intValues[i] != 0));
                    else
                        out.push_back(ConstEvalScalar::fromFloat(vf[i]));
                }
                else
                {
                    out.push_back(ConstEvalScalar::fromFloat(vf[i]));
                }
            }
            return true;
        }
        return false;
    };

    std::vector<ConstEvalScalar> rawComponents;
    for (IRValueID a : args)
    {
        if (!extractScalars(a, rawComponents))
            return InvalidIRValue;
    }

    if (rawComponents.size() == 1 && resultType.vectorSize > 1)
    {
        rawComponents.assign(static_cast<size_t>(resultType.vectorSize), rawComponents[0]);
    }
    if (rawComponents.size() != static_cast<size_t>(resultType.vectorSize))
        return InvalidIRValue;

    std::vector<float> all;
    std::vector<int64_t> allInts;
    all.reserve(rawComponents.size());

    const bool isIntOrBoolTarget = (targetBase == BaseType::Int ||
                                    targetBase == BaseType::UInt ||
                                    targetBase == BaseType::Bool ||
                                    targetBase == BaseType::Short ||
                                    targetBase == BaseType::UShort ||
                                    targetBase == BaseType::Char ||
                                    targetBase == BaseType::UChar);
    if (isIntOrBoolTarget)
        allInts.reserve(rawComponents.size());

    for (const auto& raw : rawComponents)
    {
        ConstEvalScalar converted;
        if (!convertScalar(raw, targetBase, converted))
            return InvalidIRValue;
        all.push_back(static_cast<float>(converted.asDouble()));
        if (isIntOrBoolTarget)
            allInts.push_back(converted.asInt64());
    }

    return createConstant(resultType, all, allInts);
}

IRValueID IRBuilder::buildBinaryExpr(BinaryExpr* expr)
{
    // Handle assignment specially
    if (TypeOperations::isAssignmentOp(expr->op))
    {
        IRValueID rhsValue = buildExpr(expr->right.get());

        // For compound assignments, compute the new value
        if (expr->op != BinaryOp::Assign)
        {
            IRValueID lhsValue = buildExpr(expr->left.get());
            IROp op;
            switch (expr->op)
            {
            case BinaryOp::AddAssign: op = IROp::Add; break;
            case BinaryOp::SubAssign: op = IROp::Sub; break;
            case BinaryOp::MulAssign: op = IROp::Mul; break;
            case BinaryOp::DivAssign: op = IROp::Div; break;
            case BinaryOp::ModAssign: op = IROp::Mod; break;
            default: op = IROp::Add; break;
            }
            rhsValue = emitBinaryOp(op, getExprType(expr->left.get()), lhsValue, rhsValue);
        }

        return buildAssignment(expr->left.get(), rhsValue);
    }

    // Regular binary expression
    IRValueID leftValue = buildExpr(expr->left.get());
    if (expr->op == BinaryOp::LogicalAnd || expr->op == BinaryOp::LogicalOr)
        ++shortCircuitRhsDepth_;
    IRValueID rightValue = buildExpr(expr->right.get());
    if (expr->op == BinaryOp::LogicalAnd || expr->op == BinaryOp::LogicalOr)
        --shortCircuitRhsDepth_;

    IROp op = binaryOpToIROp(expr->op);
    IRTypeInfo resultType = getExprType(expr);

    if (IRValueID folded = tryFoldBinaryOp(op, resultType, leftValue, rightValue);
        folded != InvalidIRValue)
    {
        return folded;
    }

    return emitBinaryOp(op, resultType, leftValue, rightValue, expr->loc);
}

IRValueID IRBuilder::buildUnaryExpr(UnaryExpr* expr)
{
    IRValueID operandValue = buildExpr(expr->operand.get());

    // Handle increment/decrement specially
    if (expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PreDecrement ||
        expr->op == UnaryOp::PostIncrement || expr->op == UnaryOp::PostDecrement)
    {
        IRTypeInfo type = getExprType(expr->operand.get());
        IRValueID one = createConstant(1.0f);

        IROp op = (expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PostIncrement)
            ? IROp::Add : IROp::Sub;

        IRValueID newValue = emitBinaryOp(op, type, operandValue, one);

        // Store back
        buildAssignment(expr->operand.get(), newValue);

        // Return old or new value based on pre/post
        if (expr->op == UnaryOp::PostIncrement || expr->op == UnaryOp::PostDecrement)
        {
            return operandValue;
        }
        return newValue;
    }

    IROp op = unaryOpToIROp(expr->op);
    IRTypeInfo resultType = getExprType(expr);

    if (IRValueID folded = tryFoldUnaryOp(op, resultType, operandValue);
        folded != InvalidIRValue)
    {
        return folded;
    }

    return emitUnaryOp(op, resultType, operandValue, expr->loc);
}

IRValueID IRBuilder::buildCallExpr(CallExpr* expr)
{
    // Check for built-in function
    auto builtinOp = builtinToIROp(expr->functionName);

    // Build arguments
    std::vector<IRValueID> argValues;
    for (auto& arg : expr->arguments)
    {
        argValues.push_back(buildExpr(arg.get()));
    }

    IRTypeInfo resultType = getExprType(expr);

    if (auto scale = angleConversionScale(expr->functionName);
        scale && argValues.size() == 1 && expr->resolvedFunction == nullptr)
    {
        // resolvedFunction == nullptr here means the registered-builtin path;
        // source declarations shadow it via SymbolTable's overload tie-break.
        IRValueID scaleValue = createConstant(*scale);
        return emitBinaryOp(IROp::Mul, resultType, argValues[0], scaleValue);
    }

    if (builtinOp && expr->resolvedFunction == nullptr)
    {
        if ((expr->functionName == "texDepth2D" ||
             expr->functionName == "texDepth2D_precise") && argValues.size() == 2)
        {
            // t_0970e943: these intrinsics decode packed RGB depth. Keep
            // the factor as a patchable uniform with its compiled default,
            // not a literal that the runtime cannot replace.
            const bool precise = expr->functionName == "texDepth2D_precise";
            const std::string factorName = precise ? "_depth_factor_precise" : "_depth_factor";
            auto* factor = module_->findGlobal(factorName);
            const bool generated = std::find(depthDecodeUniforms_.begin(),
                depthDecodeUniforms_.end(), factorName) != depthDecodeUniforms_.end();
            bool parameterCollision = false;
            for (const auto& parameter : currentFunction_->parameters)
                parameterCollision |= parameter.name == factorName;
            if ((factor && !generated) || parameterCollision)
            {
                error(expr->loc, "depth decode parameter '" + factorName +
                    "' conflicts with a source declaration; refusing ambiguous binding (t_0970e943)");
                return InvalidIRValue;
            }
            if (!factor)
            {
                IRGlobal global;
                global.name = factorName;
                global.type = IRTypeInfo::Float3();
                global.valueId = module_->allocateGlobalId();
                global.storage = StorageQualifier::Uniform;
                global.initialValue = precise
                    ? std::vector<float>{0.003906250465661287f, 0.000015258790881489404f, 0.00000005960465188081798f}
                    : std::vector<float>{0.99609375f, 0.0038909912109375f, 0.000015199184417724609375f};
                module_->addGlobal(global);
                depthDecodeUniforms_.push_back(factorName);
            }
            const auto vectorType = IRTypeInfo::Float3();
            IRValueID sampled = emitInstruction(IROp::TexSample, vectorType, argValues);
            if (precise)
            {
                // Reconstruct integer channel values before applying the
                // precise scale; normalized channels alone are insufficient.
                sampled = emitBinaryOp(IROp::Mul, vectorType, sampled, createConstant(255.0f));
                sampled = emitBinaryOp(IROp::Add, vectorType, sampled, createConstant(0.5f));
                sampled = emitUnaryOp(IROp::Floor, vectorType, sampled);
            }
            const IRValueID factorId = currentFunction_->allocateValueId();
            auto load = std::make_unique<IRInstruction>(IROp::LoadUniform, factorId, vectorType);
            load->targetName = factorName;
            currentBlock_->addInstruction(std::move(load));
            return emitBinaryOp(IROp::Dot, resultType, sampled, factorId);
        }
        // Special handling for 'mul' - determine correct operation based on argument types
        if (expr->functionName == "mul" && expr->arguments.size() == 2)
        {
            IRTypeInfo arg0Type = getExprType(expr->arguments[0].get());
            IRTypeInfo arg1Type = getExprType(expr->arguments[1].get());

            bool arg0IsMatrix = arg0Type.isMatrix();
            bool arg1IsMatrix = arg1Type.isMatrix();
            bool arg0IsVector = arg0Type.isVector();
            bool arg1IsVector = arg1Type.isVector();

            IROp mulOp = IROp::MatVecMul;  // Default

            if (arg0IsMatrix && arg1IsMatrix)
            {
                // Matrix * Matrix
                mulOp = IROp::MatMul;
            }
            else if (arg0IsMatrix && arg1IsVector)
            {
                // Matrix * Vector
                mulOp = IROp::MatVecMul;
            }
            else if (arg0IsVector && arg1IsMatrix)
            {
                // Vector * Matrix
                mulOp = IROp::VecMatMul;
            }

            return emitInstruction(mulOp, resultType, argValues);
        }

        // Emit built-in operation
        return emitInstruction(*builtinOp, resultType, argValues);
    }

    if (expr->resolvedFunction &&
        (currentFunction_->isEntryPoint || !inlineStack_.empty()) &&
        expr->resolvedFunction->kind == DeclKind::Function)
    {
        IRValueID inlined = InvalidIRValue;
        if (inlineUserFunctionCall(expr, argValues, inlined))
            return inlined;
        return InvalidIRValue;
    }

    // User function call
    return emitCall(expr->functionName, resultType, argValues);
}

// Whether a function body mentions `name` as an identifier anywhere - the
// predicate behind the nested-shadow refusal.  Conservative: any mention
// counts, including one that a later pass would have removed.
static void collectIdentifiers(const ExprNode* e, std::unordered_set<std::string>& out);
static void collectIdentifiersStmt(const StmtNode* st, std::unordered_set<std::string>& out)
{
    if (!st) return;
    switch (st->kind)
    {
    case StmtKind::Expr:
        collectIdentifiers(static_cast<const ExprStmt*>(st)->expr.get(), out); break;
    case StmtKind::Decl:
        for (const auto& d : static_cast<const DeclStmt*>(st)->declarations)
            if (d && d->kind == DeclKind::Variable)
                collectIdentifiers(static_cast<const VarDecl*>(d.get())->initializer.get(), out);
        break;
    case StmtKind::Return:
        collectIdentifiers(static_cast<const ReturnStmt*>(st)->value.get(), out); break;
    case StmtKind::Block:
        for (const auto& inner : static_cast<const BlockStmt*>(st)->statements)
            collectIdentifiersStmt(inner.get(), out);
        break;
    case StmtKind::If:
    {
        const auto* ifs = static_cast<const IfStmt*>(st);
        collectIdentifiers(ifs->condition.get(), out);
        collectIdentifiersStmt(ifs->thenBranch.get(), out);
        collectIdentifiersStmt(ifs->elseBranch.get(), out);
        break;
    }
    default: break;
    }
}
static void collectIdentifiers(const ExprNode* e, std::unordered_set<std::string>& out)
{
    if (!e) return;
    switch (e->kind)
    {
    case ExprKind::Identifier:
        out.insert(static_cast<const IdentifierExpr*>(e)->name); break;
    case ExprKind::Binary:
        collectIdentifiers(static_cast<const BinaryExpr*>(e)->left.get(), out);
        collectIdentifiers(static_cast<const BinaryExpr*>(e)->right.get(), out); break;
    case ExprKind::Unary:
        collectIdentifiers(static_cast<const UnaryExpr*>(e)->operand.get(), out); break;
    case ExprKind::Call:
        for (const auto& a : static_cast<const CallExpr*>(e)->arguments) collectIdentifiers(a.get(), out);
        break;
    case ExprKind::MemberAccess:
        collectIdentifiers(static_cast<const MemberAccessExpr*>(e)->object.get(), out); break;
    case ExprKind::Index:
        collectIdentifiers(static_cast<const IndexExpr*>(e)->array.get(), out);
        collectIdentifiers(static_cast<const IndexExpr*>(e)->index.get(), out); break;
    case ExprKind::Ternary:
        collectIdentifiers(static_cast<const TernaryExpr*>(e)->condition.get(), out);
        collectIdentifiers(static_cast<const TernaryExpr*>(e)->thenExpr.get(), out);
        collectIdentifiers(static_cast<const TernaryExpr*>(e)->elseExpr.get(), out); break;
    case ExprKind::Cast:
        collectIdentifiers(static_cast<const CastExpr*>(e)->operand.get(), out); break;
    case ExprKind::Constructor:
        for (const auto& a : static_cast<const ConstructorExpr*>(e)->arguments) collectIdentifiers(a.get(), out);
        break;
    default: break;
    }
}
bool IRBuilder::functionNamesIdentifier(const FunctionDecl* fn, const std::string& name)
{
    std::unordered_set<std::string> ids;
    if (fn && fn->body)
        for (const auto& st : fn->body->statements) collectIdentifiersStmt(st.get(), ids);
    return ids.count(name) != 0;
}

bool IRBuilder::inlineUserFunctionCall(CallExpr* expr,
                                       const std::vector<IRValueID>& args,
                                       IRValueID& result)
{
    result = InvalidIRValue;

    auto* callee = static_cast<FunctionDecl*>(expr->resolvedFunction);
    if (callee->isPrototype() || !callee->body)
    {
        auto defs = functionDefinitionsByName_.find(callee->name);
        if (defs != functionDefinitionsByName_.end())
        {
            for (FunctionDecl* def : defs->second)
            {
                if (def->parameters.size() == args.size())
                {
                    callee = def;
                    break;
                }
            }
        }
    }
    const std::string& name = callee->name;

    constexpr size_t kMaxInlineDepth = 16;
    for (FunctionDecl* active : inlineStack_)
    {
        if (active == callee)
        {
            error(expr->loc, "recursive user function call involving '" + name + "'");
            return false;
        }
    }
    if (inlineStack_.size() >= kMaxInlineDepth)
    {
        error(expr->loc, "user function inline depth exceeded at '" + name + "'");
        return false;
    }

    if (callee->isPrototype() || !callee->body)
    {
        error(expr->loc, "cannot inline user function '" + name + "': no function body");
        return false;
    }
    if (args.size() != callee->parameters.size())
    {
        error(expr->loc, "cannot inline user function '" + name + "': argument count mismatch");
        return false;
    }
    for (const auto& param : callee->parameters)
    {
        if (param->storage == StorageQualifier::Out ||
            param->storage == StorageQualifier::InOut)
        {
            error(expr->loc, "cannot inline user function '" + name +
                             "': out/inout parameters are not supported");
            return false;
        }
    }

    auto savedDecls = declToValue_;
    const ScopeState savedScope = scope_;   // the whole per-scope state, once
    const auto& savedNames = savedScope.names;
    const auto& savedArrays = savedScope.arrays;
    auto savedSwizzles = identityPrefixSwizzleBase_;

    // The names the CALLEE declares - its parameters and every local it
    // declares in its body - are scoped to the inlined body: they shadow
    // for its duration and are dropped afterwards.  Every OTHER name the
    // body binds is a file-scope variable it wrote, and that write must
    // reach the caller (`void gen(float4 p) { G = p * 2; }` then `return G;`
    // reads 2*p in the reference).  The first draft restored the whole map
    // and lost such writes; and it left a callee-local array `B[1]` in the
    // bare-name array map, where it shadowed the caller's global B after
    // the call returned (review: codex).
    std::unordered_set<std::string> calleeScoped;
    for (const auto& param : callee->parameters)
        if (!param->name.empty()) calleeScoped.insert(param->name);
    std::function<void(const StmtNode*)> collectDecls = [&](const StmtNode* st)
    {
        if (!st) return;
        switch (st->kind)
        {
        case StmtKind::Decl:
            for (const auto& d : static_cast<const DeclStmt*>(st)->declarations)
                if (d) calleeScoped.insert(d->name);
            break;
        case StmtKind::Block:
            for (const auto& inner : static_cast<const BlockStmt*>(st)->statements)
                collectDecls(inner.get());
            break;
        case StmtKind::If:
            collectDecls(static_cast<const IfStmt*>(st)->thenBranch.get());
            collectDecls(static_cast<const IfStmt*>(st)->elseBranch.get());
            break;
        default:
            break;
        }
    };
    for (const auto& st : callee->body->statements) collectDecls(st.get());

    for (size_t i = 0; i < callee->parameters.size(); ++i)
    {
        ParamDecl* param = callee->parameters[i].get();
        declToValue_[param] = args[i];
        if (!param->name.empty())
            nameToValue_[param->name] = args[i];
    }

    // Names the caller shadows with a local: for the body, the name means
    // the GLOBAL.  Bind its stashed value, or unbind the name so a read
    // falls through to the global load when nothing ever assigned it.
    for (const auto& kv : shadowedGlobals_)
    {
        if (calleeScoped.count(kv.first)) continue;
        if (kv.second != InvalidIRValue) nameToValue_[kv.first] = kv.second;
        else nameToValue_.erase(kv.first);
    }
    for (const auto& kv : shadowedGlobalArrays_)
    {
        if (calleeScoped.count(kv.first)) continue;
        if (!kv.second.empty()) localArrayValues_[kv.first] = kv.second;
        else localArrayValues_.erase(kv.first);
    }

    // Only the callee's own PARAMETERS exempt a name: they bind at entry,
    // so every occurrence in the body names the parameter.  A local the
    // body declares does not - a read before (or outside) that declaration
    // names the global (review: codex - a nested block declaring G after
    // `D = G` still read the enclosing helper's G).
    std::unordered_set<std::string> calleeParams;
    for (const auto& param : callee->parameters)
        if (!param->name.empty()) calleeParams.insert(param->name);
    for (const auto& scope : inlineScopes_)
        for (const auto& name : scope)
            if (!calleeParams.count(name) && module_->findGlobal(name) &&
                functionNamesIdentifier(callee, name))
            {
                error(expr->loc, "cannot inline user function '" + callee->name +
                                 "': it names file-scope '" + name +
                                 "' while an enclosing helper's parameter or local of that name is in scope; refusing");
                declToValue_ = std::move(savedDecls);
                scope_ = savedScope;
                identityPrefixSwizzleBase_ = std::move(savedSwizzles);
                return false;
            }
    const auto& savedStash = savedScope.shadowedGlobals;
    const auto& savedStashArrays = savedScope.shadowedGlobalArrays;
    inlineScopes_.push_back(calleeScoped);
    inlineStack_.push_back(callee);
    const bool ok = buildInlineFunctionBody(callee, result);
    inlineStack_.pop_back();
    inlineScopes_.pop_back();
    // A stash entry the callee CREATED (its own local shadowing a global) is
    // the callee's scope and ends with it; an entry that existed before the
    // call keeps the callee's update to the global.
    for (auto it = shadowedGlobals_.begin(); it != shadowedGlobals_.end(); )
        if (!savedStash.count(it->first)) it = shadowedGlobals_.erase(it); else ++it;
    for (auto it = shadowedGlobalArrays_.begin(); it != shadowedGlobalArrays_.end(); )
        if (!savedStashArrays.count(it->first)) it = shadowedGlobalArrays_.erase(it); else ++it;

    auto inlineSwizzles = identityPrefixSwizzleBase_;
    const auto bodyNames = nameToValue_;
    const auto bodyArrays = localArrayValues_;
    const auto bodyStash = shadowedGlobals_;
    const auto bodyStashArrays = shadowedGlobalArrays_;
    declToValue_ = std::move(savedDecls);
    scope_ = savedScope;   // restore ALL per-scope state; the rules below carry back what reaches the caller
    // a stash entry that existed before the call keeps the helper's update
    for (const auto& kv : bodyStash) if (savedStash.count(kv.first)) shadowedGlobals_[kv.first] = kv.second;
    for (const auto& kv : bodyStashArrays) if (savedStashArrays.count(kv.first)) shadowedGlobalArrays_[kv.first] = kv.second;
    identityPrefixSwizzleBase_ = std::move(savedSwizzles);
    // Carry the callee's writes to file-scope names back to the caller.
    // A CALLER-LOCAL (or the caller's PARAMETER) that shadows a global of
    // the same name is the caller's binding: the callee wrote the global,
    // which the caller cannot see by that name any more, so the caller's
    // binding is kept (the flat map cannot hold both; the reverse-shadow
    // fixtures pin it).  The STASH is the test: a name has a
    // shadowedGlobals entry exactly when this function binds it itself (a
    // local from buildDeclStmt, a parameter from buildFunction), and the
    // entry outlives every reassignment of that binding.  The earlier
    // test compared the name's current value with the declaration's
    // value, which stopped matching after one `G = G * 2` (review:
    // codex, scope-param-rebound; t_3af598c8) - a reassigned local lost
    // the same way.
    auto callerLocalBinding = [&](const std::string& name, IRValueID) -> bool
    {
        return shadowedGlobals_.count(name) != 0;
    };
    for (const auto& kv : bodyNames)
    {
        if (calleeScoped.count(kv.first)) continue;
        if (!module_->findGlobal(kv.first)) continue;
        auto prior = nameToValue_.find(kv.first);
        if (prior != nameToValue_.end() && callerLocalBinding(kv.first, prior->second))
        {
            // the caller's local keeps the name; the global's new value
            // lives in the stash for the next helper that names it
            shadowedGlobals_[kv.first] = kv.second;
            continue;
        }
        if (prior != nameToValue_.end() && prior->second == kv.second) continue;
        nameToValue_[kv.first] = kv.second;
    }
    // Arrays follow the same rule with their own stash: the callee's write
    // to the file-scope array of the same name must not replace the
    // caller's own array (review: codex - reverse_array.cg returned 2*p
    // where the reference returns the caller's p).
    auto callerLocalArray = [&](const std::string& name) -> bool
    {
        return shadowedGlobalArrays_.count(name) != 0;
    };
    for (const auto& kv : bodyArrays)
    {
        if (calleeScoped.count(kv.first)) continue;
        IRGlobal* global = module_->findGlobal(kv.first);
        if (!global || !global->type.isArray()) continue;
        if (callerLocalArray(kv.first)) { shadowedGlobalArrays_[kv.first] = kv.second; continue; }
        localArrayValues_[kv.first] = kv.second;
    }
    for (const auto& kv : inlineSwizzles)
        identityPrefixSwizzleBase_.try_emplace(kv.first, kv.second);
    // A void helper (a statement-position call that writes globals or
    // nothing) has no result value; its inlined body is the whole effect.
    const bool returnsVoid =
        getIRType(callee->returnType.get()).baseType == IRType::Void;
    return ok && (returnsVoid || result != InvalidIRValue);
}

// The shapes an inlined control-flow statement may NOT contain yet: a
// return inside a branch would need the function's value merged across the
// join (a predicated result), and loops/switch need the general path's
// control-flow milestone.  Each is refused BY NAME so the row stays a named
// gap rather than a silent drop (the reference accepts all of them).
static bool inlineBlockedShape(const StmtNode* stmt, std::string& why)
{
    if (!stmt) return false;
    switch (stmt->kind)
    {
    case StmtKind::Return:   why = "a return inside control flow"; return true;
    case StmtKind::For:
    case StmtKind::While:
    case StmtKind::DoWhile:  why = "a loop"; return true;
    case StmtKind::Switch:
    case StmtKind::Case:
    case StmtKind::Default:  why = "a switch"; return true;
    case StmtKind::Break:
    case StmtKind::Continue: why = "break/continue"; return true;
    case StmtKind::Block:
        for (const auto& inner : static_cast<const BlockStmt*>(stmt)->statements)
            if (inlineBlockedShape(inner.get(), why)) return true;
        return false;
    case StmtKind::If:
    {
        const auto* ifs = static_cast<const IfStmt*>(stmt);
        return inlineBlockedShape(ifs->thenBranch.get(), why) ||
               inlineBlockedShape(ifs->elseBranch.get(), why);
    }
    default:
        return false;
    }
}

bool IRBuilder::buildInlineFunctionBody(FunctionDecl* callee, IRValueID& result)
{
    result = InvalidIRValue;
    bool sawReturn = false;
    const bool returnsVoid =
        getIRType(callee->returnType.get()).baseType == IRType::Void;

    for (size_t i = 0; i < callee->body->statements.size(); ++i)
    {
        StmtNode* stmt = callee->body->statements[i].get();
        if (!stmt) continue;

        if (sawReturn)
        {
            error(stmt->loc, "cannot inline user function '" + callee->name +
                             "': statements after return are not supported");
            return false;
        }

        if (stmt->kind == StmtKind::Return)
        {
            auto* ret = static_cast<ReturnStmt*>(stmt);
            if (!ret->value)
            {
                if (!returnsVoid)
                {
                    error(stmt->loc, "cannot inline user function '" + callee->name +
                                     "': void return");
                    return false;
                }
                sawReturn = true;   // `return;` ends a void body
                continue;
            }
            result = buildExpr(ret->value.get());
            sawReturn = true;
            continue;
        }

        if (stmt->kind == StmtKind::Empty)
            continue;

        // if/else and nested blocks inline through the ordinary statement
        // builder: the if-join merges the callee's locals (and its
        // parameters, which are plain names here) exactly as it merges the
        // entry function's, so `if (mag > 1) { mv /= mag; }` inside a
        // helper lowers to the same select/predicate shape it would at the
        // call site.  The shapes the join cannot carry are refused by name.
        if (stmt->kind == StmtKind::Decl || stmt->kind == StmtKind::Expr ||
            stmt->kind == StmtKind::If || stmt->kind == StmtKind::Block)
        {
            std::string why;
            if (inlineBlockedShape(stmt, why))
            {
                error(stmt->loc, "cannot inline user function '" + callee->name +
                                 "': " + why + " inside its body is not supported");
                return false;
            }
            buildStmt(stmt);
            if (currentBlock_->hasTerminator())
            {
                error(stmt->loc, "cannot inline user function '" + callee->name +
                                 "': statement terminated the caller block");
                return false;
            }
            continue;
        }

        std::string why;
        if (!inlineBlockedShape(stmt, why))
            why = "unsupported control flow";
        error(stmt->loc, "cannot inline user function '" + callee->name +
                         "': " + why + " inside its body is not supported");
        return false;
    }

    if (!sawReturn && !returnsVoid)
    {
        error(callee->loc, "cannot inline user function '" + callee->name +
                           "': no return expression");
        return false;
    }
    return returnsVoid || result != InvalidIRValue;
}

IRValueID IRBuilder::buildMemberAccessExpr(MemberAccessExpr* expr)
{
    // Check for swizzle first
    if (expr->swizzleLength > 0)
    {
        IRValueID objectValue = buildExpr(expr->object.get());
        IRTypeInfo resultType = getExprType(expr);
        bool identityPrefix = true;
        for (int s = 0; s < expr->swizzleLength && s < 4; ++s)
        {
            if (expr->swizzleIndices[s] != s)
            {
                identityPrefix = false;
                break;
            }
        }

        // Constant-fold swizzles of constant vectors — e.g. `v.y`
        // where `v` was const-initialised lifts to a scalar
        // IRConstant.  Multi-lane swizzles (e.g. `.xz` from a vec3)
        // collapse to a vec IRConstant of `swizzleLength` components.
        std::vector<float> objComps;
        if (extractFloatComponents(*currentFunction_, objectValue, objComps))
        {
            std::vector<float> picked;
            picked.reserve(static_cast<size_t>(expr->swizzleLength));
            bool ok = true;
            for (int s = 0; s < expr->swizzleLength; ++s)
            {
                const size_t lane = static_cast<size_t>(expr->swizzleIndices[s]);
                if (lane >= objComps.size()) { ok = false; break; }
                picked.push_back(objComps[lane]);
            }
            if (ok)
            {
                IRValue* v = currentFunction_->getValue(objectValue);
                auto* c = v ? dynamic_cast<IRConstant*>(v) : nullptr;
                if (picked.size() == 1)
                {
                    const size_t lane = static_cast<size_t>(expr->swizzleIndices[0]);
                    if (c && !c->intValues.empty() && lane < c->intValues.size())
                    {
                        const int64_t rawInt = c->intValues[lane];
                        switch (resultType.baseType)
                        {
                        case IRType::Int32:
                            return createConstant(static_cast<int32_t>(rawInt));
                        case IRType::UInt32:
                            return createConstant(static_cast<uint32_t>(rawInt));
                        case IRType::Bool:
                            return createConstant(rawInt != 0);
                        default:
                            return createConstant(resultType, picked[0]);
                        }
                    }
                    switch (resultType.baseType)
                    {
                    case IRType::Int32:
                        return createConstant(static_cast<int32_t>(picked[0]));
                    case IRType::UInt32:
                        return createConstant(static_cast<uint32_t>(picked[0]));
                    case IRType::Bool:
                        return createConstant(picked[0] != 0.0f);
                    default:
                        return createConstant(resultType, picked[0]);
                    }
                }
                std::vector<int64_t> pickedInts;
                if (c && !c->intValues.empty())
                {
                    pickedInts.reserve(static_cast<size_t>(expr->swizzleLength));
                    for (int s = 0; s < expr->swizzleLength; ++s)
                    {
                        const size_t lane = static_cast<size_t>(expr->swizzleIndices[s]);
                        if (lane < c->intValues.size())
                            pickedInts.push_back(c->intValues[lane]);
                    }
                }
                return createConstant(resultType, picked, pickedInts);
            }
        }

        auto inst = std::make_unique<IRInstruction>(IROp::VecShuffle,
            currentFunction_->allocateValueId(), resultType);
        inst->addOperand(objectValue);
        inst->swizzleMask = IRUtils::encodeSwizzle(expr->member);
        currentBlock_->addInstruction(std::move(inst));

        if (identityPrefix)
            identityPrefixSwizzleBase_[currentFunction_->nextValueId - 1] = objectValue;

        return currentFunction_->nextValueId - 1;
    }

    // Struct member access - build a composite name
    // For local variables, use "varname.member" as the key
    if (expr->object->kind == ExprKind::Identifier)
    {
        auto* ident = static_cast<IdentifierExpr*>(expr->object.get());
        std::string compositeName = ident->name + "." +
            (expr->isSwizzle ? canonicalizeSwizzleKey(expr->member) : expr->member);

        // Look up in local names
        auto it = nameToValue_.find(compositeName);
        if (it != nameToValue_.end())
        {
            return it->second;
        }

        // If not found, this is the first read - allocate a value
        IRValueID valueId = currentFunction_->allocateValueId();
        nameToValue_[compositeName] = valueId;

        // Emit a load for input parameters with semantics
        if (ident->resolvedDecl && ident->resolvedDecl->kind == DeclKind::Parameter)
        {
            auto* param = static_cast<ParamDecl*>(ident->resolvedDecl);
            if (param->type && param->type->baseType == BaseType::Struct)
            {
                // Look up semantic info for this struct member using helper
                const std::vector<StructField>* fields = getStructFields(param->type.get());
                if (fields)
                {
                    for (size_t fieldIdx = 0; fieldIdx < fields->size(); ++fieldIdx)
                    {
                        const auto& field = (*fields)[fieldIdx];
                        if (field.name == expr->member)
                        {
                            IRTypeInfo fieldType = getIRType(field.type.get());
                            auto inst = std::make_unique<IRInstruction>(IROp::LoadAttribute,
                                valueId, fieldType);
                            inst->semanticName     = field.semantic.name;
                            inst->rawSemanticName  = field.semantic.rawName;
                            inst->semanticIndex    = field.semantic.isEmpty()
                                ? static_cast<int>(fieldIdx)
                                : field.semantic.index;
                            inst->structParamName  = param->name;       // e.g. "input"
                            inst->fieldName        = expr->member;       // e.g. "pos"
                            currentBlock_->addInstruction(std::move(inst));
                            return valueId;
                        }
                    }
                }
            }
        }

        // Check if this is a uniform struct global - emit LoadUniform with qualified name
        IRGlobal* global = module_->findGlobal(ident->name);
        if (global && global->storage == StorageQualifier::Uniform)
        {
            IRTypeInfo memberType = getExprType(expr);
            auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
                valueId, memberType);
            inst->targetName = compositeName;  // Use qualified name e.g. "u_data.color"
            currentBlock_->addInstruction(std::move(inst));
            return valueId;
        }

        return valueId;
    }

    // For nested member access (a.b.c), recursively resolve
    if (expr->object->kind == ExprKind::MemberAccess)
    {
        // Build name by walking the member access chain
        std::string baseName;
        ExprNode* current = expr->object.get();
        std::vector<std::string> members;
        members.push_back(expr->isSwizzle
                              ? canonicalizeSwizzleKey(expr->member) : expr->member);

        while (current->kind == ExprKind::MemberAccess)
        {
            auto* memberExpr = static_cast<MemberAccessExpr*>(current);
            members.push_back(memberExpr->isSwizzle
                                  ? canonicalizeSwizzleKey(memberExpr->member)
                                  : memberExpr->member);
            current = memberExpr->object.get();
        }

        if (current->kind == ExprKind::Identifier)
        {
            baseName = static_cast<IdentifierExpr*>(current)->name;
        }

        // Build composite name (reverse order)
        std::string compositeName = baseName;
        for (auto it = members.rbegin(); it != members.rend(); ++it)
        {
            compositeName += "." + *it;
        }

        auto nameIt = nameToValue_.find(compositeName);
        if (nameIt != nameToValue_.end())
        {
            return nameIt->second;
        }

        IRValueID valueId = currentFunction_->allocateValueId();
        nameToValue_[compositeName] = valueId;
        return valueId;
    }

    // Fallback - evaluate object and return placeholder
    IRValueID objectValue = buildExpr(expr->object.get());
    (void)objectValue;

    IRTypeInfo resultType = getExprType(expr);
    IRValueID valueId = currentFunction_->allocateValueId();
    return valueId;
}

// A uniform array's index is classified BEFORE any IR is built for it:
//
//   Constant   the whole expression evaluates, at compile time and with
//              checked arithmetic, to one integer - literals, unary minus,
//              + - * / % and parentheses over those.  `u_colors[1 + 1]` is
//              element 2, not a run-time index: the IR-level folder does
//              not fold integer Add, so relying on it misclassified this
//              shape (found in design review).
//   Runtime    something in it is not a compile-time integer (a varying, a
//              call, a cast of one) - the element is chosen on the GPU.
//   Invalid    it IS constant but not a valid one: a non-integer literal,
//              a divisor of zero, or arithmetic that overflows.  Reported,
//              never demoted to Runtime, and never allowed to reach signed
//              overflow on the host.
//
// The value is kept in int64 until the bounds check so a negative index
// and an over-wide unsigned one are both seen as what they are.
namespace
{

enum class IndexEval { Constant, Runtime, Invalid };

// Checked int64 arithmetic without compiler builtins (MSVC has none): the
// bound is tested before the operation, so the host never overflows.
constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();
bool checkedAdd(int64_t a, int64_t b, int64_t& out)
{
    if ((b > 0 && a > kI64Max - b) || (b < 0 && a < kI64Min - b)) return false;
    out = a + b; return true;
}
bool checkedSub(int64_t a, int64_t b, int64_t& out)
{
    if ((b < 0 && a > kI64Max + b) || (b > 0 && a < kI64Min + b)) return false;
    out = a - b; return true;
}
bool checkedMul(int64_t a, int64_t b, int64_t& out)
{
    if (a == 0 || b == 0) { out = 0; return true; }
    if ((a == -1 && b == kI64Min) || (b == -1 && a == kI64Min)) return false;
    if (a > 0) { if (b > 0 ? a > kI64Max / b : b < kI64Min / a) return false; }
    else       { if (b > 0 ? a < kI64Min / b : b < kI64Max / a) return false; }
    out = a * b; return true;
}

// THIS EVALUATOR HOLDS ONE int64 AND CANNOT CARRY A FLOATING VALUE between
// operations.  The reference evaluates a floating index expression in
// float and truncates ONCE on the result: u[1.7 * 2] is element 3,
// u[((float)3 / 2) * 2] is element 3, u[(int)(bool)(float)0.5] is element
// 1 (the fraction is truthy).  Truncating leaf by leaf read those as 2, 2
// and 0 (review: codex found the second and third, the second with no
// float literal in it at all).  So the only floating shapes evaluated
// here are the two whose measured rule is exactly one truncation - a bare
// float literal (or its negation), and a cast to a floating type around a
// constant this evaluator can already hold - and EVERY OTHER constant
// expression with a floating leaf is REFUSED BY NAME rather than read
// wrongly, until the typed constant evaluator (t_65e1b7fa) serves this
// path.  An expression that names a VARIABLE is the run-time path's, typed,
// as before.
static bool isFloatingTarget(const TypeNode* t)
{
    return t && (t->baseType == BaseType::Float || t->baseType == BaseType::Half ||
                 t->baseType == BaseType::Fixed);
}

static bool isFloatLiteral(const ExprNode* e)
{
    return e && e->kind == ExprKind::Literal &&
           static_cast<const LiteralExpr*>(e)->literalKind == LiteralExpr::LiteralKind::Float;
}

static bool containsFloatingLeaf(const ExprNode* e)
{
    if (!e) return false;
    switch (e->kind)
    {
    case ExprKind::Literal:
        return isFloatLiteral(e);
    case ExprKind::Unary:
        return containsFloatingLeaf(static_cast<const UnaryExpr*>(e)->operand.get());
    case ExprKind::Binary:
    {
        auto* b = static_cast<const BinaryExpr*>(e);
        return containsFloatingLeaf(b->left.get()) || containsFloatingLeaf(b->right.get());
    }
    case ExprKind::Cast:
    {
        auto* c = static_cast<const CastExpr*>(e);
        return isFloatingTarget(c->targetType.get()) ||
               containsFloatingLeaf(c->operand.get());
    }
    default:
        return false;
    }
}

// Does the expression name anything that is not a constant (a variable, a
// member, a call, an element)?  Then it is a run-time index whatever else
// it contains.
static bool containsNonConstant(const ExprNode* e)
{
    if (!e) return false;
    switch (e->kind)
    {
    case ExprKind::Literal:
        return false;
    case ExprKind::Unary:
        return containsNonConstant(static_cast<const UnaryExpr*>(e)->operand.get());
    case ExprKind::Binary:
    {
        auto* b = static_cast<const BinaryExpr*>(e);
        return containsNonConstant(b->left.get()) || containsNonConstant(b->right.get());
    }
    case ExprKind::Cast:
        return containsNonConstant(static_cast<const CastExpr*>(e)->operand.get());
    default:
        return true;
    }
}

IndexEval evaluateIntegralIndex(const ExprNode* e, int64_t& out, std::string& why)
{
    if (!e) { why = "array index is missing"; return IndexEval::Invalid; }
    {
        // The two floating shapes this evaluator may hold (see above):
        // a bare float literal or its negation, and a cast to a floating
        // type whose operand carries no floating leaf of its own (u[(float)2]).
        auto* asCast = e->kind == ExprKind::Cast ? static_cast<const CastExpr*>(e) : nullptr;
        const bool bareFloat = isFloatLiteral(e) ||
            (e->kind == ExprKind::Unary &&
             static_cast<const UnaryExpr*>(e)->op == UnaryOp::Negate &&
             isFloatLiteral(static_cast<const UnaryExpr*>(e)->operand.get()));
        // A FIXED cast is never admitted here: fixed clamps to [-2, 2 - 2^-10]
        // before it is read (the reference reads u[(fixed)3] as element 1),
        // and that conversion belongs to the typed evaluator.
        const bool floatingCastOfConstant = asCast &&
            isFloatingTarget(asCast->targetType.get()) &&
            asCast->targetType->baseType != BaseType::Fixed &&
            !containsFloatingLeaf(asCast->operand.get()) &&
            !containsNonConstant(asCast->operand.get());
        // ... and an INTEGRAL cast around a bare float literal, u[(int)2.5],
        // which is one truncation under the cast's own rule (commit A's
        // measured row: element 2; (int)(bool)2.5 would be element 1).
        const bool integralCastOfBareFloat = asCast &&
            !isFloatingTarget(asCast->targetType.get()) &&
            isFloatLiteral(asCast->operand.get());
        if (!bareFloat && !floatingCastOfConstant && !integralCastOfBareFloat &&
            containsFloatingLeaf(e))
        {
            if (containsNonConstant(e))
                return IndexEval::Runtime;
            why = "array index is a constant expression with a floating value; "
                  "this compiler does not evaluate it at compile time yet and "
                  "refuses rather than truncating a subexpression (the reference "
                  "evaluates it in float and truncates the result)";
            return IndexEval::Invalid;
        }
    }
    switch (e->kind)
    {
    case ExprKind::Literal:
    {
        auto* lit = static_cast<const LiteralExpr*>(e);
        if (lit->literalKind == LiteralExpr::LiteralKind::Int)
        {
            out = std::get<int64_t>(lit->value);
            return IndexEval::Constant;
        }
        if (lit->literalKind == LiteralExpr::LiteralKind::Float)
        {
            // A float constant index TRUNCATES toward zero, as int() does
            // and as the reference reads it: u[1.7] is u[1], u[-0.5] is
            // u[0] (measured, t_050bebce).  Arithmetic over float leaves
            // is left to the run-time path (see Binary below), because
            // truncating each leaf first would read u[1.7 * 2] as element
            // 2 where the reference reads element 3.
            // The reference holds a float literal in SINGLE precision: the
            // rounding happens before any conversion (4294967297.0 is
            // 4294967296 by the time an unsigned cast wraps it - codex's
            // boundary rows), so the lexer's double is narrowed first.
            const double d = static_cast<double>(static_cast<float>(std::get<double>(lit->value)));
            if (!(d > -9.2e18 && d < 9.2e18))
            {
                why = "array index constant overflows";
                return IndexEval::Invalid;
            }
            out = static_cast<int64_t>(d);
            return IndexEval::Constant;
        }
        why = "array index must be an integer constant expression";
        return IndexEval::Invalid;
    }
    case ExprKind::Unary:
    {
        auto* u = static_cast<const UnaryExpr*>(e);
        if (u->op != UnaryOp::Negate)
            return IndexEval::Runtime;
        int64_t v = 0;
        const IndexEval r = evaluateIntegralIndex(u->operand.get(), v, why);
        if (r != IndexEval::Constant) return r;
        if (v == kI64Min) { why = "array index constant overflows"; return IndexEval::Invalid; }
        out = -v;
        return IndexEval::Constant;
    }
    case ExprKind::Binary:
    {
        auto* b = static_cast<const BinaryExpr*>(e);
        if (b->op != BinaryOp::Add && b->op != BinaryOp::Sub &&
            b->op != BinaryOp::Mul && b->op != BinaryOp::Div &&
            b->op != BinaryOp::Mod)
            return IndexEval::Runtime;
        int64_t l = 0, r = 0;
        const IndexEval lr = evaluateIntegralIndex(b->left.get(), l, why);
        if (lr == IndexEval::Invalid) return lr;
        const IndexEval rr = evaluateIntegralIndex(b->right.get(), r, why);
        if (rr == IndexEval::Invalid) return rr;
        if (lr == IndexEval::Runtime || rr == IndexEval::Runtime)
            return IndexEval::Runtime;
        int64_t v = 0;
        bool ok = true;
        switch (b->op)
        {
        case BinaryOp::Add: ok = checkedAdd(l, r, v); break;
        case BinaryOp::Sub: ok = checkedSub(l, r, v); break;
        case BinaryOp::Mul: ok = checkedMul(l, r, v); break;
        case BinaryOp::Div:
        case BinaryOp::Mod:
            if (r == 0) { why = "array index constant divides by zero"; return IndexEval::Invalid; }
            if (l == kI64Min && r == -1) { ok = false; break; }
            v = (b->op == BinaryOp::Div) ? l / r : l % r;
            break;
        default: return IndexEval::Runtime;
        }
        if (!ok) { why = "array index constant overflows"; return IndexEval::Invalid; }
        out = v;
        return IndexEval::Constant;
    }
    case ExprKind::Cast:
    {
        // A cast APPLIES ITS TARGET'S SEMANTICS to a constant operand; it
        // is not a pass-through.  (int)(bool)2 is element 1, because
        // (bool)2 is true - a first draft returned the operand unchanged
        // and read element 2, byte-identical to u[2] where the reference is
        // byte-identical to u[1] (found in review).  Integral scalar
        // targets are evaluated with C semantics; any other target is
        // refused by name rather than evaluated wrongly or demoted to a
        // run-time index.  int(uv.x) is still Runtime: the operand decides.
        auto* c = static_cast<const CastExpr*>(e);
        const TypeNode* t = c->targetType.get();
        if (!t || t->vectorSize != 1 || t->matrixRows != 0 || t->arraySize != 0)
        {
            why = "array index constant cast to a non-scalar type is not evaluated";
            return IndexEval::Invalid;
        }
        int64_t v = 0;
        // A FLOAT literal is a constant only under an integral cast, where
        // C truncates it toward zero: the reference compiles u[(int)2.5]
        // as element 2.  A bare float index stays Invalid (the operand
        // rule below), and a float-typed TARGET is refused further down.
        const ExprNode* operand = c->operand.get();
        if (operand && operand->kind == ExprKind::Literal &&
            static_cast<const LiteralExpr*>(operand)->literalKind ==
                LiteralExpr::LiteralKind::Float &&
            (t->baseType == BaseType::Bool || t->baseType == BaseType::Int ||
             t->baseType == BaseType::UInt || t->baseType == BaseType::Short ||
             t->baseType == BaseType::UShort || t->baseType == BaseType::Char ||
             t->baseType == BaseType::UChar))
        {
            // Single precision first, as above: (unsigned char)4294967297.0
            // and (unsigned int)4294967297.0 are element 0 to the reference,
            // because the literal is 4294967296 before the wrap.
            const double d = static_cast<double>(static_cast<float>(
                std::get<double>(static_cast<const LiteralExpr*>(operand)->value)));
            const bool unsignedTarget = t->baseType == BaseType::UInt ||
                                        t->baseType == BaseType::UShort ||
                                        t->baseType == BaseType::UChar;
            if (t->baseType == BaseType::Bool)
                v = d != 0.0 ? 1 : 0;
            else if (d != d || !(d > -9.2e18 && d < 9.2e18))
            {
                why = "array index constant overflows";
                return IndexEval::Invalid;
            }
            else if (!unsignedTarget && !(d > -2147483649.0 && d < 2147483648.0))
                // A SIGNED target out of int32 range: the reference's
                // float-to-int folds to INT_MIN (the x86 indefinite value,
                // measured on t_65e1b7fa), so u[(int)4294967296.0] is out of
                // bounds.  An UNSIGNED target wraps instead - the reference
                // reads u[(unsigned int)4294967296.0] as element 0 (review:
                // codex) - and the narrowing below does that.
                v = INT32_MIN;
            else
                v = static_cast<int64_t>(d);
        }
        else
        {
            const IndexEval r = evaluateIntegralIndex(operand, v, why);
            if (r != IndexEval::Constant) return r;
        }
        switch (t->baseType)
        {
        case BaseType::Bool:   out = (v != 0) ? 1 : 0; return IndexEval::Constant;
        case BaseType::Int:    out = static_cast<int64_t>(static_cast<int32_t>(v));  return IndexEval::Constant;
        case BaseType::UInt:   out = static_cast<int64_t>(static_cast<uint32_t>(v)); return IndexEval::Constant;
        case BaseType::Short:  out = static_cast<int64_t>(static_cast<int16_t>(v));  return IndexEval::Constant;
        case BaseType::UShort: out = static_cast<int64_t>(static_cast<uint16_t>(v)); return IndexEval::Constant;
        case BaseType::Char:   out = static_cast<int64_t>(static_cast<int8_t>(v));   return IndexEval::Constant;
        case BaseType::UChar:  out = static_cast<int64_t>(static_cast<uint8_t>(v));  return IndexEval::Constant;
        case BaseType::Float:
        case BaseType::Half:
            // Only reached for a float/half cast around a constant with no
            // floating leaf of its own (the top of this function refuses
            // every other floating shape by name, fixed included):
            // u[(float)2] is u[2].
            out = v; return IndexEval::Constant;
        default:
            why = "array index constant cast to '" + baseTypeToString(t->baseType) +
                  "' is not evaluated";
            return IndexEval::Invalid;
        }
    }
    default:
        return IndexEval::Runtime;
    }
}

} // namespace

IRValueID IRBuilder::buildIndexExpr(IndexExpr* expr)
{
    // A UNIFORM ARRAY element is a LoadUniform that names how the element
    // was chosen (IRInstruction::arrayIndexKind), never a VecExtract: the
    // VecExtract lowering reads its selector as a LANE and falls back to
    // lane 0, which for an array base would silently read element 0 for
    // every index (t_f9ecd3ac).
    if (expr->array->kind == ExprKind::Identifier)
    {
        auto* ident = static_cast<IdentifierExpr*>(expr->array.get());

        // Provenance comes from the SCOPED binding first: an entry
        // PARAMETER array (`uniform float4 u_colors[4]` in the signature -
        // the SDK's usual spelling) is a name in nameToValue_, and it used
        // to fall through to the generic VecExtract below, which read one
        // LANE of the parameter as the element (the e88 miscompile).  A
        // parameter that shadows a global must not inherit the global's
        // shape either, so the binding table is consulted before findGlobal.
        // ANY scoped binding wins over a global of the same name.  A local
        // or a non-array parameter that shadows a global array is that
        // local or parameter - the first draft consulted the binding only
        // when it was itself a uniform-array parameter and otherwise fell
        // back to findGlobal, so `float4 u = p; return u[2];` under a
        // global `u[4]` read the GLOBAL's element (found in review: the
        // parent refused that shape, the draft accepted it wrongly).
        const IRParameter* arrayParam = nullptr;
        const bool nameIsBound = nameToValue_.count(ident->name) != 0;
        if (nameIsBound && currentFunction_)
        {
            const IRValueID boundId = nameToValue_[ident->name];
            for (const auto& p : currentFunction_->parameters)
            {
                if (p.valueId == boundId && p.type.isArray() &&
                    p.storage == StorageQualifier::Uniform)
                {
                    arrayParam = &p;
                    break;
                }
            }
        }
        IRGlobal* global = nameIsBound ? nullptr : module_->findGlobal(ident->name);
        if (global && !global->type.isArray())
            global = nullptr;

        // A file-scope array (implicitly uniform in Cg) that the program has
        // ASSIGNED is promoted to a value array by the store (t_7a4e3b36):
        // an element written earlier reads the written value, exactly as
        // the reference does (it lists the array as UNDEFINED params and
        // computes from the stores).  Before this, the store was dropped
        // and every read came from the uniform slot - a silent wrong
        // picture on both profiles (GpuBezierTessellation's B[10]).  An
        // element never written falls through to the uniform load below;
        // a run-time index over a written array is refused by name.
        if (global && !arrayParam)
        {
            auto promoted = localArrayValues_.find(ident->name);
            if (promoted != localArrayValues_.end())
            {
                int64_t index = 0;
                std::string why;
                const IndexEval eval = evaluateIntegralIndex(expr->index.get(), index, why);
                if (eval == IndexEval::Invalid)
                {
                    error(expr->loc, why + " ('" + ident->name + "')");
                    return InvalidIRValue;
                }
                if (eval != IndexEval::Constant)
                {
                    error(expr->loc, "file-scope array '" + ident->name +
                                     "' is assigned at run time and read with a run-time index; refusing");
                    return InvalidIRValue;
                }
                if (index >= 0 && index < static_cast<int64_t>(promoted->second.size()) &&
                    promoted->second[static_cast<size_t>(index)] != InvalidIRValue)
                    return promoted->second[static_cast<size_t>(index)];
            }
        }

        if (arrayParam || global)
        {
            const std::string& arrayName = arrayParam ? arrayParam->name : global->name;
            const int64_t arrayCount = arrayParam ? arrayParam->type.arraySize
                                                  : global->type.arraySize;
            int64_t index = 0;
            std::string why;
            const IndexEval eval = evaluateIntegralIndex(expr->index.get(), index, why);
            if (eval == IndexEval::Invalid)
            {
                error(expr->loc, why + " ('" + arrayName + "')");
                return InvalidIRValue;
            }

            // The run-time index is built BEFORE the load's own id is
            // allocated, and the load's id is what is returned - a first
            // draft allocated the load first and returned "the last id",
            // which was the index expression's; the load then had no users,
            // was eliminated, and the shader returned the INDEX as its
            // colour with exit 0 (caught by fp_array_uniform_dynamic's
            // expectation of a refusal).
            IRValueID indexValue = InvalidIRValue;
            int elementIndex = 0;
            if (eval == IndexEval::Constant)
            {
                // Bounds are checked on the evaluated int64, before it is
                // narrowed, and out of range REFUSES - the reference does
                // (C1068 array index out of bounds) and a clamp would
                // silently read a different element.
                if (index < 0 || index >= arrayCount)
                {
                    error(expr->loc, "array index " + std::to_string(index) +
                          " out of bounds for '" + arrayName + "[" +
                          std::to_string(arrayCount) + "]'");
                    return InvalidIRValue;
                }
                elementIndex = static_cast<int>(index);
            }
            else
            {
                indexValue = buildExpr(expr->index.get());
                if (indexValue == InvalidIRValue)
                    return InvalidIRValue;
                // A FLOAT or HALF index is int(index): the reference emits
                // the same address-register load for u[idx] and u[int(idx)]
                // with a float idx (t_050bebce).  The cast is what the
                // lowering folds into the ARL (vertex) or, when the value
                // is a constant, into the element read on either profile.
                const IRTypeInfo indexType = getExprType(expr->index.get());
                if (indexType.baseType == IRType::Float32 ||
                    indexType.baseType == IRType::Float16)
                {
                    const IRValueID castId = currentFunction_->allocateValueId();
                    auto cast = std::make_unique<IRInstruction>(
                        IROp::FloatToInt, castId, IRTypeInfo::Int());
                    cast->addOperand(indexValue);
                    cast->loc = expr->index->loc;
                    currentBlock_->addInstruction(std::move(cast));
                    indexValue = castId;
                }
            }

            IRTypeInfo resultType = getExprType(expr);
            const IRValueID loadId = currentFunction_->allocateValueId();
            auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
                loadId, resultType);
            inst->targetName = arrayName;
            if (eval == IndexEval::Constant)
            {
                inst->componentIndex = elementIndex;
                inst->arrayIndexKind = IRInstruction::ArrayIndexKind::Constant;
            }
            else
            {
                inst->addOperand(indexValue);
                inst->componentIndex = -1;
                inst->arrayIndexKind = IRInstruction::ArrayIndexKind::Dynamic;
            }
            currentBlock_->addInstruction(std::move(inst));
            return loadId;
        }

        auto localIt = localArrayValues_.find(ident->name);
        if (localIt != localArrayValues_.end())
        {
            IRValueID indexValue = buildExpr(expr->index.get());
            if (indexValue == InvalidIRValue)
                return InvalidIRValue;

            int32_t constIndex = 0;
            if (extractIntScalar(*currentFunction_, indexValue, constIndex))
            {
                if (constIndex >= 0 &&
                    constIndex < static_cast<int32_t>(localIt->second.size()))
                    return localIt->second[static_cast<size_t>(constIndex)];
                error(expr->loc, "array index out of bounds");
                return InvalidIRValue;
            }

            error(expr->loc, "local array dynamic indexing is not supported by this profile");
            return InvalidIRValue;
        }
    }

    // Fallback: vector component extraction (e.g., vec.x, vec[i])
    IRValueID arrayValue = buildExpr(expr->array.get());
    IRValueID indexValue = buildExpr(expr->index.get());

    IRTypeInfo resultType = getExprType(expr);

    int32_t constIdx = 0;
    if (extractIntScalar(*currentFunction_, indexValue, constIdx) && constIdx >= 0)
    {
        IRValue* av = currentFunction_->getValue(arrayValue);
        if (auto* ac = dynamic_cast<IRConstant*>(av))
        {
            const size_t uIdx = static_cast<size_t>(constIdx);
            if (!ac->intValues.empty() && uIdx < ac->intValues.size())
            {
                const int64_t rawInt = ac->intValues[uIdx];
                switch (resultType.baseType)
                {
                case IRType::Int32:  return createConstant(static_cast<int32_t>(rawInt));
                case IRType::UInt32: return createConstant(static_cast<uint32_t>(rawInt));
                case IRType::Bool:   return createConstant(rawInt != 0);
                default: break;
                }
            }
            std::vector<float> comps;
            if (extractFloatComponents(*currentFunction_, arrayValue, comps) && uIdx < comps.size())
            {
                switch (resultType.baseType)
                {
                case IRType::Int32:  return createConstant(static_cast<int32_t>(comps[uIdx]));
                case IRType::UInt32: return createConstant(static_cast<uint32_t>(comps[uIdx]));
                case IRType::Bool:   return createConstant(comps[uIdx] != 0.0f);
                default:             return createConstant(resultType, comps[uIdx]);
                }
            }
        }
    }

    auto inst = std::make_unique<IRInstruction>(IROp::VecExtract,
        currentFunction_->allocateValueId(), resultType);
    inst->addOperand(arrayValue);
    inst->addOperand(indexValue);
    currentBlock_->addInstruction(std::move(inst));

    return currentFunction_->nextValueId - 1;
}

IRValueID IRBuilder::buildTernaryExpr(TernaryExpr* expr)
{
    IRValueID condValue = buildExpr(expr->condition.get());
    IRValueID thenValue = buildExpr(expr->thenExpr.get());
    IRValueID elseValue = buildExpr(expr->elseExpr.get());

    IRTypeInfo resultType = getExprType(expr);

    auto inst = std::make_unique<IRInstruction>(IROp::Select,
        currentFunction_->allocateValueId(), resultType);
    inst->addOperand(condValue);
    inst->addOperand(thenValue);
    inst->addOperand(elseValue);
    currentBlock_->addInstruction(std::move(inst));

    return currentFunction_->nextValueId - 1;
}

IRValueID IRBuilder::buildCastExpr(CastExpr* expr)
{
    IRValueID operandValue = buildExpr(expr->operand.get());
    IRTypeInfo targetType = getIRType(expr->targetType.get());
    IRTypeInfo sourceType = getExprType(expr->operand.get());

    if (sourceType.baseType == targetType.baseType &&
        sourceType.elementType == targetType.elementType)
    {
        return operandValue;
    }

    if (targetType.isVector())
    {
        if (IRValueID folded = tryFoldVecConstruct(targetType, { operandValue },
                expr->targetType ? std::optional<BaseType>(expr->targetType->baseType) : std::nullopt);
            folded != InvalidIRValue)
        {
            return folded;
        }
    }

    // A CAST FROM A SCALAR TO A VECTOR IS A BROADCAST.  `(float4)d` and
    // `(half4)d` are the cast spelling of `float4(d,d,d,d)`, and the
    // reference lowers all three of these to ONE instruction:
    //
    //   float d = a.x; return (float4)d;        MOV R0.xyzw, TEX0.xxxx
    //   half  d = a.x; return (half4)d;         MOV H0.xyzw, TEX0.xxxx  prec=1
    //   half  d = a.x; return half4(d,d,d,d);   MOV H0.xyzw, TEX0.xxxx  prec=1
    //
    // Only the constructor spelling reached VecConstruct here; the cast fell
    // through to the default Bitcast below, which the NV40 lowering refuses
    // outright ("unsupported IR op bitcast").  It is not a half-only gap -
    // the float row above refused too - and it is 22 rows of the
    // reference-SDK sweep (t_cde25bad).
    //
    // The broadcast is built in the SOURCE's element type and any precision
    // conversion is left to the vector path below, so `(float4)h` is one
    // splat plus the ordinary hvec4 -> vec4 conversion rather than a second
    // scalar-conversion path to keep in step with this one.
    if (targetType.isVector() && sourceType.isScalar() &&
        targetType.componentCount() > 1)
    {
        IRTypeInfo splatType = targetType;
        splatType.elementType = sourceType.baseType;
        auto splat = std::make_unique<IRInstruction>(IROp::VecConstruct,
            currentFunction_->allocateValueId(), splatType);
        for (int i = 0; i < targetType.componentCount(); ++i)
            splat->addOperand(operandValue);
        currentBlock_->addInstruction(std::move(splat));
        operandValue = currentFunction_->nextValueId - 1;
        sourceType = splatType;
        if (sourceType.elementType == targetType.elementType)
            return operandValue;
    }

    // Determine conversion operation
    IROp op = IROp::Bitcast;  // Default
    const IRType srcElem = sourceType.isVector() ? sourceType.elementType : sourceType.baseType;
    const IRType dstElem = targetType.isVector() ? targetType.elementType : targetType.baseType;

    if ((srcElem == IRType::Int32 || srcElem == IRType::UInt32 || srcElem == IRType::Bool) &&
        (dstElem == IRType::Float32 || dstElem == IRType::Float16))
    {
        op = IROp::IntToFloat;
    }
    else if ((srcElem == IRType::Float32 || srcElem == IRType::Float16) &&
             (dstElem == IRType::Int32 || dstElem == IRType::UInt32 || dstElem == IRType::Bool))
    {
        op = IROp::FloatToInt;
    }
    else if (srcElem == IRType::Float32 && dstElem == IRType::Float16)
    {
        op = IROp::FloatToHalf;
    }
    else if (srcElem == IRType::Float16 && dstElem == IRType::Float32)
    {
        op = IROp::HalfToFloat;
    }

    if (IRValueID folded = tryFoldUnaryOp(op, targetType, operandValue);
        folded != InvalidIRValue)
    {
        return folded;
    }
    return emitUnaryOp(op, targetType, operandValue);
}

IRValueID IRBuilder::buildConstructorExpr(ConstructorExpr* expr)
{
    IRTypeInfo resultType = getIRType(expr->constructedType.get());

    // Build argument values
    std::vector<IRValueID> argValues;
    for (auto& arg : expr->arguments)
    {
        argValues.push_back(buildExpr(arg.get()));
    }

    // Single argument handling
    if (argValues.size() == 1)
    {
        IRTypeInfo argType = getExprType(expr->arguments[0].get());
        if (argType.componentCount() == resultType.componentCount())
        {
            // Same type - just return it
            if (argType.baseType == resultType.baseType &&
                argType.elementType == resultType.elementType)
            {
                return argValues[0];
            }

            if (resultType.isVector())
            {
                if (IRValueID folded = tryFoldVecConstruct(resultType, argValues,
                        expr->constructedType ? std::optional<BaseType>(expr->constructedType->baseType) : std::nullopt);
                    folded != InvalidIRValue)
                {
                    return folded;
                }
            }

            // Different base type - need type conversion (e.g., half(1.0) = float->half)
            IROp convOp = IROp::Bitcast;  // Default
            const IRType srcElem = argType.isVector() ? argType.elementType : argType.baseType;
            const IRType dstElem = resultType.isVector() ? resultType.elementType : resultType.baseType;

            if (srcElem == IRType::Float32 && dstElem == IRType::Float16)
            {
                convOp = IROp::FloatToHalf;
            }
            else if (srcElem == IRType::Float16 && dstElem == IRType::Float32)
            {
                convOp = IROp::HalfToFloat;
            }
            else if ((srcElem == IRType::Int32 || srcElem == IRType::UInt32 || srcElem == IRType::Bool) &&
                     (dstElem == IRType::Float32 || dstElem == IRType::Float16))
            {
                convOp = IROp::IntToFloat;
            }
            else if ((srcElem == IRType::Float32 || srcElem == IRType::Float16) &&
                     (dstElem == IRType::Int32 || dstElem == IRType::UInt32 || dstElem == IRType::Bool))
            {
                convOp = IROp::FloatToInt;
            }

            if (IRValueID folded = tryFoldUnaryOp(convOp, resultType, argValues[0]);
                folded != InvalidIRValue)
            {
                return folded;
            }
            return emitUnaryOp(convOp, resultType, argValues[0]);
        }
    }

    // Vector construction
    if (resultType.isVector())
    {
        if (IRValueID folded = tryFoldVecConstruct(resultType, argValues,
                expr->constructedType ? std::optional<BaseType>(expr->constructedType->baseType) : std::nullopt);
            folded != InvalidIRValue)
        {
            return folded;
        }
        auto inst = std::make_unique<IRInstruction>(IROp::VecConstruct,
            currentFunction_->allocateValueId(), resultType);
        for (IRValueID arg : argValues)
        {
            inst->addOperand(arg);
        }
        currentBlock_->addInstruction(std::move(inst));
        return currentFunction_->nextValueId - 1;
    }

    // Matrix construction from vectors (e.g., half3x3(v1, v2, v3))
    if (resultType.isMatrix())
    {
        // Verify we have the right number of row/column vectors
        int expectedVectors = resultType.matrixRows; // Assume row-major: N rows of M-component vectors
        if (argValues.size() == (size_t)expectedVectors)
        {
            auto inst = std::make_unique<IRInstruction>(IROp::MatConstruct,
                currentFunction_->allocateValueId(), resultType);
            for (IRValueID arg : argValues)
            {
                inst->addOperand(arg);
            }
            currentBlock_->addInstruction(std::move(inst));
            return currentFunction_->nextValueId - 1;
        }
        // Could also be constructed from all scalar components
        int totalComponents = resultType.matrixRows * resultType.matrixCols;
        if (argValues.size() == (size_t)totalComponents)
        {
            auto inst = std::make_unique<IRInstruction>(IROp::MatConstruct,
                currentFunction_->allocateValueId(), resultType);
            for (IRValueID arg : argValues)
            {
                inst->addOperand(arg);
            }
            currentBlock_->addInstruction(std::move(inst));
            return currentFunction_->nextValueId - 1;
        }
    }

    // Single scalar broadcast
    if (argValues.size() == 1)
    {
        return argValues[0];
    }

    error(expr->loc, "Complex constructor not yet implemented");
    return InvalidIRValue;
}

IRValueID IRBuilder::buildAssignment(ExprNode* target, IRValueID value)
{
    value = coerceAssignmentValue(target, value);

    if (target->kind == ExprKind::Identifier)
    {
        auto* ident = static_cast<IdentifierExpr*>(target);
        nameToValue_[ident->name] = value;

        // Check if this is an out parameter with a semantic - emit StoreOutput
        if (ident->resolvedDecl && ident->resolvedDecl->kind == DeclKind::Parameter)
        {
            auto* param = static_cast<ParamDecl*>(ident->resolvedDecl);
            const bool defaultedOut = isDefaultedFragmentOutput(param);
            if ((param->storage == StorageQualifier::Out ||
                 param->storage == StorageQualifier::InOut) &&
                (!param->semantic.isEmpty() || defaultedOut))
            {
                // Emit a store output instruction for the out parameter
                auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                    InvalidIRValue, IRTypeInfo::Void());
                inst->addOperand(value);
                inst->semanticName    = defaultedOut ? "COLOR" : param->semantic.name;
                inst->rawSemanticName = defaultedOut ? "COLOR" : param->semantic.rawName;
                inst->semanticIndex   = defaultedOut ? 0 : param->semantic.index;
                currentBlock_->addInstruction(std::move(inst));
            }
        }
        return value;
    }

    // Handle struct member assignment
    if (target->kind == ExprKind::MemberAccess)
    {
        auto* memberExpr = static_cast<MemberAccessExpr*>(target);

        // Single-lane swizzle write-back: `c.a = scalar;` rewrites a
        // single lane of an existing vector value.  Lower as VecInsert
        // against the variable's prior SSA value.  Multi-lane swizzle
        // assignment (`c.rgb = vec3;`) is split into per-lane VecInserts
        // — one per swizzle index — so the back-end sees a sequence of
        // single-lane overrides.
        if (memberExpr->isSwizzle && memberExpr->swizzleLength >= 1)
        {
            // A field that already has a whole value needs the same
            // read/modify/write as a local vector. Storing r.color.rgb
            // under a separate key leaves return r reading stale r.color.
            ExprNode* base = memberExpr->object.get();
            std::vector<std::string> fields;
            while (base->kind == ExprKind::MemberAccess)
            {
                auto* field = static_cast<MemberAccessExpr*>(base);
                if (field->isSwizzle) break;
                fields.push_back(field->member);
                base = field->object.get();
            }
            auto* ident = base->kind == ExprKind::Identifier
                ? static_cast<IdentifierExpr*>(base) : nullptr;
            std::string objectName = ident ? ident->name : std::string();
            for (auto it = fields.rbegin(); it != fields.rend(); ++it)
                objectName += "." + *it;
            auto nvIt = nameToValue_.find(objectName);
            if (nvIt != nameToValue_.end())
            {
                IRValueID currentVec = nvIt->second;
                IRTypeInfo vecType = getExprType(memberExpr->object.get());

                for (int s = 0; s < memberExpr->swizzleLength; ++s)
                {
                    const int lane = memberExpr->swizzleIndices[s];
                    IRValueID scalar = value;
                    if (memberExpr->swizzleLength > 1)
                    {
                        // For multi-lane swizzle, RHS is a vector; pull out
                        // the matching scalar lane via VecExtract.
                        auto extractInst = std::make_unique<IRInstruction>(
                            IROp::VecExtract,
                            currentFunction_->allocateValueId(),
                            IRTypeInfo::Float());
                        extractInst->addOperand(value);
                        extractInst->componentIndex = s;
                        scalar = extractInst->result;
                        currentBlock_->addInstruction(std::move(extractInst));
                    }

                    auto insertInst = std::make_unique<IRInstruction>(
                        IROp::VecInsert,
                        currentFunction_->allocateValueId(),
                        vecType);
                    insertInst->addOperand(currentVec);
                    insertInst->addOperand(scalar);
                    insertInst->componentIndex = lane;
                    currentVec = insertInst->result;
                    currentBlock_->addInstruction(std::move(insertInst));
                }

                nameToValue_[objectName] = currentVec;

                // If the underlying identifier is an out parameter
                // carrying a semantic, fall through to the regular
                // identifier-assignment StoreOutput emit so the lane-
                // overridden vec value propagates to the output.
                //
                // Sequential swizzle-writes (`oColor.rgb = ...;
                // oColor.a = ...;`) each fire this branch, so the
                // emitted StoreOutputs would stack with intermediate
                // values reaching the back-end.  Suppress the chain:
                // if the last instruction in the current block is
                // already a StoreOutput to this same semantic, just
                // update its operand instead of appending a fresh
                // store — the latest VecInsert chain holds all the
                // accumulated lane writes.
                if (fields.empty() && ident && ident->resolvedDecl &&
                    ident->resolvedDecl->kind == DeclKind::Parameter)
                {
                    auto* param = static_cast<ParamDecl*>(ident->resolvedDecl);
                    const bool defaultedOut = isDefaultedFragmentOutput(param);
                    const std::string outSemName =
                        defaultedOut ? std::string("COLOR") : param->semantic.name;
                    const std::string outSemRaw =
                        defaultedOut ? std::string("COLOR") : param->semantic.rawName;
                    const int outSemIndex =
                        defaultedOut ? 0 : param->semantic.index;
                    if ((param->storage == StorageQualifier::Out ||
                         param->storage == StorageQualifier::InOut) &&
                        (!param->semantic.isEmpty() || defaultedOut))
                    {
                        // Drop any earlier StoreOutput in this block
                        // that targets the same semantic — this
                        // swizzle-write supersedes it.  Walk forward
                        // erasing matches; nothing depends on a
                        // StoreOutput's side effect within the IR
                        // (StoreOutputs have no result), so removal
                        // is safe.  Crucially this fires for
                        // chained `oColor.rgb = ...; oColor.a = ...;`
                        // where the rgb StoreOutput is no longer the
                        // last instruction (the alpha VecInsert was
                        // already appended above).
                        auto& insts = currentBlock_->instructions;
                        for (auto it = insts.begin(); it != insts.end(); )
                        {
                            const auto& cur = *it;
                            if (cur && cur->op == IROp::StoreOutput &&
                                cur->semanticName == outSemName &&
                                cur->semanticIndex == outSemIndex)
                            {
                                it = insts.erase(it);
                            }
                            else
                            {
                                ++it;
                            }
                        }

                        auto inst = std::make_unique<IRInstruction>(
                            IROp::StoreOutput,
                            InvalidIRValue, IRTypeInfo::Void());
                        inst->addOperand(currentVec);
                        inst->semanticName    = outSemName;
                        inst->rawSemanticName = outSemRaw;
                        inst->semanticIndex   = outSemIndex;
                        currentBlock_->addInstruction(std::move(inst));
                    }
                }
                // The lvalue now holds the updated vector, but the value
                // of an assignment expression is its coerced RHS.
                return value;
            }
        }

        // Build the composite name for the member
        if (memberExpr->object->kind == ExprKind::Identifier)
        {
            auto* ident = static_cast<IdentifierExpr*>(memberExpr->object.get());
            std::string compositeName = ident->name + "." +
                (memberExpr->isSwizzle ? canonicalizeSwizzleKey(memberExpr->member)
                                       : memberExpr->member);
            nameToValue_[compositeName] = value;

            // Check if this is an output struct - emit StoreOutput.
            // For local struct vars that get returned (`OUT.field = ...;
            // return OUT;`), the StoreOutput emit is deferred to
            // buildReturnStmt so the output param table follows the
            // struct's *field declaration order* rather than the
            // source statement order — that's what the reference
            // compiler emits.  For `out` parameter struct fields we
            // still emit inline (no return statement to anchor).
            if (ident->resolvedDecl)
            {
                const bool isLocalVar =
                    (ident->resolvedDecl->kind == DeclKind::Variable);

                // Look up struct type info for the target
                TypeNode* typeNode = nullptr;
                if (ident->resolvedDecl->kind == DeclKind::Variable)
                {
                    typeNode = static_cast<VarDecl*>(ident->resolvedDecl)->type.get();
                }

                if (!isLocalVar)
                {
                    const std::vector<StructField>* fields = getStructFields(typeNode);
                    if (fields)
                    {
                        for (const auto& field : *fields)
                        {
                            if (field.name == memberExpr->member && !field.semantic.isEmpty())
                            {
                                auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                                    InvalidIRValue, IRTypeInfo::Void());
                                inst->addOperand(value);
                                inst->semanticName    = field.semantic.name;
                                inst->rawSemanticName = field.semantic.rawName;
                                inst->semanticIndex   = field.semantic.index;
                                inst->fieldName       = memberExpr->member;
                                currentBlock_->addInstruction(std::move(inst));
                                break;
                            }
                        }
                    }
                }
            }

            return value;
        }

        // Handle nested member access (a.b.c = value)
        if (memberExpr->object->kind == ExprKind::MemberAccess)
        {
            std::string baseName;
            ExprNode* current = memberExpr->object.get();
            std::vector<std::string> members;
            members.push_back(memberExpr->isSwizzle
                                  ? canonicalizeSwizzleKey(memberExpr->member)
                                  : memberExpr->member);

            while (current->kind == ExprKind::MemberAccess)
            {
                auto* nested = static_cast<MemberAccessExpr*>(current);
                members.push_back(nested->isSwizzle
                                      ? canonicalizeSwizzleKey(nested->member)
                                      : nested->member);
                current = nested->object.get();
            }

            if (current->kind == ExprKind::Identifier)
            {
                baseName = static_cast<IdentifierExpr*>(current)->name;
            }

            // Build composite name (reverse order)
            std::string compositeName = baseName;
            for (auto it = members.rbegin(); it != members.rend(); ++it)
            {
                compositeName += "." + *it;
            }

            nameToValue_[compositeName] = value;
            return value;
        }
    }

    // Handle array index assignment
    if (target->kind == ExprKind::Index)
    {
        auto* indexExpr = static_cast<IndexExpr*>(target);
        if (indexExpr->array->kind == ExprKind::Identifier &&
            indexExpr->index->kind == ExprKind::Literal)
        {
            auto* ident =
                static_cast<IdentifierExpr*>(indexExpr->array.get());
            auto* lit = static_cast<LiteralExpr*>(indexExpr->index.get());
            auto arrIt = localArrayValues_.find(ident->name);
            if (arrIt == localArrayValues_.end() && !nameToValue_.count(ident->name))
            {
                // First store into a file-scope array: promote it (see the
                // read side in buildIndexExpr).  Elements stay unwritten
                // until stored, so a later read of an untouched element
                // still takes the uniform.
                IRGlobal* global = module_->findGlobal(ident->name);
                if (global && global->type.isArray() && global->type.arraySize > 0)
                    arrIt = localArrayValues_.emplace(
                        ident->name,
                        std::vector<IRValueID>(static_cast<size_t>(global->type.arraySize),
                                               InvalidIRValue)).first;
            }
            if (arrIt != localArrayValues_.end() &&
                lit->literalKind == LiteralExpr::LiteralKind::Int)
            {
                int64_t rawIndex = std::get<int64_t>(lit->value);
                if (rawIndex >= 0 &&
                    rawIndex < static_cast<int64_t>(arrIt->second.size()))
                    arrIt->second[static_cast<size_t>(rawIndex)] = value;
                else
                    error(target->loc, "array index out of bounds");
                return value;
            }
        }
        // Any other element store used to fall out of this function with
        // the value discarded and no diagnostic - the program compiled
        // and read whatever the array held before.  Refuse by name.
        error(target->loc, "assignment to an array element this compiler cannot "
                           "track (non-local array or non-literal index); refusing");
        return value;
    }

    error(target->loc, "Complex assignment target not yet implemented");
    return value;
}

IRValueID IRBuilder::coerceAssignmentValue(ExprNode* target, IRValueID value)
{
    if (value == InvalidIRValue || !target || !currentFunction_)
        return value;

    const IRTypeInfo targetType = getExprType(target);
    IRValue* srcValue = currentFunction_->getValue(value);
    if (!srcValue) return value;

    if (!targetType.isVector() || !srcValue->type.isVector())
        return value;
    if (srcValue->type.vectorSize <= targetType.vectorSize)
        return value;

    static const char lanes[4] = {'x', 'y', 'z', 'w'};
    std::string swizzle;
    for (int i = 0; i < targetType.vectorSize && i < 4; ++i)
        swizzle.push_back(lanes[i]);

    auto inst = std::make_unique<IRInstruction>(
        IROp::VecShuffle,
        currentFunction_->allocateValueId(),
        targetType);
    inst->addOperand(value);
    inst->swizzleMask = IRUtils::encodeSwizzle(swizzle);
    IRValueID result = inst->result;
    currentBlock_->addInstruction(std::move(inst));
    return result;
}

// ============================================================================
// Instruction Emission
// ============================================================================

IRValueID IRBuilder::emitInstruction(IROp op, const IRTypeInfo& resultType,
                                      const std::vector<IRValueID>& operands,
                                      const SourceLocation& loc)
{
    IRValueID result = (resultType.baseType != IRType::Void)
        ? currentFunction_->allocateValueId()
        : InvalidIRValue;

    auto inst = std::make_unique<IRInstruction>(op, result, resultType);
    inst->loc = loc;
    inst->shortCircuitRhs = shortCircuitRhsDepth_ > 0;
    for (IRValueID opnd : operands)
    {
        inst->addOperand(opnd);
    }
    currentBlock_->addInstruction(std::move(inst));

    return result;
}

IRValueID IRBuilder::emitBinaryOp(IROp op, const IRTypeInfo& resultType,
                                   IRValueID left, IRValueID right,
                                   const SourceLocation& loc)
{
    return emitInstruction(op, resultType, {left, right}, loc);
}

IRValueID IRBuilder::emitUnaryOp(IROp op, const IRTypeInfo& resultType,
                                 IRValueID operand, const SourceLocation& loc)
{
    return emitInstruction(op, resultType, {operand}, loc);
}

IRValueID IRBuilder::emitCall(const std::string& funcName, const IRTypeInfo& resultType,
                               const std::vector<IRValueID>& args)
{
    IRValueID result = (resultType.baseType != IRType::Void)
        ? currentFunction_->allocateValueId()
        : InvalidIRValue;

    auto inst = std::make_unique<IRInstruction>(IROp::Call, result, resultType);
    inst->targetName = funcName;
    inst->shortCircuitRhs = shortCircuitRhsDepth_ > 0;
    for (IRValueID arg : args)
    {
        inst->addOperand(arg);
    }
    currentBlock_->addInstruction(std::move(inst));

    return result;
}

void IRBuilder::emitBranch(IRBasicBlock* target)
{
    auto inst = std::make_unique<IRInstruction>(IROp::Branch, InvalidIRValue, IRTypeInfo::Void());
    inst->targetName = target->name;
    currentBlock_->addInstruction(std::move(inst));

    // Update CFG
    currentBlock_->successors.push_back(target);
    target->predecessors.push_back(currentBlock_);
}

void IRBuilder::emitCondBranch(IRValueID condition, IRBasicBlock* trueTarget, IRBasicBlock* falseTarget)
{
    auto inst = std::make_unique<IRInstruction>(IROp::CondBranch, InvalidIRValue, IRTypeInfo::Void());
    inst->addOperand(condition);
    inst->targetName = trueTarget->name + "," + falseTarget->name;
    currentBlock_->addInstruction(std::move(inst));

    // Update CFG
    currentBlock_->successors.push_back(trueTarget);
    currentBlock_->successors.push_back(falseTarget);
    trueTarget->predecessors.push_back(currentBlock_);
    falseTarget->predecessors.push_back(currentBlock_);
}

void IRBuilder::emitReturn(IRValueID value)
{
    auto inst = std::make_unique<IRInstruction>(IROp::Return, InvalidIRValue, IRTypeInfo::Void());
    if (value != InvalidIRValue)
    {
        inst->addOperand(value);
    }
    currentBlock_->addInstruction(std::move(inst));
}

void IRBuilder::emitStore(IRValueID address, IRValueID value)
{
    auto inst = std::make_unique<IRInstruction>(IROp::Store, InvalidIRValue, IRTypeInfo::Void());
    inst->addOperand(address);
    inst->addOperand(value);
    currentBlock_->addInstruction(std::move(inst));
}

// ============================================================================
// Type Helpers
// ============================================================================

IRTypeInfo IRBuilder::getIRType(const CgType& cgType)
{
    return IRTypeInfo::fromCgType(cgType);
}

IRTypeInfo IRBuilder::getIRType(TypeNode* typeNode)
{
    if (!typeNode) return IRTypeInfo::Void();
    return IRTypeInfo::fromCgType(CgType(std::make_shared<TypeNode>(*typeNode)));
}

IRTypeInfo IRBuilder::getExprType(ExprNode* expr)
{
    if (!expr || !expr->resolvedType) return IRTypeInfo::Void();
    return getIRType(expr->resolvedType.get());
}

const std::vector<StructField>* IRBuilder::getStructFields(TypeNode* typeNode)
{
    if (!typeNode || typeNode->baseType != BaseType::Struct)
        return nullptr;

    // First check if fields are directly on the TypeNode
    if (!typeNode->structFields.empty())
        return &typeNode->structFields;

    // Otherwise look up the struct declaration by name from symbol table
    if (!typeNode->structName.empty() && semantic_)
    {
        std::optional<CgType> cgTypeOpt = semantic_->symbolTable().lookupType(typeNode->structName);
        if (cgTypeOpt && cgTypeOpt->isStruct() && !cgTypeOpt->structFields().empty())
        {
            // Store fields in the TypeNode for future lookups (avoid repeated lookups)
            typeNode->structFields = cgTypeOpt->structFields();
            return &typeNode->structFields;
        }
    }
    return nullptr;
}

void IRBuilder::emitStructOutputs(ExprNode* structExpr, const std::vector<StructField>& fields)
{
    // For each field with a semantic, emit a StoreOutput
    for (const auto& field : fields)
    {
        if (field.semantic.isEmpty())
            continue;

        // Look up the value for this field from nameToValue_
        // The format is "varname.fieldname"
        std::string fieldKey;
        if (structExpr->kind == ExprKind::Identifier)
        {
            auto* ident = static_cast<IdentifierExpr*>(structExpr);
            fieldKey = ident->name + "." + field.name;
        }

        auto it = nameToValue_.find(fieldKey);
        if (it != nameToValue_.end())
        {
            IRValueID fieldValue = it->second;

            auto inst = std::make_unique<IRInstruction>(IROp::StoreOutput,
                InvalidIRValue, getIRType(field.type.get()));
            inst->addOperand(fieldValue);
            inst->semanticName    = field.semantic.name;
            inst->rawSemanticName = field.semantic.rawName;
            inst->semanticIndex   = field.semantic.index;
            inst->fieldName       = field.name;
            currentBlock_->addInstruction(std::move(inst));
        }
    }
}

// ============================================================================
// Operator Mapping
// ============================================================================

IROp IRBuilder::binaryOpToIROp(BinaryOp op)
{
    switch (op)
    {
    case BinaryOp::Add: return IROp::Add;
    case BinaryOp::Sub: return IROp::Sub;
    case BinaryOp::Mul: return IROp::Mul;
    case BinaryOp::Div: return IROp::Div;
    case BinaryOp::Mod: return IROp::Mod;
    case BinaryOp::BitwiseAnd: return IROp::And;
    case BinaryOp::BitwiseOr: return IROp::Or;
    case BinaryOp::BitwiseXor: return IROp::Xor;
    case BinaryOp::ShiftLeft: return IROp::Shl;
    case BinaryOp::ShiftRight: return IROp::Shr;
    case BinaryOp::Equal: return IROp::CmpEq;
    case BinaryOp::NotEqual: return IROp::CmpNe;
    case BinaryOp::Less: return IROp::CmpLt;
    case BinaryOp::LessEqual: return IROp::CmpLe;
    case BinaryOp::Greater: return IROp::CmpGt;
    case BinaryOp::GreaterEqual: return IROp::CmpGe;
    case BinaryOp::LogicalAnd: return IROp::LogicalAnd;
    case BinaryOp::LogicalOr: return IROp::LogicalOr;
    default: return IROp::Nop;
    }
}

IROp IRBuilder::unaryOpToIROp(UnaryOp op)
{
    switch (op)
    {
    case UnaryOp::Negate: return IROp::Neg;
    case UnaryOp::LogicalNot: return IROp::LogicalNot;
    case UnaryOp::BitwiseNot: return IROp::Not;
    default: return IROp::Nop;
    }
}

std::optional<IROp> IRBuilder::builtinToIROp(const std::string& name)
{
    static const std::unordered_map<std::string, IROp> builtins = {
        {"abs", IROp::Abs},
        {"sign", IROp::Sign},
        {"floor", IROp::Floor},
        {"ceil", IROp::Ceil},
        {"frac", IROp::Frac},
        {"round", IROp::Round},
        {"trunc", IROp::Trunc},
        {"min", IROp::Min},
        {"max", IROp::Max},
        {"clamp", IROp::Clamp},
        {"saturate", IROp::Saturate},
        {"ddx", IROp::Ddx},
        {"ddy", IROp::Ddy},
        {"pack_2half", IROp::PackHalf2},
        {"unpack_2half", IROp::UnpackHalf2},
        {"pack_4ubyte", IROp::PackUByte4},
        {"unpack_4ubyte", IROp::UnpackUByte4},
        {"pack_4byte", IROp::PackByte4},
        {"unpack_4byte", IROp::UnpackByte4},
        {"pack_2ushort", IROp::PackUShort2},
        {"unpack_2ushort", IROp::UnpackUShort2},
        {"lerp", IROp::Lerp},
        {"step", IROp::Step},
        {"smoothstep", IROp::SmoothStep},
        {"sin", IROp::Sin},
        {"cos", IROp::Cos},
        {"tan", IROp::Tan},
        {"asin", IROp::Asin},
        {"acos", IROp::Acos},
        {"atan", IROp::Atan},
        {"atan2", IROp::Atan2},
        {"pow", IROp::Pow},
        {"exp", IROp::Exp},
        {"exp2", IROp::Exp2},
        {"log", IROp::Log},
        {"log2", IROp::Log2},
        {"log10", IROp::Log10},
        {"sqrt", IROp::Sqrt},
        {"rsqrt", IROp::RSqrt},
        {"dot", IROp::Dot},
        {"cross", IROp::Cross},
        {"length", IROp::Length},
        {"distance", IROp::Distance},
        {"normalize", IROp::Normalize},
        {"reflect", IROp::Reflect},
        {"refract", IROp::Refract},
        {"faceforward", IROp::FaceForward},
        {"mul", IROp::MatVecMul},
        {"tex1D", IROp::TexSample},
        {"tex2D", IROp::TexSample},
        {"h4tex2D", IROp::TexSample},
        {"h3tex2D", IROp::TexSample},
        {"texDepth2D", IROp::TexSample},
        {"texDepth2D_precise", IROp::TexSample},
        {"texRECT", IROp::TexSample},
        {"tex2Dlod", IROp::TexSampleLod},
        {"tex2Dproj", IROp::TexSampleProj},
        {"texCUBE", IROp::TexSample},
    };

    auto it = builtins.find(name);
    if (it != builtins.end())
    {
        return it->second;
    }
    return std::nullopt;
}

// ============================================================================
// Constant Creation
// ============================================================================

IRValueID IRBuilder::createConstant(bool value)
{
    auto* constant = currentFunction_->createConstant(IRTypeInfo::Bool(), value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(int32_t value)
{
    auto* constant = currentFunction_->createConstant(IRTypeInfo::Int(), value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(uint32_t value)
{
    auto* constant = currentFunction_->createConstant(IRTypeInfo::UInt(), value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(float value)
{
    auto* constant = currentFunction_->createConstant(IRTypeInfo::Float(), value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(const IRTypeInfo& type, float value)
{
    auto* constant = currentFunction_->createConstant(type, value);
    return constant->id;
}

IRValueID IRBuilder::createConstant(const IRTypeInfo& type, const std::vector<float>& values, const std::vector<int64_t>& intValues)
{
    auto* constant = currentFunction_->createConstant(type, values, intValues);
    return constant->id;
}
