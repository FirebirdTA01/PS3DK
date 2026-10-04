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
bool arrayStorageKey(ExprNode* expr, std::string& key);
constexpr float kPiDiv180 = 0.017453292519943295769f;
constexpr float k180DivPi = 57.295779513082320876f;

std::optional<float> angleConversionScale(const std::string& name)
{
    if (name == "radians") return kPiDiv180;
    if (name == "degrees") return k180DivPi;
    return std::nullopt;
}

// Broadcast a one-element vector to n copies of that element.  Written as
// v.assign(n, v[0]) this is undefined behaviour - assign's value may not be a
// reference into the container - and the result depends on the standard
// library: libstdc++ copies the value first, libc++ reads it after clearing
// the storage and broadcasts garbage, which made a libc++-built compiler
// refuse `uniform float4 c = 0.5;` (tests/shader-compiler/
// cross-stl-determinism-test.sh).  Copy the element first.
template <class V>
void broadcastFirst(V& v, size_t n)
{
    const typename V::value_type first = v[0];
    v.assign(n, first);
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

// Evaluate a file-scope const's initialiser to floats (file-scope-const-initializer).
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


// ---------------------------------------------------------------------------
// Constant VALUE evaluation for file-scope initialisers (file-scope-initializer-fold).
//
// The reference folds an initialiser expression before it becomes a default
// or an inline constant.  Measured on sce-cgc 475 (.local/probe-init, byte
// twins against hand-folded literals):
//   - int op int is integer arithmetic: 7/2 is 3, -7/2 is -3, 7%3 is 1;
//   - any float operand makes it FLOAT arithmetic, each operation rounded to
//     single precision: (16777216.0f + 1.0f + 1.0f) - 16777216.0f is 0, so
//     folding in double and rounding once would be WRONG;
//   - comparisons and the ternary fold (1 < 2 ? 0.5f : 0.25f is 0.5);
//   - a vector and a scalar combine by broadcast (float2(1,2) * 0.5f);
//   - a constructor flattens its arguments (float2(1.0f/1280.0f, ...) and
//     the brace spelling are byte-identical);
//   - an identifier naming a file-scope const that is itself folded is a
//     constant; a uniform is not (it stays refused here, with the reference's
//     runtime evaluation of that shape recorded as a separate gap).
// A zero divisor, an assignment operator, a bitwise operator on a float and
// anything not listed make the expression NOT a constant: the caller refuses
// rather than compiling a guess.
// ---------------------------------------------------------------------------
namespace
{

using ConstLanes = std::vector<ConstEvalScalar>;

// The shape of a constant value: a scalar (all zero), a vector (width), a
// matrix (rows x cols) or an array of `elems` elements of `elemLanes` lanes.
// Index needs it to select a matrix ROW, an array ELEMENT or a vector LANE
// from the flat lane list; a flat list alone selected M[1] as lane 1.
struct ConstShape
{
    int width = 1;     // vector width of one element (1 for scalar)
    int rows = 0;      // matrix rows (0 = not a matrix)
    int cols = 0;
    int elems = 0;     // array element count (0 = not an array)
    int laneCount() const
    {
        const int per = rows > 0 ? rows * cols : width;
        return (elems > 0 ? elems : 1) * per;
    }
    bool isScalar() const { return elems == 0 && rows == 0 && width == 1; }
};

// An array type node is `BaseType::Array` with the element in `elementType`;
// the scalar kind and the vector/matrix shape both live on the element.
const TypeNode* elementOf(const TypeNode* t)
{
    while (t && t->baseType == BaseType::Array && t->elementType)
        t = t->elementType.get();
    return t;
}

BaseType scalarBaseOf(const TypeNode* t)
{
    const TypeNode* e = elementOf(t);
    return e ? e->baseType : BaseType::Void;
}

ConstShape shapeOfType(const TypeNode* t)
{
    ConstShape sh;
    if (!t) return sh;
    int elems = 0;
    const TypeNode* e = t;
    while (e && e->baseType == BaseType::Array && e->elementType)
    {
        elems = elems > 0 ? elems * e->arraySize : e->arraySize;
        e = e->elementType.get();
    }
    if (e->matrixRows > 0 && e->matrixCols > 0) { sh.rows = e->matrixRows; sh.cols = e->matrixCols; }
    else sh.width = e->vectorSize > 0 ? e->vectorSize : 1;
    if (elems > 0) sh.elems = elems;
    else if (t->arraySize > 0) sh.elems = t->arraySize;
    return sh;
}

ConstShape shapeOfIRType(const IRTypeInfo& t)
{
    ConstShape sh;
    if (t.matrixRows > 0 && t.matrixCols > 0) { sh.rows = t.matrixRows; sh.cols = t.matrixCols; }
    else sh.width = t.vectorSize > 0 ? t.vectorSize : 1;
    if (t.arraySize > 0) sh.elems = t.arraySize;
    return sh;
}

// The shape two elementwise operands combine to: equal shapes, or a scalar
// broadcast against anything.
bool combineShapes(const ConstShape& a, const ConstShape& b, ConstShape& out)
{
    if (a.isScalar()) { out = b; return true; }
    if (b.isScalar()) { out = a; return true; }
    if (a.laneCount() == b.laneCount()) { out = a; return true; }
    return false;
}

ConstEvalScalar::Kind promotedKind(const ConstEvalScalar& a, const ConstEvalScalar& b)
{
    using K = ConstEvalScalar::Kind;
    if (a.kind == K::Float || b.kind == K::Float) return K::Float;
    if (a.kind == K::UInt || b.kind == K::UInt) return K::UInt;
    return K::Int;
}

int64_t wrapInt32(int64_t v)
{
    return static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(v))));
}

bool foldBinaryScalar(BinaryOp op, const ConstEvalScalar& a, const ConstEvalScalar& b, ConstEvalScalar& out)
{
    using K = ConstEvalScalar::Kind;
    switch (op)
    {
    case BinaryOp::Comma:      out = b; return true;
    case BinaryOp::LogicalAnd: out = ConstEvalScalar::fromBool(a.isTruthy() && b.isTruthy()); return true;
    case BinaryOp::LogicalOr:  out = ConstEvalScalar::fromBool(a.isTruthy() || b.isTruthy()); return true;
    default: break;
    }
    const K kind = promotedKind(a, b);
    if (kind == K::Float)
    {
        const float x = static_cast<float>(a.asDouble());
        const float y = static_cast<float>(b.asDouble());
        float r = 0.0f;
        switch (op)
        {
        case BinaryOp::Add: r = x + y; break;
        case BinaryOp::Sub: r = x - y; break;
        case BinaryOp::Mul: r = x * y; break;
        case BinaryOp::Div:
            if (y == 0.0f) return false;   // the reference's value for x/0 is unmeasured
            r = x / y; break;
        case BinaryOp::Mod:
            if (y == 0.0f) return false;
            r = std::fmod(x, y); break;
        case BinaryOp::Equal:        out = ConstEvalScalar::fromBool(x == y); return true;
        case BinaryOp::NotEqual:     out = ConstEvalScalar::fromBool(x != y); return true;
        case BinaryOp::Less:         out = ConstEvalScalar::fromBool(x <  y); return true;
        case BinaryOp::LessEqual:    out = ConstEvalScalar::fromBool(x <= y); return true;
        case BinaryOp::Greater:      out = ConstEvalScalar::fromBool(x >  y); return true;
        case BinaryOp::GreaterEqual: out = ConstEvalScalar::fromBool(x >= y); return true;
        default: return false;       // bitwise, shifts, assignments
        }
        if (!std::isfinite(r)) return false;
        out = ConstEvalScalar::fromFloat(static_cast<double>(r));
        return true;
    }
    if (kind == K::UInt)
    {
        const uint64_t x = static_cast<uint64_t>(a.asInt64()) & 0xFFFFFFFFULL;
        const uint64_t y = static_cast<uint64_t>(b.asInt64()) & 0xFFFFFFFFULL;
        uint64_t r = 0;
        switch (op)
        {
        case BinaryOp::Add: r = x + y; break;
        case BinaryOp::Sub: r = x - y; break;
        case BinaryOp::Mul: r = x * y; break;
        case BinaryOp::Div: if (y == 0) return false; r = x / y; break;
        case BinaryOp::Mod: if (y == 0) return false; r = x % y; break;
        case BinaryOp::BitwiseAnd: r = x & y; break;
        case BinaryOp::BitwiseOr:  r = x | y; break;
        case BinaryOp::BitwiseXor: r = x ^ y; break;
        case BinaryOp::ShiftLeft:  if (y > 31) return false; r = x << y; break;
        case BinaryOp::ShiftRight: if (y > 31) return false; r = x >> y; break;
        case BinaryOp::Equal:        out = ConstEvalScalar::fromBool(x == y); return true;
        case BinaryOp::NotEqual:     out = ConstEvalScalar::fromBool(x != y); return true;
        case BinaryOp::Less:         out = ConstEvalScalar::fromBool(x <  y); return true;
        case BinaryOp::LessEqual:    out = ConstEvalScalar::fromBool(x <= y); return true;
        case BinaryOp::Greater:      out = ConstEvalScalar::fromBool(x >  y); return true;
        case BinaryOp::GreaterEqual: out = ConstEvalScalar::fromBool(x >= y); return true;
        default: return false;
        }
        out = ConstEvalScalar::fromUInt(r & 0xFFFFFFFFULL);
        return true;
    }
    // Int (bool operands promote to 0/1)
    const int64_t x = a.asInt64();
    const int64_t y = b.asInt64();
    int64_t r = 0;
    switch (op)
    {
    case BinaryOp::Add: r = x + y; break;
    case BinaryOp::Sub: r = x - y; break;
    case BinaryOp::Mul: r = x * y; break;
    case BinaryOp::Div: if (y == 0) return false; r = x / y; break;   // truncates toward zero, as measured
    case BinaryOp::Mod: if (y == 0) return false; r = x % y; break;
    case BinaryOp::BitwiseAnd: r = x & y; break;
    case BinaryOp::BitwiseOr:  r = x | y; break;
    case BinaryOp::BitwiseXor: r = x ^ y; break;
    case BinaryOp::ShiftLeft:  if (y < 0 || y > 31) return false; r = static_cast<int64_t>(static_cast<uint64_t>(x) << y); break;
    case BinaryOp::ShiftRight: if (y < 0 || y > 31) return false; r = x >> y; break;
    case BinaryOp::Equal:        out = ConstEvalScalar::fromBool(x == y); return true;
    case BinaryOp::NotEqual:     out = ConstEvalScalar::fromBool(x != y); return true;
    case BinaryOp::Less:         out = ConstEvalScalar::fromBool(x <  y); return true;
    case BinaryOp::LessEqual:    out = ConstEvalScalar::fromBool(x <= y); return true;
    case BinaryOp::Greater:      out = ConstEvalScalar::fromBool(x >  y); return true;
    case BinaryOp::GreaterEqual: out = ConstEvalScalar::fromBool(x >= y); return true;
    default: return false;
    }
    out = ConstEvalScalar::fromInt(wrapInt32(r));
    return true;
}

// The type both branches of a ternary convert to before anything uses the
// result: (true ? 7 : 2.0f) / 2 is 3.5 on the reference, not 3 (codex's cell).
BaseType commonBaseType(const ConstEvalScalar& a, const ConstEvalScalar& b)
{
    using K = ConstEvalScalar::Kind;
    if (a.kind == K::Float || b.kind == K::Float) return BaseType::Float;
    if (a.kind == K::UInt || b.kind == K::UInt) return BaseType::UInt;
    if (a.kind == K::Bool && b.kind == K::Bool) return BaseType::Bool;
    return BaseType::Int;
}

bool foldUnaryScalar(UnaryOp op, const ConstEvalScalar& v, ConstEvalScalar& out)
{
    using K = ConstEvalScalar::Kind;
    switch (op)
    {
    case UnaryOp::LogicalNot:
        out = ConstEvalScalar::fromBool(!v.isTruthy()); return true;
    case UnaryOp::Negate:
        switch (v.kind)
        {
        case K::Int:   out = ConstEvalScalar::fromInt(wrapInt32(-v.i)); return true;
        case K::UInt:  out = ConstEvalScalar::fromUInt(static_cast<uint64_t>(-static_cast<int64_t>(v.u & 0xFFFFFFFFULL)) & 0xFFFFFFFFULL); return true;
        case K::Float: out = ConstEvalScalar::fromFloat(-v.f); return true;
        case K::Bool:  return false;
        }
        return false;
    case UnaryOp::BitwiseNot:
        switch (v.kind)
        {
        case K::Int:  out = ConstEvalScalar::fromInt(wrapInt32(~v.i)); return true;
        case K::UInt: out = ConstEvalScalar::fromUInt(~v.u & 0xFFFFFFFFULL); return true;
        default: return false;
        }
    default:
        return false;
    }
}

int laneOfSwizzleLetter(char ch)
{
    switch (ch)
    {
    case 'x': case 'r': return 0;
    case 'y': case 'g': return 1;
    case 'z': case 'b': return 2;
    case 'w': case 'a': return 3;
    default: return -1;
    }
}

int componentCountOf(const TypeNode* t)
{
    if (!t) return 1;
    if (t->matrixRows > 0 && t->matrixCols > 0) return t->matrixRows * t->matrixCols;
    return t->vectorSize > 0 ? t->vectorSize : 1;
}

struct ConstEvalContext
{
    bool allowBuiltinCalls;
    std::unordered_set<const DeclNode*> validated;
    std::unordered_set<const DeclNode*> active;
};

bool evaluateConstValueImpl(const ExprNode* e, ConstLanes& out, ConstShape& shape,
                            IRModule* module, ConstEvalContext& context);

// Builtin folding is opt-in for initializer callers. Runtime condition analysis
// uses this evaluator too, with its own scope and control-flow restrictions.
bool evaluateConstValue(const ExprNode* e, ConstLanes& out, ConstShape& shape,
                        IRModule* module, bool allowBuiltinCalls = false)
{
    ConstEvalContext context{allowBuiltinCalls, {}, {}};
    return evaluateConstValueImpl(e, out, shape, module, context);
}

bool evaluateConstValueImpl(const ExprNode* e, ConstLanes& out, ConstShape& shape,
                            IRModule* module, ConstEvalContext& context)
{
    out.clear();
    shape = ConstShape();
    if (!e) return false;
    switch (e->kind)
    {
    case ExprKind::Literal:
    {
        ConstEvalScalar v;
        if (!evaluateConstScalar(e, v)) return false;
        out.push_back(v);
        return true;
    }
    case ExprKind::Call:
    {
        const auto* call = static_cast<const CallExpr*>(e);
        // Source functions may shadow these names. A successfully resolved
        // builtin has no source declaration; an unresolved call has no valid
        // result type. Neither a source function nor a dormant bad call folds.
        const TypeNode* type = call->resolvedType.get();
        if (!context.allowBuiltinCalls || call->resolvedFunction || !type || type->baseType != BaseType::Float)
            return false;
        shape = shapeOfType(type);
        if (shape.rows || shape.elems || shape.width < 1 || shape.width > 4)
            return false;
        const std::string& name = call->functionName;
        const size_t arity = (name == "ceil" || name == "abs") ? 1
                           : (name == "min" || name == "max") ? 2
                           : name == "clamp" ? 3 : 0;
        if (!arity || call->arguments.size() != arity) return false;
        std::vector<ConstLanes> args(arity);
        for (size_t i = 0; i < arity; ++i)
        {
            const ExprNode* arg = call->arguments[i].get();
            // Low precision and integer overload conversions are separate
            // measured debts, not permission to evaluate them as float.
            if (!arg || !arg->resolvedType || arg->resolvedType->baseType != BaseType::Float)
                return false;
            ConstShape sh;
            if (!evaluateConstValueImpl(arg, args[i], sh, module, context) || sh.rows || sh.elems ||
                (sh.width != 1 && sh.width != shape.width) ||
                args[i].size() != static_cast<size_t>(sh.width)) return false;
            for (const auto& lane : args[i])
                if (!std::isfinite(static_cast<float>(lane.asDouble()))) return false;
        }
        for (int lane = 0; lane < shape.width; ++lane)
        {
            float v[3] = {};
            for (size_t i = 0; i < arity; ++i)
                v[i] = static_cast<float>(args[i][args[i].size() == 1 ? 0 : lane].asDouble());
            // Equal signed zeros have an ordering-dependent result in min/max.
            // Keep that unmeasured boundary refused; ceil preserves -0 and
            // abs clears it, as the ordinary floating-point operations do.
            for (size_t i = 0; i < arity; ++i)
                for (size_t j = i + 1; j < arity; ++j)
                    if (v[i] == 0 && v[j] == 0 && std::signbit(v[i]) != std::signbit(v[j]))
                        return false;
            float result = 0;
            if (name == "ceil") result = std::ceil(v[0]);
            else if (name == "abs") result = std::fabs(v[0]);
            else if (name == "min") result = std::min(v[0], v[1]);
            else if (name == "max") result = std::max(v[0], v[1]);
            else
            {
                if (v[1] > v[2]) return false; // reversed bounds are unmeasured
                result = std::min(std::max(v[0], v[1]), v[2]);
            }
            out.push_back(ConstEvalScalar::fromFloat(static_cast<double>(result)));
        }
        return true;
    }
    case ExprKind::Unary:
    {
        const auto* u = static_cast<const UnaryExpr*>(e);
        ConstLanes v; ConstShape sh;
        if (!evaluateConstValueImpl(u->operand.get(), v, sh, module, context)) return false;
        for (const auto& lane : v)
        {
            ConstEvalScalar r;
            if (!foldUnaryScalar(u->op, lane, r)) return false;
            out.push_back(r);
        }
        shape = sh;
        return !out.empty();
    }
    case ExprKind::Binary:
    {
        const auto* b = static_cast<const BinaryExpr*>(e);
        ConstLanes l, r; ConstShape ls, rs;
        if (!evaluateConstValueImpl(b->left.get(), l, ls, module, context)) return false;
        if (!evaluateConstValueImpl(b->right.get(), r, rs, module, context)) return false;
        if (l.empty() || r.empty()) return false;
        if (!combineShapes(ls, rs, shape)) return false;
        const size_t n = std::max(l.size(), r.size());
        for (size_t i = 0; i < n; ++i)
        {
            const ConstEvalScalar& a = l[l.size() == 1 ? 0 : i];
            const ConstEvalScalar& c = r[r.size() == 1 ? 0 : i];
            ConstEvalScalar v;
            if (!foldBinaryScalar(b->op, a, c, v)) return false;
            out.push_back(v);
        }
        return true;
    }
    case ExprKind::Ternary:
    {
        const auto* t = static_cast<const TernaryExpr*>(e);
        ConstLanes cond, a, c; ConstShape cs, as, bs;
        if (!evaluateConstValueImpl(t->condition.get(), cond, cs, module, context) || cond.size() != 1) return false;
        // Both branches are evaluated: the result takes their COMMON type and
        // the common shape, whichever branch is selected.
        if (!evaluateConstValueImpl(t->thenExpr.get(), a, as, module, context)) return false;
        if (!evaluateConstValueImpl(t->elseExpr.get(), c, bs, module, context)) return false;
        if (a.empty() || c.empty() || !combineShapes(as, bs, shape)) return false;
        const BaseType common = commonBaseType(a[0], c[0]);
        const ConstLanes& chosen = cond[0].isTruthy() ? a : c;
        const size_t n = static_cast<size_t>(shape.laneCount());
        for (size_t i = 0; i < n; ++i)
        {
            ConstEvalScalar v;
            if (!convertScalar(chosen[chosen.size() == 1 ? 0 : i], common, v)) return false;
            out.push_back(v);
        }
        return true;
    }
    case ExprKind::Cast:
    {
        const auto* cast = static_cast<const CastExpr*>(e);
        if (!cast->targetType) return false;
        ConstLanes v; ConstShape sh;
        if (!evaluateConstValueImpl(cast->operand.get(), v, sh, module, context)) return false;
        shape = shapeOfType(cast->targetType.get());
        const int n = shape.laneCount();
        if (v.size() == 1 && n > 1) broadcastFirst(v, static_cast<size_t>(n));
        if (static_cast<int>(v.size()) != n) return false;
        for (const auto& lane : v)
        {
            ConstEvalScalar c;
            if (!convertScalar(lane, scalarBaseOf(cast->targetType.get()), c)) return false;
            out.push_back(c);
        }
        return true;
    }
    case ExprKind::Constructor:
    {
        const auto* ctor = static_cast<const ConstructorExpr*>(e);
        if (ctor->arguments.empty() || !ctor->constructedType) return false;
        const TypeNode* ctorType = ctor->constructedType.get();
        const BaseType elemType = scalarBaseOf(ctorType);   // an array's scalar kind is its element's
        ConstLanes flat;
        for (const auto& a : ctor->arguments)
        {
            ConstLanes v; ConstShape sh;
            if (!evaluateConstValueImpl(a.get(), v, sh, module, context)) return false;
            for (const auto& lane : v)
            {
                ConstEvalScalar c;
                if (!convertScalar(lane, elemType, c)) return false;
                flat.push_back(c);
            }
        }
        shape = shapeOfType(ctorType);
        const int n = shape.laneCount();
        if (flat.size() == 1 && n > 1) broadcastFirst(flat, static_cast<size_t>(n));
        // The constructed value has exactly its type's lanes: a brace list or
        // constructor with more data than the type holds is C1058 "too much
        // data" on the reference (measured, s6 in .local/probe-init) and is
        // not a constant here either.
        if (static_cast<int>(flat.size()) != n) return false;
        out = flat;
        return true;
    }
    case ExprKind::Identifier:
    {
        if (!module) return false;
        const auto* id = static_cast<const IdentifierExpr*>(e);
        IRGlobal* g = module->findGlobal(id->name);
        // Only a STATIC const is a constant here: the reference treats a
        // non-static file-scope const as a uniform with a default, and an
        // initialiser that reads one is C1059 there (measured, c6 in
        // .local/probe-init), so it must not fold.
        if (!g || g->storage != StorageQualifier::Const || !g->declaredStatic) return false;
        if (g->initialValue.empty() && g->initialIntValues.empty()) return false;
        if (!context.allowBuiltinCalls)
        {
            // Checking only the immediate Call node misses transitive forms:
            // static const A=ceil(...); const K=A also loses K's binding.
            // Recheck the already evaluated declaration's dependency chain
            // under this caller's policy before consuming its saved value.
            const DeclNode* decl = id->resolvedDecl;
            if (!decl || decl->kind != DeclKind::Variable) return false;
            if (!context.validated.count(decl))
            {
                if (!context.active.insert(decl).second) return false;
                const auto* var = static_cast<const VarDecl*>(decl);
                ConstLanes dependency; ConstShape dependencyShape;
                if (!evaluateConstValueImpl(var->initializer.get(), dependency, dependencyShape,
                                            module, context)) return false;
                context.active.erase(decl);
                context.validated.insert(decl);
            }
        }
        const IRType bt = g->type.baseType;
        const bool useInts = !g->initialIntValues.empty() &&
                             (bt == IRType::Int32 || bt == IRType::UInt32 || bt == IRType::Bool);
        if (useInts)
        {
            for (int64_t v : g->initialIntValues)
                out.push_back(bt == IRType::UInt32 ? ConstEvalScalar::fromUInt(static_cast<uint64_t>(v) & 0xFFFFFFFFULL)
                            : bt == IRType::Bool ? ConstEvalScalar::fromBool(v != 0)
                            : ConstEvalScalar::fromInt(v));
        }
        else
        {
            for (float v : g->initialValue)
                out.push_back(ConstEvalScalar::fromFloat(static_cast<double>(v)));
        }
        shape = shapeOfIRType(g->type);
        if (static_cast<int>(out.size()) != shape.laneCount()) return false;
        return !out.empty();
    }
    case ExprKind::MemberAccess:
    {
        const auto* m = static_cast<const MemberAccessExpr*>(e);
        if (!m->isSwizzle || m->member.empty() || m->member.size() > 4) return false;
        ConstLanes v; ConstShape sh;
        if (!evaluateConstValueImpl(m->object.get(), v, sh, module, context)) return false;
        if (sh.rows > 0 || sh.elems > 0) return false;   // a swizzle reads a vector
        for (char ch : m->member)
        {
            const int lane = laneOfSwizzleLetter(ch);
            if (lane < 0 || lane >= sh.width) return false;
            out.push_back(v[static_cast<size_t>(lane)]);
        }
        shape.width = static_cast<int>(m->member.size());
        return true;
    }
    case ExprKind::Index:
    {
        const auto* ix = static_cast<const IndexExpr*>(e);
        ConstLanes v, idx; ConstShape sh, is;
        if (!evaluateConstValueImpl(ix->array.get(), v, sh, module, context)) return false;
        if (!evaluateConstValueImpl(ix->index.get(), idx, is, module, context) || idx.size() != 1) return false;
        if (idx[0].kind == ConstEvalScalar::Kind::Float) return false;   // float selectors are the IR path's rule, not this one's
        const int64_t i = idx[0].asInt64();
        int count = 0, stride = 0;
        ConstShape elem;
        if (sh.elems > 0)            // array: element i
        {
            count = sh.elems; elem = sh; elem.elems = 0; stride = elem.laneCount();
        }
        else if (sh.rows > 0)        // matrix: row i, a vector of cols lanes
        {
            count = sh.rows; elem.width = sh.cols; stride = sh.cols;
        }
        else if (sh.width > 1)       // vector: lane i
        {
            count = sh.width; stride = 1;
        }
        else return false;
        if (i < 0 || i >= count) return false;
        const size_t begin = static_cast<size_t>(i) * static_cast<size_t>(stride);
        if (begin + static_cast<size_t>(stride) > v.size()) return false;
        out.assign(v.begin() + begin, v.begin() + begin + stride);
        shape = elem;
        return true;
    }
    default:
        return false;
    }
}

} // namespace

bool IRBuilder::evaluateConstInitializerTyped(const ExprNode* init,
                                             const TypeNode* declType,
                                             std::vector<float>& floatOut,
                                             std::vector<int64_t>& intOut,
                                             IRModule* module, bool allowBuiltinCalls)
{
    floatOut.clear();
    intOut.clear();
    if (!init)
        return false;

    ConstLanes lanes;
    ConstShape shape;
    if (!evaluateConstValue(init, lanes, shape, module, allowBuiltinCalls) || lanes.empty())
        return false;

    if (declType)
    {
        // A scalar initialiser broadcasts to a vector (float2 d = 1.0f is
        // (1,1)).  A WIDER vector narrows to the declared type's leading
        // lanes: `float2 u = float4(1,2,3,4)` is byte-identical to
        // `float2(1,2)` on the reference (measured, s5 in .local/probe-init).
        // Too much data inside one constructor is refused above, not here.
        // A declared ARRAY has arraySize * element lanes and is never
        // narrowed: it must be initialised by a value of the same shape.
        const ConstShape declared = shapeOfType(declType);
        const int n = declared.laneCount();
        if (declared.elems > 0 || declared.rows > 0)
        {
            if (static_cast<int>(lanes.size()) != n) return false;
        }
        else
        {
            if (lanes.size() == 1 && n > 1)
                broadcastFirst(lanes, static_cast<size_t>(n));
            if (shape.rows > 0 || shape.elems > 0) return false;   // a matrix or array is not a vector
            if (static_cast<int>(lanes.size()) > n)
                lanes.resize(static_cast<size_t>(n));
        }
    }

    for (const auto& lane : lanes)
    {
        ConstEvalScalar converted = lane;
        if (declType && !convertScalar(lane, scalarBaseOf(declType), converted))
            return false;
        floatOut.push_back(static_cast<float>(converted.asDouble()));
        intOut.push_back(converted.asInt64());
    }
    return true;
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
    globalDeclarations_.clear();
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

    // LOWER ONLY WHAT THE ENTRY CAN REACH.  The reference emits no code for an
    // unreachable function and reports no name error from inside one - the
    // semantic pass already holds those back (function-visibility), and lowering one
    // anyway would re-raise them here as "IR generation error: Unknown
    // identifier".  Type and arity errors are NOT affected: pass 2 analyses
    // every body, reachable or not, which is what the reference does too.
    //
    // functionDefinitionsByName_ above stays COMPLETE on purpose - it is the
    // inlining lookup, and narrowing it would change which definition a
    // reachable call resolves to.
    const std::unordered_set<const FunctionDecl*> reachable =
        semantic.entryReachableDefinitions();
    for (auto& decl : unit.declarations)
    {
        if (decl->kind == DeclKind::Function)
        {
            auto* funcDecl = static_cast<FunctionDecl*>(decl.get());
            if (!funcDecl->isPrototype() && !funcDecl->isIntrinsic)
            {
                // An empty set means there is no entry at all, which the
                // semantic pass has already refused; lower everything then
                // rather than silently emitting nothing.
                if (!reachable.empty() && reachable.find(funcDecl) == reachable.end())
                {
                    continue;
                }
                buildFunction(funcDecl);
            }
        }
    }

    // Set entry point
    module_->entryPoint = module_->getFunction(module_->entryPointName);

    // Bool matrices currently have local value semantics only. Container
    // reflection still describes matrix parameters/globals as float matrices.
    for (const auto& global : module_->globals)
        if (global.type.isMatrix() && global.type.elementType == IRType::Bool)
            error(SourceLocation{}, "bool matrix globals are not implemented: '" + global.name + "'");
    if (module_->entryPoint)
        for (const auto& parameter : module_->entryPoint->parameters)
            if (parameter.type.isMatrix() && parameter.type.elementType == IRType::Bool)
                error(SourceLocation{}, "bool matrix entry parameters are not implemented: '" + parameter.name + "'");

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
            //             must be known or the fold is a zero (file-scope-const-initializer).
            //   uniform - the value is the parameter's COMPILED DEFAULT,
            //             written into the same inline const block that a
            //             runtime patch later overwrites.  So `uniform
            //             float4 c = float4(1,0,0,1);` works unpatched AND
            //             stays patchable by name, which is what the
            //             reference compiler does (general-lowering-default).
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
            // A mutable `static` starts each invocation at its initialiser
            // (measured: `static float k = 0.5;` read before any write folds
            // to 0.5, `static float2 aspect = float2(1.0, 0.5)` to the pair,
            // a brace-initialised static array to its elements).  The value
            // is bound at the entry's start, and writes rebind it like any
            // other file-scope name.
            const bool isInitialisedStatic =
                varDecl->storage == StorageQualifier::Static &&
                varDecl->initializer != nullptr;
            // Non-static const reflection is an existing separate debt: it
            // currently materializes literals instead of a patchable default.
            // Do not newly accept builtin expressions in that context until
            // the binding path is corrected. Explicit uniforms remain allowed.
            if (isFileScopeConst || isInitialisedUniform || isInitialisedStatic)
            {
                if (!evaluateConstInitializerTyped(varDecl->initializer.get(),
                                                   varDecl->type.get(),
                                                   constInit,
                                                   constIntInit,
                                                   module_.get(),
                                                   !isFileScopeConst || varDecl->isStatic))
                {
                    // REFUSE rather than drop it.  Silently emitting zero for
                    // a value we could not evaluate is the defect this fix
                    // exists to remove, and a wrong constant is invisible in
                    // a container that is otherwise well formed.  A uniform
                    // is no safer: a compiled default of zero where the
                    // source says otherwise is a shader that renders wrong
                    // until something patches it.
                    // A mutable STATIC is the exception: it is not seeded
                    // (no value is invented), so an UNREAD static with an
                    // initialiser this evaluator cannot fold - a helper call,
                    // `static float g = f(0.25);` - stays accepted, as it was
                    // before statics were seeded (fp_reach_init_root_valid_f),
                    // and a READ of it still refuses at the read.
                    if (isInitialisedStatic)
                        runtimeStaticInits_.push_back(varDecl);
                    else
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
            globalDeclarations_.insert(varDecl);
            global.name = varDecl->name;
            global.type = getIRType(varDecl->type.get());
            global.valueId = module_->allocateGlobalId();
            global.storage = varDecl->storage;
            global.declaredStatic = varDecl->isStatic;

            if (!varDecl->semantic.isEmpty())
            {
                global.semanticName = varDecl->semantic.name;
                global.rawSemanticName = varDecl->semantic.rawName;
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
                // An array declaration holds arraySize elements of its
                // component count (file-scope-initializer-fold: `static const float A[3]`
                // is three lanes, not one).
                const int declared = global.type.componentCount() *
                                     (global.type.arraySize > 0 ? global.type.arraySize : 1);
                if (declared > 1)
                    broadcastFirst(constInit, static_cast<size_t>(declared));
            }
            else if (!constInit.empty() &&
                     constInit.size() !=
                         static_cast<size_t>(global.type.componentCount() *
                                             (global.type.arraySize > 0 ? global.type.arraySize : 1)))
            {
                // Any other count mismatch is a form this narrow evaluator
                // does not understand well enough to widen.  Refuse rather
                // than pad with zeros, for the same reason as above.
                error(varDecl->loc,
                      "file-scope '" + varDecl->name + "' has a " +
                      std::to_string(constInit.size()) +
                      "-component initialiser for a " +
                      std::to_string(global.type.componentCount() *
                                     (global.type.arraySize > 0 ? global.type.arraySize : 1)) +
                      "-component declaration; refusing rather than "
                      "padding it with zeros");
                constInit.clear();
                constIntInit.clear();
            }

            if (constIntInit.size() == 1)
            {
                const int declared = global.type.componentCount();
                if (declared > 1)
                    broadcastFirst(constIntInit, static_cast<size_t>(declared));
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
            // no diagnostic (file-scope-const-initializer).  A local const never had this
            // problem because buildVarDeclStmt maps the name to
            // buildExpr(initialiser), which is a real IRConstant.
            //
            // File-scope `const` (including `static const` and `const static`,
            // both mapped to StorageQualifier::Const by the parser) folds to its
            // initializer so its value materializes as a typed IRConstant
            // rather than an unbacked uniform load (static-const-qualifiers).
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
                    // A NESTED struct member flattens by its full path
                    // ("u.in1.a"), the way the reference lists it; before
                    // this the inner struct became one struct-typed global no
                    // read could use, and the program was refused.
                    const auto flatten = [&](const auto& self, const std::vector<StructField>& members,
                                             const std::string& prefix) -> void {
                        for (const auto& field : members)
                        {
                            std::string qualifiedName = prefix + "." + field.name;
                            if (field.type && field.type->baseType == BaseType::Struct &&
                                !field.type->isArray())
                            {
                                if (const auto* inner = getStructFields(field.type.get()))
                                {
                                    self(self, *inner, qualifiedName);
                                    continue;
                                }
                            }
                            IRTypeInfo fieldIRType = getIRType(field.type.get());

                            IRGlobal memberGlobal;
                            memberGlobal.name = qualifiedName;
                            memberGlobal.type = fieldIRType;
                            memberGlobal.valueId = module_->allocateGlobalId();
                            memberGlobal.storage = StorageQualifier::Uniform;

                            module_->addGlobal(memberGlobal);
                            nameToValue_[qualifiedName] = memberGlobal.valueId;
                        }
                    };
                    flatten(flatten, *fields, varDecl->name);
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
// COLOR0 - and without it we dropped the whole shader (implicit-colour-output).  The
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
    // Only the ENTRY's own parameter: while a helper is inlined the current
    // function is still the entry, and a helper's `out float4 o` assigned in
    // its body was taken for the entry's default COLOR and stored there.
    for (const auto& own : currentFunctionDecl_->parameters)
        if (own.get() == param) return true;
    return false;
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

    // A mutable file-scope `static` with an initialiser starts the program
    // at that value.  Bound here, at the entry's start, so a read before any
    // write is the initialiser and every write - the entry's own or an
    // inlined helper's, which share these bindings - rebinds it the way an
    // assigned file-scope name already does.  An array binds per element.
    // FIRST, before any parameter: a parameter of the same name binds after
    // it and stashes it, as a local shadowing a global does, so the entry
    // reads its parameter and an inlined helper the static (review: codex,
    // static-entry-shadow).
    if (currentFunction_->isEntryPoint)
    {
        for (const IRGlobal& g : module_->globals)
        {
            if (g.storage != StorageQualifier::Static) continue;
            if (g.initialValue.empty() && g.initialIntValues.empty()) continue;
            if (g.type.arraySize > 0 && !g.type.isMatrix())
            {
                IRTypeInfo element = g.type;
                element.arraySize = 0;
                const size_t width = static_cast<size_t>(element.componentCount());
                auto& values = localArrayValues_[g.name];
                values.clear();
                for (int i = 0; i < g.type.arraySize; ++i)
                {
                    const size_t at = static_cast<size_t>(i) * width;
                    std::vector<float> lanes;
                    std::vector<int64_t> intLanes;
                    if (at + width <= g.initialValue.size())
                        lanes.assign(g.initialValue.begin() + at, g.initialValue.begin() + at + width);
                    if (at + width <= g.initialIntValues.size())
                        intLanes.assign(g.initialIntValues.begin() + at,
                                        g.initialIntValues.begin() + at + width);
                    values.push_back(materialiseInitialiser(element, lanes, intLanes));
                }
                continue;
            }
            const IRValueID value = materialiseInitialiser(g.type, g.initialValue, g.initialIntValues);
            if (value != InvalidIRValue) nameToValue_[g.name] = value;
        }
    }


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
                if (output.type.isArray() && !output.type.isMatrix())
                {
                    const int count = output.type.arraySize;
                    if (currentFunction_->isEntryPoint)
                    {
                        std::string semantic = output.semanticName;
                        std::transform(semantic.begin(), semantic.end(), semantic.begin(),
                            [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                        int limit = 0;
                        const bool vertex = module_->shaderStage == ShaderStage::Vertex;
                        if (semantic == "COLOR" || semantic == "COL") limit = vertex ? 2 : 4;
                        if (vertex && (semantic == "TEXCOORD" || semantic == "TEX")) limit = 10;
                        const bool singleton = vertex
                            ? (semantic == "POSITION" || semantic == "HPOS" ||
                               semantic == "PSIZE" || semantic == "PSIZ" ||
                               semantic == "FOG" || semantic == "FOGC")
                            : (semantic == "DEPTH" || semantic == "DEPR");
                        if (singleton && count > 1)
                            error(decl->loc, "C5121: multiple bindings to a singleton output semantic");
                        if (limit && (output.semanticIndex < 0 ||
                            output.semanticIndex > limit - count))
                            error(decl->loc, "C5102: output array semantic index exceeds its resource range");
                    }
                    output.type.arraySize = 0;
                    for (int i = 0; i < count; ++i)
                    {
                        IRParameter element = output;
                        element.name += "[" + std::to_string(i) + "]";
                        element.semanticIndex += i;
                        currentFunction_->returnOutputs.push_back(std::move(element));
                    }
                }
                else currentFunction_->returnOutputs.push_back(std::move(output));
            } else if (getStructFields(field.type.get())) {
                self(self, field.type.get(), fieldPath);
            }
        }
    };
    if (decl->returnType) {
        collectReturnOutputs(collectReturnOutputs, decl->returnType.get(), "");
    }

    // Build parameters
    int sourceOrdinal = -1;
    for (auto& param : decl->parameters)
    {
        ++sourceOrdinal;
        // A UNIFORM STRUCT ENTRY PARAMETER is a set of uniforms, one per
        // member, named `param.member` and sharing the parameter's paramno
        // (measured on the reference: `uniform input IN` lists IN.video_size,
        // IN.texture_size, ... all at paramno 1, an unread member included
        // and marked unreferenced; a later parameter keeps its own ordinal).
        // Flatten it into one uniform parameter per member so every member
        // takes the ordinary uniform-parameter path; a member read resolves
        // through nameToValue_ by its qualified name.  Before this a member
        // read became an attribute load indexed by the member's position and
        // the program was refused as an unsupported input semantic (libretro
        // COMPAT_IN_FRAGMENT's `uniform input IN`).
        if (currentFunction_->isEntryPoint &&
            param->storage == StorageQualifier::Uniform && param->type &&
            param->type->baseType == BaseType::Struct && !param->type->isArray())
        {
            const std::vector<StructField>* fields = getStructFields(param->type.get());
            if (fields && !fields->empty())
            {
                // The reference gives each member its own default record
                // (`IN.b = 1 2`); per-member defaults are not evaluated here
                // yet, so a default refuses by name rather than being dropped.
                if (param->defaultValue)
                {
                    error(param->loc, "uniform struct entry parameter '" + param->name +
                          "' has a default value; per-member defaults are not emitted yet "
                          "(uniform-struct-entry-parameter), refusing rather than dropping it");
                }
                // Stash the file-scope bindings this parameter shadows BEFORE
                // binding the members: the stash records each global key's
                // current binding, so stashing after would save the
                // parameter's members as the global's (review: codex), and a
                // helper reading the file-scope `IN.b` would read the
                // parameter.  The ordinary parameter path stashes first too.
                stashShadowedGlobal(param->name);
                // Nested struct members flatten the same way, named by their
                // full path (the reference lists `IN.in1.a`).  Array members
                // are not flattened yet and refuse by name.
                const auto flatten = [&](const auto& self, const std::vector<StructField>& members,
                                         const std::string& prefix) -> void {
                    for (const auto& field : members)
                    {
                        const std::string qualified = prefix + "." + field.name;
                        if (field.type && field.type->baseType == BaseType::Struct &&
                            !field.type->isArray())
                        {
                            if (const auto* inner = getStructFields(field.type.get()))
                            {
                                self(self, *inner, qualified);
                                continue;
                            }
                        }
                        if (!field.type || field.type->baseType == BaseType::Struct ||
                            field.type->isArray())
                        {
                            error(param->loc, "uniform struct entry parameter member '" + qualified +
                                  "' is an array; only scalar, vector, matrix and struct members "
                                  "are supported (uniform-struct-entry-parameter), refusing");
                            continue;
                        }
                        IRParameter member;
                        member.name = qualified;
                        member.type = getIRType(field.type.get());
                        member.valueId = currentFunction_->allocateValueId();
                        member.storage = StorageQualifier::Uniform;
                        member.sourceOrdinal = sourceOrdinal;
                        currentFunction_->parameters.push_back(member);
                        nameToValue_[qualified] = member.valueId;
                    }
                };
                flatten(flatten, *fields, param->name);
                flattenedUniformStructParams_.insert(param->name);
                continue;
            }
        }
        // A VARYING struct entry parameter may declare some members
        // `uniform` (libretro's `struct prev { uniform float2 video_size;
        // uniform sampler2D texture; float2 tex_coord; }` passed as a plain
        // parameter).  The reference lists each such member as a uniform
        // named `param.member` at the parameter's paramno, a sampler on its
        // own texture unit, and keeps the other members as varyings
        // (measured: P.size uniform, P.tc TEX1).  Bind the uniform members
        // here as uniform parameters; a member read finds them by qualified
        // name before any attribute load is made.  The parameter itself
        // continues as the varying struct it is.
        if (currentFunction_->isEntryPoint &&
            param->storage != StorageQualifier::Uniform &&
            param->storage != StorageQualifier::Out && param->storage != StorageQualifier::InOut &&
            param->type && param->type->baseType == BaseType::Struct && !param->type->isArray())
        {
            if (const std::vector<StructField>* fields = getStructFields(param->type.get()))
            {
                bool stashed = false;
                for (const auto& field : *fields)
                {
                    // a sampler member is a uniform whether or not it says so
                    // (measured: an unqualified sampler2D member binds a unit)
                    if (field.storage != StorageQualifier::Uniform &&
                        !(field.type && field.type->isSampler())) continue;
                    const std::string qualified = param->name + "." + field.name;
                    if (!field.type || field.type->baseType == BaseType::Struct || field.type->isArray())
                    {
                        error(param->loc, "uniform member '" + qualified + "' of a varying struct "
                              "parameter is a struct or an array; only scalar, vector, matrix and "
                              "sampler members are supported (varying-struct-uniform-member), refusing");
                        continue;
                    }
                    if (!stashed) { stashShadowedGlobal(param->name); stashed = true; }
                    IRParameter member;
                    member.name = qualified;
                    member.type = getIRType(field.type.get());
                    member.valueId = currentFunction_->allocateValueId();
                    member.storage = StorageQualifier::Uniform;
                    member.sourceOrdinal = sourceOrdinal;
                    // The member's own binding carries over (review: codex;
                    // measured: `sampler2D tex : TEXUNIT3` is unit 3 and
                    // `: register(s5)` unit 5 - without these the sampler
                    // took the next free unit and read the wrong texture).
                    // Only measured for samplers; a numeric member's explicit
                    // register is not carried (unmeasured).
                    if (field.type && field.type->isSampler())
                    {
                        member.semanticName = field.semantic.name;
                        member.rawSemanticName = field.semantic.rawName;
                        member.semanticIndex = field.semantic.index;
                        if (field.semantic.hasExplicitRegister())
                        {
                            member.explicitRegisterBank = field.semantic.explicitRegisterBank;
                            member.explicitRegisterIndex = field.semantic.explicitRegisterIndex;
                        }
                    }
                    currentFunction_->parameters.push_back(member);
                    nameToValue_[qualified] = member.valueId;
                }
            }
        }
        IRParameter irParam;
        irParam.sourceOrdinal = sourceOrdinal;
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

        // A UNIFORM entry parameter's default value is its compiled default,
        // the same as a file-scope uniform's initialiser (uniform-default-records A1).
        // Semantic analysis has already refused a default anywhere the
        // reference refuses one - C1114, only uniform parameters of the
        // SELECTED entry - so anything reaching here with a defaultValue is
        // a legal one.  Refuse rather than drop when it cannot be evaluated,
        // for the reason the file-scope path gives: a compiled default of
        // zero where the source says otherwise is a shader that renders
        // wrong until something patches it.
        if (param->defaultValue && param->storage == StorageQualifier::Uniform)
        {
            std::vector<float>   pInit;
            std::vector<int64_t> pIntInit;
            // NAMED GAP: the reference ACCEPTS a default on an array-typed
            // entry parameter and records one default block per element
            // (measured 2026-09-14, entry-param-default-test row s11 and
            // .local/probe-init/s11_uniform). We do not emit per-element
            // default records yet (array-default-records), so this stays a refusal that
            // names the gap; the array-aware evaluator (file-scope-initializer-fold) must not
            // silently turn it into an acceptance without those records.
            if (param->type && param->type->isArray())
            {
                error(param->loc,
                      "uniform entry parameter '" + param->name +
                      "' is an array with a default value; per-element default records are not emitted yet (array-default-records), refusing rather than dropping the default");
                pInit.clear();
                pIntInit.clear();
            }
            else if (!evaluateConstInitializerTyped(param->defaultValue.get(),
                                               param->type.get(),
                                               pInit, pIntInit, module_.get()))
            {
                error(param->loc,
                      "uniform entry parameter '" + param->name +
                      "' has a default value this compiler cannot evaluate; "
                      "refusing rather than compiling it as zero");
            }
            else
            {
                irParam.initialValue     = std::move(pInit);
                irParam.initialIntValues = std::move(pIntInit);
                // Cg BROADCASTS a scalar default across a vector, the same
                // as a file-scope initialiser: `uniform float3 Ka = 0.6f`
                // means (0.6,0.6,0.6) and the reference records exactly
                // that.  The evaluator reports what the EXPRESSION held -
                // one component - so without this the block is
                // (0.6,0,0,0): right in x and zero elsewhere, which renders
                // as a plausible darker colour rather than as anything
                // obviously wrong.  The file-scope path learned this the
                // same way (review finding, codex); the fixture caught it
                // here before the first review round.
                if (irParam.initialValue.size() == 1)
                {
                    const int declared = irParam.type.componentCount();
                    if (declared > 1)
                    {
                        irParam.initialValue.assign(
                            static_cast<size_t>(declared),
                            irParam.initialValue[0]);
                        if (!irParam.initialIntValues.empty())
                            irParam.initialIntValues.assign(
                                static_cast<size_t>(declared),
                                irParam.initialIntValues[0]);
                    }
                }
                else if (irParam.initialValue.size() >
                         static_cast<size_t>(irParam.type.componentCount()))
                {
                    // NARROWING is legal and the reference does it:
                    // `uniform float2 u = float4(1,2,3,4)` is ACCEPTED and
                    // records [1,2,0,0] - the leading components, zero
                    // padded, the same rule a narrowing cast follows.
                    // Measured; the first version of this refused it.
                    irParam.initialValue.resize(
                        static_cast<size_t>(irParam.type.componentCount()));
                    if (!irParam.initialIntValues.empty())
                        irParam.initialIntValues.resize(
                            static_cast<size_t>(irParam.type.componentCount()));
                }
                else if (!irParam.initialValue.empty() &&
                         irParam.initialValue.size() !=
                             static_cast<size_t>(irParam.type.componentCount()))
                {
                    // Narrower than the declared type and not a single
                    // scalar to broadcast.  checkParameterDefaultShape
                    // refuses the source-level forms of this (a short braced
                    // list, a too-narrow constructor), so reaching here means
                    // a shape that check does not model.  Refuse rather than
                    // pad with zeros: a compiled default of zero where the
                    // source says otherwise renders wrong until something
                    // patches it.
                    error(param->loc,
                          "uniform entry parameter '" + param->name +
                          "' has a " +
                          std::to_string(irParam.initialValue.size()) +
                          "-component default for a " +
                          std::to_string(irParam.type.componentCount()) +
                          "-component type; refusing rather than padding it "
                          "with zeros");
                    irParam.initialValue.clear();
                    irParam.initialIntValues.clear();
                }
            }
        }

        currentFunction_->parameters.push_back(irParam);

        // A parameter that shadows a file-scope variable is a scope like a
        // local's (inline-entry-parameter-scope): the global keeps its own binding in the
        // stash - unassigned at entry - so an inlined helper naming it
        // reads the global, and a helper's write to it lands in the stash,
        // not on the parameter.  Before this the entry function's parameter
        // was the one binding the flat map could not tell from the global.
        stashShadowedGlobal(param->name);
        // Map parameter to value
        declToValue_[param.get()] = irParam.valueId;
        nameToValue_[param->name] = irParam.valueId;
    }

    if (currentFunction_->isEntryPoint &&
        std::any_of(currentFunction_->returnOutputs.begin(), currentFunction_->returnOutputs.end(),
            [](const IRParameter& p) { return p.name.find('[') != std::string::npos; }))
    {
        std::unordered_set<std::string> slots;
        auto occupy = [&](const IRParameter& output) {
            std::string semantic = output.semanticName;
            std::transform(semantic.begin(), semantic.end(), semantic.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (semantic == "TEX") semantic = "TEXCOORD";
            if (semantic == "COL") semantic = "COLOR";
            if (semantic == "HPOS") semantic = "POSITION";
            if (semantic == "PSIZ") semantic = "PSIZE";
            if (semantic == "FOGC") semantic = "FOG";
            if (semantic == "DEPR") semantic = "DEPTH";
            if (!semantic.empty() && !slots.insert(semantic + ":" + std::to_string(output.semanticIndex)).second)
                error(decl->loc, "C5121: multiple bindings to an output array semantic slot");
        };
        for (const auto& output : currentFunction_->returnOutputs) occupy(output);
        for (const auto& parameter : currentFunction_->parameters)
            if (parameter.storage == StorageQualifier::Out || parameter.storage == StorageQualifier::InOut)
                occupy(parameter);
    }

    // Create entry block
    currentBlock_ = currentFunction_->createBlock("entry");

    // A mutable static whose initialiser the constant evaluator cannot fold
    // (`static float g = f(0.25);`) starts at that initialiser BUILT here,
    // at program start: the reference folds the call (measured: a read of g
    // is 0.5).  Only when the built value's type is the declared type - no
    // conversion is invented - otherwise the static stays unseeded and a
    // read refuses as before.  A same-named entry parameter has already
    // stashed the name, so the value goes to the stash, not over the
    // parameter.  An unread static's build is dead and removed.
    if (currentFunction_->isEntryPoint)
    {
        for (VarDecl* decl : runtimeStaticInits_)
        {
            const IRTypeInfo declared = getIRType(decl->type.get());
            const IRTypeInfo built = getExprType(decl->initializer.get());
            if (built.baseType != declared.baseType || built.vectorSize != declared.vectorSize ||
                built.matrixRows != declared.matrixRows || built.isArray() || declared.isArray())
                continue;
            // The initialiser is FILE-SCOPE code: build it with the file-scope
            // bindings, not the entry's parameters that have since shadowed
            // some of them (review: codex - `static float y = f(x);` under
            // `main(float x : TEXCOORD0)` reads the static x, measured 0.5).
            // The stash holds each shadowed name's file-scope binding;
            // restore the entry's view afterwards.
            const auto entryNames = nameToValue_;
            const auto entryArrays = localArrayValues_;
            for (const auto& kv : shadowedGlobals_)
            {
                if (kv.second != InvalidIRValue) nameToValue_[kv.first] = kv.second;
                else nameToValue_.erase(kv.first);
            }
            for (const auto& kv : shadowedGlobalArrays_)
            {
                if (!kv.second.empty()) localArrayValues_[kv.first] = kv.second;
                else localArrayValues_.erase(kv.first);
            }
            const auto fileNames = nameToValue_;
            const auto fileArrays = localArrayValues_;
            const IRValueID value = buildExpr(decl->initializer.get());
            // A helper the initialiser calls may WRITE file-scope state, and
            // the write persists (review: codex; measured: `g = .75` inside
            // f() of `static float y = f();` reads 0.75 afterwards - scalar,
            // array element, a shadowed g, and a later initialiser all
            // see it).  Every binding the build changed is such a write:
            // carry it back, into the stash where the entry shadows it.
            const auto changedNames = nameToValue_;
            const auto changedArrays = localArrayValues_;
            nameToValue_ = entryNames;
            localArrayValues_ = entryArrays;
            for (const auto& kv : changedNames)
            {
                auto before = fileNames.find(kv.first);
                if (before != fileNames.end() && before->second == kv.second) continue;
                if (auto st = shadowedGlobals_.find(kv.first); st != shadowedGlobals_.end())
                    st->second = kv.second;
                else
                    nameToValue_[kv.first] = kv.second;
            }
            for (const auto& kv : changedArrays)
            {
                auto before = fileArrays.find(kv.first);
                if (before != fileArrays.end() && before->second == kv.second) continue;
                if (auto st = shadowedGlobalArrays_.find(kv.first); st != shadowedGlobalArrays_.end())
                    st->second = kv.second;
                else
                    localArrayValues_[kv.first] = kv.second;
            }
            if (value == InvalidIRValue) continue;
            if (auto st = shadowedGlobals_.find(decl->name); st != shadowedGlobals_.end())
                st->second = value;
            else
                nameToValue_[decl->name] = value;
        }
    }

    // A semantic array is an array of input values, not a vector whose
    // lanes are its elements. Seed the same array state used by locals and
    // helper argument snapshots, with an element-typed load for each slot.
    // Only the entry owns attributes: helper parameter names must retain
    // the entry loads (and their reflection provenance) when renamed.
    if (currentFunction_->isEntryPoint)
    {
        const bool vertex = module_->shaderStage == ShaderStage::Vertex;
        auto seedInputs = [&](auto& self, TypeNode* type, const std::string& root,
                              const std::string& prefix) -> void {
            const auto* fields = getStructFields(type);
            if (!fields) return;
            for (const auto& field : *fields)
            {
                const std::string path = prefix.empty() ? field.name : prefix + "." + field.name;
                if (getStructFields(field.type.get()))
                {
                    self(self, field.type.get(), root, path);
                    continue;
                }
                IRTypeInfo elementType = getIRType(field.type.get());
                if (!elementType.isArray() || elementType.isMatrix()) continue;
                const int count = elementType.arraySize;
                elementType.arraySize = 0;
                std::string semantic = field.semantic.name;
                std::transform(semantic.begin(), semantic.end(), semantic.begin(),
                    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                int base = field.semantic.index;
                bool repeated = false;
                if (vertex && (semantic == "POSITION" || semantic == "NORMAL" ||
                         semantic == "TANGENT" || semantic == "BINORMAL" ||
                         semantic == "DIFFUSE" || semantic == "SPECULAR"))
                    repeated = true;
                else if (!vertex && (semantic == "FOG" || semantic == "FOGC"))
                    repeated = true;
                // Diagnose unmodelled semantics and range overflow only for
                // loads that survive DCE. The reference accepts an unused
                // overflowing tail, and a read of its valid first element.
                auto& values = localArrayValues_[root + "." + path];
                values.clear();
                for (int i = 0; i < count; ++i)
                {
                    const IRValueID value = currentFunction_->allocateValueId();
                    auto load = std::make_unique<IRInstruction>(IROp::LoadAttribute, value, elementType);
                    load->semanticName = semantic;
                    load->rawSemanticName = field.semantic.rawName;
                    load->inferredSemantic = field.semantic.inferred;
                    load->semanticIndex = base + (repeated ? 0 : i);
                    load->structParamName = root;
                    load->fieldName = path + "[" + std::to_string(i) + "]";
                    currentBlock_->addInstruction(std::move(load));
                    values.push_back(value);
                }
            }
        };
        for (const auto& param : decl->parameters)
            if (param->storage != StorageQualifier::Uniform &&
                param->storage != StorageQualifier::Out && param->storage != StorageQualifier::InOut)
                seedInputs(seedInputs, param->type.get(), param->name, "");
    }

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
    flattenedUniformStructParams_.clear();
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
    case StmtKind::Empty:
        // The enclosing statement still builds its condition, side effects
        // and control-flow edges. Only the empty body has no instructions.
        break;
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
    // The fourth row of ScopeState's boundary table: a block's own
    // declarations end with it.  Snapshot first; the declared set fills as
    // the declarators are built (no pre-scan - see blockDeclared_).
    const ScopeState pre = scope_;
    blockDeclared_.emplace_back();
    for (auto& s : stmt->statements)
    {
        // A selected literal arm can return directly in the entry block.
        // Its suffix is unreachable, but block bindings still unwind below.
        if (module_->shaderStage == ShaderStage::Fragment &&
            currentFunction_->isEntryPoint && currentBlock_->hasTerminator() &&
            currentBlock_->instructions.back()->op == IROp::Return)
            break;
        buildStmt(s.get());
    }
    const std::unordered_set<std::string> declared = std::move(blockDeclared_.back());
    blockDeclared_.pop_back();
    for (const auto& name : declared)
        exitBlockBinding(name, pre);
}

// Undo ONE name the block declared.  The pre-block binding of the name comes
// back (or the name is unbound); if the block's declaration is what moved a
// file-scope variable's binding into the stash, the global comes back OUT of
// the stash - its value as the block (or a helper the block called) left it -
// and the stash entry goes.  A stash entry that existed before the block
// belongs to an outer local or parameter and is left alone: restoring the
// outer binding is all the inner shadow's end means.
void IRBuilder::exitBlockBinding(const std::string& name, const ScopeState& pre)
{
    // A declared STRUCT owns qualified keys too ("s.f", "s.f.xyz", nested
    // "s.a.b"): buildDeclStmt seeds them and field assignment rebinds them,
    // so they end with the declaration exactly as the bare name does
    // (review: codex - an inner `S s` left its s.f visible after the block,
    // and the return read 3p where the reference reads the outer p).
    const std::string prefix = name + ".";
    auto owned = [&](const std::string& key) {
        return key == name || key.compare(0, prefix.size(), prefix) == 0;
    };
    std::vector<std::string> keys;
    for (const auto& kv : nameToValue_) if (owned(kv.first)) keys.push_back(kv.first);
    for (const auto& kv : pre.names)   if (owned(kv.first)) keys.push_back(kv.first);
    for (const auto& key : keys)
    {
        auto it = pre.names.find(key);
        if (it != pre.names.end()) nameToValue_[key] = it->second;
        else nameToValue_.erase(key);
    }
    keys.clear();
    for (const auto& kv : localArrayValues_) if (owned(kv.first)) keys.push_back(kv.first);
    for (const auto& kv : pre.arrays)       if (owned(kv.first)) keys.push_back(kv.first);
    for (const auto& key : keys)
    {
        auto ait = pre.arrays.find(key);
        if (ait != pre.arrays.end()) localArrayValues_[key] = ait->second;
        else localArrayValues_.erase(key);
    }

    // UNSTASH every global key the declared name owns whose stash entry
    // this block created (a pre-existing entry belongs to an outer shadow
    // and keeps the value a helper gave it).
    keys.clear();
    for (const auto& kv : shadowedGlobals_)
        if (owned(kv.first) && !pre.shadowedGlobals.count(kv.first)) keys.push_back(kv.first);
    for (const auto& key : keys)
    {
        auto st = shadowedGlobals_.find(key);
        if (st->second != InvalidIRValue) nameToValue_[key] = st->second;
        else nameToValue_.erase(key);
        shadowedGlobals_.erase(st);
        auto sa = shadowedGlobalArrays_.find(key);
        if (sa != shadowedGlobalArrays_.end())
        {
            if (!sa->second.empty()) localArrayValues_[key] = sa->second;
            else localArrayValues_.erase(key);
            shadowedGlobalArrays_.erase(sa);
        }
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
    // Macro debug switches commonly arrive as literal 0/1. Semantic analysis
    // has already checked both arms; building a dead return would leave a
    // spurious multi-exit CFG. Keep this slice to FP entry literals: no local,
    // uniform, side-effecting expression or reduced-precision guess is folded.
    if (module_->shaderStage == ShaderStage::Fragment &&
        currentFunction_->isEntryPoint && stmt->condition &&
        stmt->condition->kind == ExprKind::Literal)
    {
        if (const std::optional<bool> taken = constCondition(stmt->condition.get()))
        {
            buildStmt(*taken ? stmt->thenBranch.get() : stmt->elseBranch.get());
            return;
        }
    }
    const std::function<void()> thenArm = [&] { buildStmt(stmt->thenBranch.get()); };
    const std::function<void()> elseArm = [&] { buildStmt(stmt->elseBranch.get()); };
    buildConditional(stmt->condition.get(), stmt->loc, thenArm, stmt->elseBranch ? &elseArm : nullptr);
}

// The if/else join, with its arms supplied by the caller: buildIfStmt passes
// the two branch statements; an inlined helper whose return sits under a
// run-time condition passes "this arm, then the rest of the body"
// (runInlineStatements), so the same Select join merges its result.
void IRBuilder::buildConditional(ExprNode* condition, SourceLocation loc,
                                 const std::function<void()>& thenArm,
                                 const std::function<void()>* elseArm)
{
    // Evaluate condition
    IRValueID condValue = buildExpr(condition);
    condValue = normalizeCondition(condition, condValue);

    // Create blocks
    IRBasicBlock* thenBlock = currentFunction_->createBlock(makeLabel("if.then"));
    IRBasicBlock* elseBlock = elseArm
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
    // this replaces: local-array-conditional-store (arrays never joined), user-function-inlining review
    // (the stashes added and missed the same way).
    const ScopeState preIf = scope_;
    auto preIfMap = preIf.fold();

    // Build then block
    currentBlock_ = thenBlock;
    thenArm();
    const bool thenTerminated = currentBlock_->hasTerminator();
    if (!thenTerminated)
    {
        emitBranch(mergeBlock);
    }
    auto postThenMap = scope_.fold();

    // Build else block
    bool elseTerminated = false;
    std::unordered_map<std::string, IRValueID> postElseMap;
    if (elseArm)
    {
        scope_ = preIf;  // restore before processing else
        currentBlock_ = elseBlock;
        (*elseArm)();
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
    // SORTED key order, never in a hash map's order (deterministic-join-order class).
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
        load->uniformSource = global->valueId;
        load->targetName = base;
        if (index >= 0)
        {
            load->componentIndex = static_cast<int>(index);
            load->arrayIndexKind = IRInstruction::ArrayIndexKind::Constant;
        }
        load->loc = loc;
        currentBlock_->addInstruction(std::move(load));
        preIfMap[key] = loadId;
        nameToValue_[key] = loadId;
    }

    // A varying struct member of the entry is bound lazily, at its first
    // use.  When an arm ASSIGNS it before any read (`if (c) v.x = 0.75;`),
    // the other side never bound it, yet its value there is the INPUT - not
    // undefined (review: codex, join-input-write).  Load the input here at
    // the merge so the join selects against it.  A key every arm only READ
    // is left alone: both sides are the same input, and the join below
    // takes the load as it is.
    // True only for the load OF THIS KEY's input - the same parameter and
    // member - not any input load: `v.x = v.y` binds v.x to v.y's load,
    // and treating that as a read of v.x took y on the untouched side too
    // (review: codex, join-other-input-write).
    auto isLoadOf = [&](IRValueID id, const std::string& key) {
        const size_t dot = key.find('.');
        if (dot == std::string::npos) return false;
        for (const auto& bp : currentFunction_->blocks)
            if (bp)
                for (const auto& ip : bp->instructions)
                    if (ip && ip->result == id)
                        return ip->op == IROp::LoadAttribute &&
                               ip->structParamName == key.substr(0, dot) &&
                               ip->fieldName == key.substr(dot + 1);
        return false;
    };
    std::vector<std::string> untouchedInputs;
    if (currentFunctionDecl_)
        for (const auto* post : {&postThenMap, &postElseMap})
            for (const auto& kv : *post)
            {
                const std::string& key = kv.first;
                if (preIfMap.count(key) || isLoadOf(kv.second, key)) continue;
                const size_t dot = key.find('.');
                if (dot == std::string::npos || key.find_first_of(".[@", dot + 1) != std::string::npos)
                    continue;
                untouchedInputs.push_back(key);
            }
    std::sort(untouchedInputs.begin(), untouchedInputs.end());
    untouchedInputs.erase(std::unique(untouchedInputs.begin(), untouchedInputs.end()),
                          untouchedInputs.end());
    for (const std::string& key : untouchedInputs)
    {
        const size_t dot = key.find('.');
        const std::string base = key.substr(0, dot);
        for (const auto& param : currentFunctionDecl_->parameters)
        {
            if (!param || param->name != base) continue;
            if (param->storage == StorageQualifier::Uniform ||
                param->storage == StorageQualifier::Out ||
                param->storage == StorageQualifier::InOut) break;
            const IRValueID loadId = currentFunction_->allocateValueId();
            if (emitInputMemberLoad(param.get(), key.substr(dot + 1), loadId))
            {
                preIfMap[key] = loadId;
                nameToValue_[key] = loadId;
            }
            break;
        }
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
        // (vector-input-select-lanes, measured 2026-09-02 against the reference; the
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

    // A value nothing defines: no id at all, or a function-local id that is
    // not a constant, not a parameter and not an instruction's result (an
    // uninitialised local's declaration binds such an id).  File-scope ids
    // live above kGlobalIdBase and are always defined.
    auto isUndefinedValue = [&](IRValueID id) -> bool
    {
        if (id == InvalidIRValue) return true;
        if (id >= IRModule::kGlobalIdBase) return false;
        if (currentFunction_->getValue(id)) return false;
        for (const IRParameter& param : currentFunction_->parameters)
            if (param.valueId == id) return false;
        for (const auto& bp : currentFunction_->blocks)
        {
            if (!bp) continue;
            for (const auto& ip : bp->instructions)
                if (ip && ip->result == id) return false;
        }
        return true;
    };

    // Collect every name that appears in either post-map.
    //
    // THE ORDER OF THIS LOOP IS PART OF THE COMPILER'S OUTPUT.  It decides the
    // order the join's Select instructions are emitted in, which decides SSA
    // numbering, the virtual program, and register allocation.  Iterating an
    // unordered_set here made that order the standard library's string-hash
    // bucket order, so an MSVC-built compiler and a gcc-built compiler emitted
    // DIFFERENT PROGRAMS from the same source - measured on 2026-09-06, three
    // of 421 corpus shaders (deterministic-join-order).  Never iterate an unordered container
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
        if (elseArm)
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

        // One side with no definition takes the other side, no Select - in
        // exactly two shapes, because a Select against nothing left an
        // operand the lowering could not resolve:
        //  - no binding at all (InvalidIRValue) where the other side is an
        //    input LOAD: a varying member first READ inside one arm
        //    (lanczos_horiz reads `vertex.one` only under an if).  It is the
        //    same input wherever it sits.  A member an arm ASSIGNED got its
        //    input loaded at the merge above, so it never reaches here.
        //  - a local declared without an initialiser (its declaration id,
        //    which nothing defines) that a path never writes: the xbr
        //    family's `pix1`/`blend1`.  Undefined in Cg; the reference reads
        //    whatever its register holds on that path (measured: an if /
        //    else-if writing `p` with no final else reads an unwritten r2
        //    where neither arm ran).  Taking the written arm is the existing
        //    rule for local elements above.
        // Anything else keeps the Select and its refusal.
        //
        // The uninitialised local does NOT take the written arm, though: the
        // pixel judge showed the xbr family (2xbr-lv3-pass0, xbr-lv3-pass0,
        // 2xbr-hybrid-v4-gamma) rendering wrong that way.  The reference's
        // unwritten register reads ZERO at run time (fragment temps start at
        // 0), so `blend1` on the no-branch path is 0 and `lerp(E, pix1, 0)`
        // is E; taking another arm's blend painted a blend the reference
        // never paints.  That side becomes a zero of the joined type, and the
        // Select below picks it.
        IRValueID joinThen = thenVal, joinElse = elseVal;
        {
            if (thenVal == InvalidIRValue && isLoadOf(elseVal, name)) { nameToValue_[name] = elseVal; continue; }
            if (elseVal == InvalidIRValue && isLoadOf(thenVal, name)) { nameToValue_[name] = thenVal; continue; }
            const auto zeroOf = [&](IRValueID like) {
                const IRTypeInfo t = getValueType(like);
                const int n = std::max(1, t.componentCount());
                return createConstant(t, std::vector<float>(static_cast<size_t>(n), 0.0f),
                                      std::vector<int64_t>(static_cast<size_t>(n), 0));
            };
            // Measured for FRAGMENT programs only (pixel judge).  A vertex
            // program keeps main's behaviour: the Select against nothing
            // stays, and the lowering refuses the unresolved operand.
            const bool fragment = module_->shaderStage == ShaderStage::Fragment;
            const bool thenUndef = fragment && thenVal != InvalidIRValue && isUndefinedValue(thenVal);
            const bool elseUndef = fragment && elseVal != InvalidIRValue && isUndefinedValue(elseVal);
            if (thenUndef && !isUndefinedValue(elseVal)) joinThen = zeroOf(elseVal);
            else if (elseUndef && !isUndefinedValue(thenVal)) joinElse = zeroOf(thenVal);
        }

        IRTypeInfo selType = getValueType(joinThen);
        if (selType.baseType == IRType::Void)
            selType = getValueType(joinElse);

        IRValueID selId = currentFunction_->allocateValueId();
        auto sel = std::make_unique<IRInstruction>(
            IROp::Select, selId, selType);
        sel->addOperand(condValue);
        sel->addOperand(joinThen);
        sel->addOperand(joinElse);
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

// The body proof is keyed by resolved declaration, not spelling: an inner
// local named i must not invalidate a loop over an outer i.
bool loopBodyChanges(StmtNode* body, const std::unordered_set<DeclNode*>& dependencies,
                     bool watchesGlobal)
{
    auto writesDependency = [&](ExprNode* e) {
        while (e && (e->kind == ExprKind::MemberAccess || e->kind == ExprKind::Index))
            e = e->kind == ExprKind::MemberAccess
                ? static_cast<MemberAccessExpr*>(e)->object.get()
                : static_cast<IndexExpr*>(e)->array.get();
        return e && e->kind == ExprKind::Identifier &&
            dependencies.count(static_cast<IdentifierExpr*>(e)->resolvedDecl) != 0;
    };
    std::function<bool(ExprNode*)> expr;
    expr = [&](ExprNode* e) -> bool {
        if (!e) return false;
        switch (e->kind) {
        case ExprKind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            const bool assignment = b->op == BinaryOp::Assign || b->op == BinaryOp::AddAssign ||
                b->op == BinaryOp::SubAssign || b->op == BinaryOp::MulAssign ||
                b->op == BinaryOp::DivAssign || b->op == BinaryOp::ModAssign;
            return (assignment && writesDependency(b->left.get())) ||
                expr(b->left.get()) || expr(b->right.get());
        }
        case ExprKind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            const bool changes = u->op == UnaryOp::PreIncrement || u->op == UnaryOp::PostIncrement ||
                u->op == UnaryOp::PreDecrement || u->op == UnaryOp::PostDecrement;
            return (changes && writesDependency(u->operand.get())) || expr(u->operand.get());
        }
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            if (c->resolvedFunction && c->resolvedFunction->kind == DeclKind::Function) {
                // A source callee can modify a watched global without an argument.
                if (watchesGlobal) return true;
                auto* f = static_cast<FunctionDecl*>(c->resolvedFunction);
                for (size_t i = 0; i < c->arguments.size() && i < f->parameters.size(); ++i)
                    if ((f->parameters[i]->storage == StorageQualifier::Out ||
                         f->parameters[i]->storage == StorageQualifier::InOut) &&
                        writesDependency(c->arguments[i].get())) return true;
            }
            // The supported ordinary math/texture builtins have no global side
            // effects. These output-argument forms must not bypass the proof.
            if (c->functionName == "sincos" || c->functionName == "modf" ||
                c->functionName == "frexp")
                for (const auto& a : c->arguments) if (writesDependency(a.get())) return true;
            for (const auto& a : c->arguments) if (expr(a.get())) return true;
            return false;
        }
        case ExprKind::Constructor:
            for (const auto& a : static_cast<ConstructorExpr*>(e)->arguments)
                if (expr(a.get())) return true;
            return false;
        case ExprKind::Cast: return expr(static_cast<CastExpr*>(e)->operand.get());
        case ExprKind::MemberAccess: return expr(static_cast<MemberAccessExpr*>(e)->object.get());
        case ExprKind::Index: {
            auto* i = static_cast<IndexExpr*>(e);
            return expr(i->array.get()) || expr(i->index.get());
        }
        case ExprKind::Ternary: {
            auto* t = static_cast<TernaryExpr*>(e);
            return expr(t->condition.get()) || expr(t->thenExpr.get()) || expr(t->elseExpr.get());
        }
        case ExprKind::Identifier:
        case ExprKind::Literal:
        case ExprKind::Sizeof: return false;
        default: return true; // Unknown side effects: do not speculate.
        }
    };
    std::function<bool(StmtNode*)> stmt;
    stmt = [&](StmtNode* node) -> bool {
        if (!node) return false;
        switch (node->kind) {
        case StmtKind::Expr: return expr(static_cast<ExprStmt*>(node)->expr.get());
        case StmtKind::Decl:
            for (const auto& d : static_cast<DeclStmt*>(node)->declarations) {
                if (!d || d->kind != DeclKind::Variable) return true;
                if (expr(static_cast<VarDecl*>(d.get())->initializer.get())) return true;
            }
            return false;
        case StmtKind::Block:
            for (const auto& child : static_cast<BlockStmt*>(node)->statements)
                if (stmt(child.get())) return true;
            return false;
        case StmtKind::If: {
            auto* i = static_cast<IfStmt*>(node);
            return expr(i->condition.get()) || stmt(i->thenBranch.get()) || stmt(i->elseBranch.get());
        }
        case StmtKind::For: {
            auto* f = static_cast<ForStmt*>(node);
            return stmt(f->init.get()) || expr(f->condition.get()) ||
                expr(f->increment.get()) || stmt(f->body.get());
        }
        case StmtKind::While: {
            auto* w = static_cast<WhileStmt*>(node);
            return expr(w->condition.get()) || stmt(w->body.get());
        }
        case StmtKind::DoWhile: {
            auto* w = static_cast<DoWhileStmt*>(node);
            return stmt(w->body.get()) || expr(w->condition.get());
        }
        case StmtKind::Switch: {
            auto* sw = static_cast<SwitchStmt*>(node);
            return expr(sw->expr.get()) || stmt(sw->body.get());
        }
        case StmtKind::Case: return expr(static_cast<CaseStmt*>(node)->value.get());
        case StmtKind::Return: return expr(static_cast<ReturnStmt*>(node)->value.get());
        case StmtKind::Default:
        case StmtKind::Break:
        case StmtKind::Continue:
        case StmtKind::Discard:
        case StmtKind::Empty: return false;
        default: return true;
        }
    };
    return stmt(body);
}

}  // namespace

bool IRBuilder::tryUnrollStaticFor(ForStmt* stmt)
{
    if (!stmt->init || !stmt->condition || !stmt->increment || !stmt->body)
        return false;

    std::unordered_set<DeclNode*> dependencies;
    bool watchesGlobal = false;
    bool unsignedInduction = false;
    DeclNode* induction = nullptr;
    // Read only constants already known at this point. Never build an expression
    // speculatively: failing recognition must leave the ordinary CFG path intact.
    std::function<bool(ExprNode*, int64_t&)> constant;
    constant = [&](ExprNode* e, int64_t& value) -> bool {
        if (!e) return false;
        // The initializer's IR payload may not carry a variable's declared
        // type. In particular, negating uint(1) is not signed -1.
        const IRTypeInfo scalarType = getExprType(e);
        if (!scalarType.isScalar() || (scalarType.baseType != IRType::Int32 &&
            scalarType.baseType != IRType::Float32 &&
            !(unsignedInduction && scalarType.baseType == IRType::UInt32))) return false;
        double number;
        if (e->kind == ExprKind::Unary) {
            auto* u = static_cast<UnaryExpr*>(e);
            // Unsigned support is deliberately non-negating and non-wrapping.
            if (unsignedInduction) return false;
            if (u->op != UnaryOp::Negate) return false;
            if (!constant(u->operand.get(), value)) return false;
            // Integer simulation cannot retain a floating negative-zero sign.
            // It is observable in the body (e.g. division by the induction value).
            if (value == 0 && scalarType.baseType == IRType::Float32) return false;
            if (u->op == UnaryOp::Negate) value = -value;
            return true;
        }
        if (e->kind == ExprKind::Literal) {
            auto* l = static_cast<LiteralExpr*>(e);
            if (l->literalKind == LiteralExpr::LiteralKind::Int)
                number = static_cast<double>(std::get<int64_t>(l->value));
            else if (l->literalKind == LiteralExpr::LiteralKind::Float)
            {
                number = std::get<double>(l->value);
                // Do not treat a rounded float literal as an exact uint input.
                if (unsignedInduction && double(static_cast<float>(number)) != number) return false;
            }
            else return false;
        } else if (e->kind == ExprKind::Identifier) {
            auto* id = static_cast<IdentifierExpr*>(e);
            if (!id->resolvedDecl || id->resolvedDecl == induction) return false;
            auto it = nameToValue_.find(id->name);
            if (it != nameToValue_.end()) {
                auto* c = dynamic_cast<IRConstant*>(currentFunction_->getValue(it->second));
                if (!c || !c->type.isScalar()) return false;
                if (std::holds_alternative<int32_t>(c->value)) number = std::get<int32_t>(c->value);
                else if (unsignedInduction && std::holds_alternative<uint32_t>(c->value)) number = std::get<uint32_t>(c->value);
                else if (std::holds_alternative<float>(c->value)) number = std::get<float>(c->value);
                else return false;
            } else {
                // An absent local SSA binding is not permission to fall back by
                // spelling. Only this resolved file-scope declaration qualifies.
                if (!globalDeclarations_.count(id->resolvedDecl)) return false;
                const auto* g = module_->findGlobal(id->name);
                if (!g || g->storage != StorageQualifier::Const || !g->declaredStatic ||
                    !g->type.isScalar()) return false;
                if (scalarType.baseType == IRType::Int32 || scalarType.baseType == IRType::UInt32) {
                    if (g->initialIntValues.size() != 1) return false;
                    number = static_cast<double>(g->initialIntValues[0]);
                } else {
                    if (g->initialValue.size() != 1) return false;
                    number = g->initialValue[0];
                }
            }
            dependencies.insert(id->resolvedDecl);
            watchesGlobal |= module_->findGlobal(id->name) != nullptr;
        } else return false;
        if (!std::isfinite(number) || (number == 0 && std::signbit(number)) ||
            std::trunc(number) != number ||
            number < (unsignedInduction ? 0.0 : double(INT32_MIN)) ||
            number > (unsignedInduction ? double(UINT32_MAX) : double(INT32_MAX))) return false;
        value = static_cast<int64_t>(number);
        return true;
    };

    std::string varName;
    ExprNode* initial = nullptr;
    IRTypeInfo type;
    const bool declares = stmt->init->kind == StmtKind::Decl;
    if (declares) {
        auto* d = static_cast<DeclStmt*>(stmt->init.get());
        if (d->declarations.size() != 1 || !d->declarations[0] ||
            d->declarations[0]->kind != DeclKind::Variable) return false;
        auto* v = static_cast<VarDecl*>(d->declarations[0].get());
        varName = v->name; induction = v; initial = v->initializer.get();
        type = getIRType(v->type.get());
    } else if (stmt->init->kind == StmtKind::Expr) {
        auto* e = static_cast<ExprStmt*>(stmt->init.get())->expr.get();
        if (!e || e->kind != ExprKind::Binary) return false;
        auto* b = static_cast<BinaryExpr*>(e);
        if (b->op != BinaryOp::Assign || b->left->kind != ExprKind::Identifier) return false;
        auto* id = static_cast<IdentifierExpr*>(b->left.get());
        varName = id->name; induction = id->resolvedDecl; initial = b->right.get();
        type = getExprType(id);
    } else return false;
    if (!induction || !type.isScalar() ||
        (type.baseType != IRType::Int32 && type.baseType != IRType::Float32 &&
         type.baseType != IRType::UInt32)) return false;
    unsignedInduction = type.baseType == IRType::UInt32;
    const bool floating = type.baseType == IRType::Float32;
    auto isInduction = [&](ExprNode* e) {
        return e && e->kind == ExprKind::Identifier &&
            static_cast<IdentifierExpr*>(e)->resolvedDecl == induction;
    };
    int64_t first, bound, step;
    if (!constant(initial, first)) return false;
    if (stmt->condition->kind != ExprKind::Binary) return false;
    auto* cond = static_cast<BinaryExpr*>(stmt->condition.get());
    const BinaryOp cmp = cond->op;
    if (cmp != BinaryOp::Less && cmp != BinaryOp::LessEqual && cmp != BinaryOp::Greater &&
        cmp != BinaryOp::GreaterEqual && cmp != BinaryOp::Equal && cmp != BinaryOp::NotEqual) return false;
    if (!isInduction(cond->left.get()) || !constant(cond->right.get(), bound)) return false;
    auto* inc = stmt->increment.get();
    if (inc->kind == ExprKind::Unary) {
        auto* u = static_cast<UnaryExpr*>(inc);
        if (!isInduction(u->operand.get())) return false;
        if (u->op == UnaryOp::PreIncrement || u->op == UnaryOp::PostIncrement) step = 1;
        else if (u->op == UnaryOp::PreDecrement || u->op == UnaryOp::PostDecrement) step = -1;
        else return false;
    } else if (inc->kind == ExprKind::Binary) {
        auto* b = static_cast<BinaryExpr*>(inc);
        if (!isInduction(b->left.get()) || !constant(b->right.get(), step)) return false;
        if (b->op == BinaryOp::SubAssign) step = -step;
        else if (b->op != BinaryOp::AddAssign) return false;
    } else return false;
    if (step == 0 || stmtContainsBreakOrContinue(stmt->body.get())) return false;
    dependencies.insert(induction);
    // A global induction target can also be changed by an otherwise unrelated call.
    watchesGlobal |= !declares && module_->findGlobal(varName) != nullptr;
    if (loopBodyChanges(stmt->body.get(), dependencies, watchesGlobal)) return false;

    const bool floatComparison = floating || getExprType(cond->right.get()).baseType == IRType::Float32;
    auto exact = [&](int64_t value) {
        return value >= (unsignedInduction ? int64_t(0) : int64_t(INT32_MIN)) &&
            value <= (unsignedInduction ? int64_t(UINT32_MAX) : int64_t(INT32_MAX)) &&
            (!floatComparison || static_cast<double>(static_cast<float>(value)) == static_cast<double>(value));
    };
    if (!exact(first) || !exact(bound) || (floating && !exact(step))) return false;
    auto test = [&](int64_t value) {
        switch (cmp) {
        case BinaryOp::Less: return value < bound;
        case BinaryOp::LessEqual: return value <= bound;
        case BinaryOp::Greater: return value > bound;
        case BinaryOp::GreaterEqual: return value >= bound;
        case BinaryOp::Equal: return value == bound;
        case BinaryOp::NotEqual: return value != bound;
        default: return false;
        }
    };
    // OUR resource bound, not the reference compiler's body/type-dependent
    // unroll heuristic (hardware-loop-selection). Complete the proof before emitting IR.
    constexpr size_t kMaxUnroll = 64;
    std::vector<int64_t> iterations;
    int64_t final = first;
    while (test(final)) {
        if (iterations.size() == kMaxUnroll) {
            error(stmt->loc, "static loop expansion exceeds 64 iterations; hardware-loop lowering required (static-loop-expansion)");
            return true;
        }
        iterations.push_back(final);
        final += step; // 32-bit operands, so the int64 simulation cannot overflow.
        // Descending unsigned loops are valid until a simulated value underflows.
        if (!exact(final)) return false;
    }

    const ScopeState pre = scope_;
    // The for initializer has its own lexical scope, rather than declaring its
    // name in the enclosing block's declaration set.
    blockDeclared_.emplace_back();
    buildStmt(stmt->init.get());
    auto bind = [&](int64_t value) {
        nameToValue_[varName] = floating ? createConstant(static_cast<float>(value))
            : unsignedInduction ? createConstant(static_cast<uint32_t>(value))
                                : createConstant(static_cast<int32_t>(value));
    };
    for (int64_t value : iterations) {
        bind(value);
        buildStmt(stmt->body.get());
        if (currentBlock_->hasTerminator()) break;
    }
    if (!currentBlock_->hasTerminator()) bind(final);
    blockDeclared_.pop_back();
    if (declares) exitBlockBinding(varName, pre);
    return true;
}

void IRBuilder::buildForStmt(ForStmt* stmt)
{
    // Proven static loops expand within our resource bound. The reference's
    // choice between expansion and a hardware loop also depends on body cost;
    // we preserve values without claiming the same instruction shape.
    if (tryUnrollStaticFor(stmt)) return;

    // Inlining supports proven expansion only. Falling through to a CFG here
    // would admit loop-carried bindings the inliner cannot yet preserve.
    if (!inlineStack_.empty()) {
        error(stmt->loc, "cannot inline user function '" + inlineStack_.back()->name +
              "': unproven for-loop; hardware-loop lowering required (static-loop-expansion)");
        return;
    }

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
        condValue = normalizeCondition(stmt->condition.get(), condValue);
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
    condValue = normalizeCondition(stmt->condition.get(), condValue);
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
    condValue = normalizeCondition(stmt->condition.get(), condValue);
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
// "a" keeps its own key, so two members never merge (struct-member-lvalue-swizzle).
//
// Only the LOWERCASE rgba/stpq spellings are mapped - the ones the reference
// treats as equivalent to xyzw (measured: it accepts .rgb and .xyz alike).
// An UPPERCASE output swizzle (.XYZ) is left unchanged: the reference rejects
// it with C1048 "invalid character in swizzle", and our compiler also refuses
// it, unchanged by this slice; that our validator accepts the uppercase
// spelling at all is a separate upstream divergence (uppercase-swizzle-acceptance).
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
        if (currentFunctionDecl_)
            retValue = narrowToScalar(currentFunctionDecl_->returnType.get(), stmt->value.get(), retValue);

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

                    // Each written element owns one output semantic. The
                    // declaration records remain present for unwritten
                    // elements, but there is no write and no invented zero.
                    if (field.type && getIRType(field.type.get()).isArray() &&
                        localArrayValues_.count(fieldKey))
                    {
                        const auto& elements = localArrayValues_.at(fieldKey);
                        IRTypeInfo elementType = getIRType(field.type.get());
                        if (elementType.isMatrix())
                        {
                            error(stmt->loc, "matrix-array output emission is not supported (member-array-storage)");
                            continue;
                        }
                        elementType.arraySize = 0;
                        for (size_t i = 0; i < elements.size(); ++i)
                        {
                            if (elements[i] == InvalidIRValue) continue;
                            auto output = std::make_unique<IRInstruction>(IROp::StoreOutput,
                                InvalidIRValue, elementType);
                            output->addOperand(elements[i]);
                            output->semanticName = field.semantic.name;
                            output->rawSemanticName = field.semantic.rawName;
                            output->inferredSemantic = field.semantic.inferred;
                            output->semanticIndex = field.semantic.index + static_cast<int>(i);
                            output->fieldName = fieldPath + "[" + std::to_string(i) + "]";
                            currentBlock_->addInstruction(std::move(output));
                        }
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
                    inst->inferredSemantic = field.semantic.inferred;
                    inst->semanticIndex   = field.semantic.index;
                    inst->fieldName       = fieldPath;
                    currentBlock_->addInstruction(std::move(inst));
                }
            };

            emitStructOutputs(emitStructOutputs, baseName, "", structType);
            if (currentFunction_->isEntryPoint && module_->shaderStage == ShaderStage::Vertex &&
                std::any_of(currentFunction_->returnOutputs.begin(), currentFunction_->returnOutputs.end(),
                    [](const IRParameter& p) { return p.name.find('[') != std::string::npos; }))
            {
                bool positionWritten = false;
                for (const auto& block : currentFunction_->blocks)
                    for (const auto& instruction : block->instructions)
                        if (instruction->op == IROp::StoreOutput)
                        {
                            std::string semantic = instruction->semanticName;
                            std::transform(semantic.begin(), semantic.end(), semantic.begin(),
                                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                            if (semantic == "POSITION" || semantic == "HPOS") positionWritten = true;
                        }
                if (!positionWritten)
                    error(stmt->loc, "C6014: output array program does not write required HPOS");
            }
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
        const ExprNode* outer = statementExpr_;
        statementExpr_ = stmt->expr.get();
        buildExpr(stmt->expr.get());
        statementExpr_ = outer;
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
    if (name.empty()) return;
    // A global STRUCT is flattened into globals named by its qualified keys
    // ("s.f"), and a local `S s` shadows every one of them: each key the
    // declared name owns gets its own stash entry, keyed by that qualified
    // name, so a helper's write to the real global field is routed to the
    // stash and comes back out at block exit (review: codex - the bare-name
    // stash never fired for a struct, findGlobal("s") being null, and the
    // helper's write was restored away).
    const std::string prefix = name + ".";
    auto stashKey = [&](const std::string& key)
    {
        if (shadowedGlobals_.count(key)) return;   // captured ONCE
        auto prior = nameToValue_.find(key);
        shadowedGlobals_[key] =
            prior != nameToValue_.end() ? prior->second : InvalidIRValue;
        auto priorArr = localArrayValues_.find(key);
        shadowedGlobalArrays_[key] =
            priorArr != localArrayValues_.end() ? priorArr->second : std::vector<IRValueID>{};
    };
    for (const auto& g : module_->globals)
        if (g.name == name || g.name.compare(0, prefix.size(), prefix) == 0)
            stashKey(g.name);
}

void IRBuilder::buildDeclStmt(DeclStmt* stmt)
{
    // Every declarator in the statement, in source order, so `float a, b;`
    // builds both and an initialiser list `float a = x, b = y;` evaluates
    // them left to right (multiple-declarators).
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
        if (!blockDeclared_.empty())
            blockDeclared_.back().insert(varDecl->name);   // undone at this block's exit
        // Allocate a value for the variable
        IRValueID varId = currentFunction_->allocateValueId();
        declToValue_[varDecl] = varId;
        nameToValue_[varDecl->name] = varId;
        IRTypeInfo varType = getIRType(varDecl->type.get());
        if (varType.isArray() && varType.arraySize > 0)
            localArrayValues_[varDecl->name].assign(
                static_cast<size_t>(varType.arraySize), InvalidIRValue);

        // A STRUCT INITIALISED FROM A SCALAR - `OUT o = (OUT)0;` - fills
        // every leaf with that scalar.  It is how the reference SDK's own
        // shaders clear an output struct, and semantic analysis used to
        // refuse it outright ("cannot cast from 'int' to 'struct'").
        //
        // Expanded HERE rather than in buildCastExpr because a struct is not
        // a value in this builder: its fields are bound by name, exactly as
        // an uninitialised struct's vector leaves are bound below.  Binding
        // the leaves is what lets a later `o.c.xy = p.xy` build the ordinary
        // VecInsert chain on top of the zero rather than on nothing.
        //
        // The zeros ARE emitted.  The reference drops the ones that no later
        // write touches - `float4 z = (float4)0; z.xy = p.xy;` compiles to a
        // single MOV of the xy lanes there - but that is only sound if an
        // unwritten register reads as zero, and RPCS3's zero-initialisation
        // of every fragment local (FragmentProgramDecompiler AddReg) cannot
        // tell a hardware guarantee from an emulator convenience; a physical
        // register reused after another value's live range holds that value
        // either way (codex, 2026-09-07).  So: correct now, shorter later.
        if (varDecl->initializer && getStructFields(varDecl->type.get()) &&
            varDecl->initializer->kind == ExprKind::Cast)
        {
            auto* cast = static_cast<CastExpr*>(varDecl->initializer.get());
            const IRTypeInfo operandType = getExprType(cast->operand.get());
            // The cast's target IS this struct - `OUT o = (OUT)0` - rather
            // than a struct-typed expression cast to something else; there
            // is no IRType for a struct, so the AST node is what says so.
            const bool castsToThisStruct =
                cast->targetType && varDecl->type &&
                cast->targetType->baseType == BaseType::Struct &&
                cast->targetType->structName == varDecl->type->structName;
            // A DEFENSIVE BACKSTOP.  The unsigned source is refused in
            // semantic analysis, where the type still says UInt - codex
            // measured ScalarKind UInt reaching analyzeCastExpr, correcting
            // my earlier belief that the unsignedness was lost by then.  This
            // check is what catches a source that arrives here typed
            // otherwise; `static const unsigned int U = 2147483648; S s =
            // (S)U;` read +2147483648 where the reference reads
            // -2147483648, and the signedness conversion belongs with the
            // typed evaluator.
            if (castsToThisStruct && operandType.isScalar() &&
                operandType.baseType == IRType::UInt32)
            {
                error(varDecl->loc,
                      "cannot fill a struct from an unsigned value: the "
                      "signed conversion of the leaf is not implemented");
                continue;
            }
            if (castsToThisStruct && operandType.isScalar())
            {
                const IRValueID scalar = buildExpr(cast->operand.get());
                std::unordered_map<int, IRValueID> convertedScalar;

                // One converted scalar per element type, then one splat per
                // leaf width: a struct of four float4 fields costs one
                // conversion, not four.
                auto leafValue = [&](const IRTypeInfo& leafType) -> IRValueID {
                    const IRType elem = leafType.isVector()
                        ? leafType.elementType : leafType.baseType;
                    auto it = convertedScalar.find(static_cast<int>(elem));
                    IRValueID converted;
                    if (it != convertedScalar.end()) {
                        converted = it->second;
                    } else {
                        converted = scalar;
                        IRTypeInfo scalarTarget = leafType;
                        scalarTarget.baseType = elem;
                        scalarTarget.vectorSize = 1;
                        scalarTarget.matrixRows = 0;
                        scalarTarget.matrixCols = 0;
                        const IRType from = operandType.baseType;
                        IROp conv = IROp::Bitcast;
                        if ((from == IRType::Int32 || from == IRType::UInt32 ||
                             from == IRType::Bool) &&
                            (elem == IRType::Float32 || elem == IRType::Float16))
                            conv = IROp::IntToFloat;
                        else if (from == IRType::Float32 && elem == IRType::Float16)
                            conv = IROp::FloatToHalf;
                        else if (from == IRType::Float16 && elem == IRType::Float32)
                            conv = IROp::HalfToFloat;
                        else if ((from == IRType::Float32 || from == IRType::Float16) &&
                                 (elem == IRType::Int32 || elem == IRType::UInt32))
                            conv = IROp::FloatToInt;
                        if (conv != IROp::Bitcast) {
                            if (IRValueID folded = tryFoldUnaryOp(conv, scalarTarget, converted);
                                folded != InvalidIRValue)
                                converted = folded;
                            else
                                converted = emitUnaryOp(conv, scalarTarget, converted);
                        }
                        convertedScalar[static_cast<int>(elem)] = converted;
                    }
                    if (leafType.componentCount() <= 1)
                        return converted;
                    if (IRValueID folded = tryFoldVecConstruct(leafType,
                            std::vector<IRValueID>(
                                static_cast<size_t>(leafType.componentCount()), converted),
                            std::nullopt);
                        folded != InvalidIRValue)
                        return folded;
                    auto splat = std::make_unique<IRInstruction>(IROp::VecConstruct,
                        currentFunction_->allocateValueId(), leafType);
                    for (int i = 0; i < leafType.componentCount(); ++i)
                        splat->addOperand(converted);
                    currentBlock_->addInstruction(std::move(splat));
                    return currentFunction_->nextValueId - 1;
                };

                auto bindLeaves = [&](auto& self, const std::string& prefix,
                                      TypeNode* typeNode) -> void {
                    const auto* leaves = getStructFields(typeNode);
                    if (!leaves) return;
                    for (const auto& field : *leaves) {
                        if (!field.type) continue;
                        const std::string name = prefix + "." + field.name;
                        if (field.type->baseType == BaseType::Struct) {
                            self(self, name, field.type.get());
                            continue;
                        }
                        nameToValue_[name] = leafValue(getIRType(field.type.get()));
                    }
                };
                bindLeaves(bindLeaves, varDecl->name, varDecl->type.get());
                continue;
            }
        }

        // A local ARRAY from a brace list or an array constructor - `float
        // m[9] = {0, ...}`, `float o[5] = float[](0.0, 1.0, ...)` - fills its
        // elements in order (measured: both accepted by the reference).  Each
        // element is built and converted to the element type; a shape that
        // does not match the element refuses by name.
        if (varDecl->initializer && varDecl->initializer->kind == ExprKind::Constructor)
        {
            const IRTypeInfo declaredType = getIRType(varDecl->type.get());
            auto* ctor = static_cast<ConstructorExpr*>(varDecl->initializer.get());
            if (declaredType.isArray() && declaredType.arraySize > 0 && !getStructFields(varDecl->type->elementType.get()))
            {
                IRTypeInfo elementType = declaredType;
                elementType.arraySize = 0;
                // A FLATTENED list fills the elements lane by lane: `float2
                // o[4] = {x0, y0, x1, y1, ...}`, or mixed `{float2(1,2), 3, 4}`
                // (measured: the reference accepts both with warning C1058).
                // Every argument splits into scalar lanes, each converted to
                // the element's scalar type; the lanes regroup into elements.
                const int width = elementType.isVector() ? elementType.vectorSize : 1;
                if (ctor->arguments.size() != static_cast<size_t>(declaredType.arraySize) &&
                    !elementType.isMatrix())
                {
                    IRTypeInfo laneTarget = elementType;
                    laneTarget.baseType = elementType.isVector() ? elementType.elementType : elementType.baseType;
                    laneTarget.elementType = laneTarget.baseType;
                    laneTarget.vectorSize = 1;
                    std::vector<IRValueID> lanes;
                    bool flat = true;
                    for (const auto& arg : ctor->arguments)
                    {
                        const IRTypeInfo argType = getExprType(arg.get());
                        if (argType.isMatrix() || argType.arraySize != 0) { flat = false; break; }
                        const IRValueID v = buildExpr(arg.get());
                        if (v == InvalidIRValue) { flat = false; break; }
                        if (argType.isVector())
                        {
                            IRTypeInfo laneType = argType;
                            laneType.baseType = argType.elementType;
                            laneType.vectorSize = 1;
                            for (int j = 0; j < argType.vectorSize; ++j)
                            {
                                const IRValueID lane = emitInstruction(IROp::VecExtract, laneType,
                                    {v, createConstant(static_cast<int32_t>(j))}, arg->loc);
                                lanes.push_back(emitScalarConversion(laneType, laneTarget, lane, arg->loc));
                            }
                        }
                        else
                            lanes.push_back(emitScalarConversion(argType, laneTarget, v, arg->loc));
                    }
                    if (flat && lanes.size() == static_cast<size_t>(declaredType.arraySize * width))
                    {
                        auto& slots = localArrayValues_[varDecl->name];
                        slots.assign(static_cast<size_t>(declaredType.arraySize), InvalidIRValue);
                        for (int e = 0; e < declaredType.arraySize; ++e)
                        {
                            if (width == 1) { slots[static_cast<size_t>(e)] = lanes[static_cast<size_t>(e)]; continue; }
                            std::vector<IRValueID> parts(lanes.begin() + e * width, lanes.begin() + (e + 1) * width);
                            slots[static_cast<size_t>(e)] = emitInstruction(IROp::VecConstruct, elementType, parts, varDecl->loc);
                        }
                        continue;
                    }
                }
                if (ctor->arguments.size() != static_cast<size_t>(declaredType.arraySize))
                {
                    error(varDecl->loc, "array initialiser for '" + varDecl->name + "' has " +
                          std::to_string(ctor->arguments.size()) + " elements for " +
                          std::to_string(declaredType.arraySize) + " (local-array-initialiser)");
                    continue;
                }
                auto& slots = localArrayValues_[varDecl->name];
                slots.assign(static_cast<size_t>(declaredType.arraySize), InvalidIRValue);
                bool ok = true;
                for (size_t i = 0; i < ctor->arguments.size() && ok; ++i)
                {
                    const IRTypeInfo argType = getExprType(ctor->arguments[i].get());
                    IRValueID v = buildExpr(ctor->arguments[i].get());
                    const bool scalars = !elementType.isVector() && !elementType.isMatrix() &&
                                         !argType.isVector() && !argType.isMatrix() && argType.arraySize == 0;
                    if (scalars)
                        v = emitScalarConversion(argType, elementType, v, ctor->arguments[i]->loc);
                    else if (argType.vectorSize != elementType.vectorSize || argType.isMatrix() ||
                             elementType.isMatrix() || argType.arraySize != 0)
                    {
                        error(ctor->arguments[i]->loc, "array initialiser element " + std::to_string(i) +
                              " of '" + varDecl->name + "' does not match the element type (local-array-initialiser)");
                        ok = false;
                    }
                    slots[i] = v;
                }
                continue;
            }
        }

        if (varDecl->initializer && getStructFields(varDecl->type.get()) &&
            varDecl->initializer->kind == ExprKind::Constructor)
        {
            buildLocalStructInitializer(varDecl->name, varDecl->type.get(), varDecl->initializer.get());
            continue;
        }

        // If there's an initializer, evaluate it
        if (varDecl->initializer)
        {
            IRValueID initValue = buildExpr(varDecl->initializer.get());
            copyArrayAggregate(varDecl->name, varDecl->initializer.get(), varDecl->type.get());
            const IRTypeInfo declaredType = getIRType(varDecl->type.get());
            const IRTypeInfo initType = getExprType(varDecl->initializer.get());
            if (initType.isMatrix() && !declaredType.isMatrix() &&
                (declaredType.isScalar() || declaredType.isVector()))
                initValue = emitMatrixComponents(initType, declaredType, initValue, varDecl->loc);
            else if (declaredType.isMatrix() && declaredType.elementType == IRType::Float32 &&
                initType.isScalar() && initType.arraySize == 0)
                initValue = emitScalarMatrixBroadcast(initType, declaredType, initValue, varDecl->loc);
            else if (declaredType.arraySize == 0 && !declaredType.isMatrix() &&
                initType.arraySize == 0 && !initType.isMatrix() &&
                (declaredType.isVector() ? declaredType.elementType : declaredType.baseType) == IRType::Bool)
            {
                if (initType.isVector() && initType.componentCount() > declaredType.componentCount())
                    initValue = emitVectorNarrowing(initType, declaredType, initValue, varDecl->loc);
                else
                    initValue = emitNumericToBool(initType, declaredType, initValue, varDecl->loc);
            }
            else if (initValue != InvalidIRValue && declaredType.arraySize == 0 && declaredType.isVector() &&
                     initType.arraySize == 0 && initType.isScalar())
            {
                // A scalar initializer broadcasts to the declared vector just
                // like its explicit constructor. Build the initializer once;
                // all lanes share that value, including side effects.
                const IRValueID folded = tryFoldVecConstruct(
                    declaredType, {initValue}, varDecl->type->baseType);
                initValue = folded != InvalidIRValue ? folded :
                    emitInstruction(IROp::VecConstruct, declaredType, {initValue}, varDecl->loc);
            }
            else if (declaredType.arraySize == 0 && !declaredType.isMatrix() && declaredType.isVector() &&
                     initType.isVector() && initType.arraySize == 0 && !initType.isMatrix() &&
                     initType.componentCount() > declaredType.componentCount())
            {
                // float2 a = t.zyx keeps the leading lanes (t.zy), as an
                // assignment already does (measured).
                initValue = emitVectorNarrowing(initType, declaredType, initValue, varDecl->loc);
            }
            else if (declaredType.arraySize == 0 && !declaredType.isMatrix() && !declaredType.isVector() &&
                     declaredType.baseType != IRType::Void && getStructFields(varDecl->type.get()) == nullptr &&
                     initType.isVector() && initType.arraySize == 0 && !initType.isMatrix())
            {
                initValue = emitScalarNarrowing(initType, declaredType, initValue, varDecl->loc);
            }
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
                        if (field.type && getIRType(field.type.get()).isArray())
                        {
                            const auto type = getIRType(field.type.get());
                            localArrayValues_[fieldName].assign(
                                static_cast<size_t>(type.arraySize), InvalidIRValue);
                        }
                        else if (field.type && field.type->vectorSize > 1)
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

// A file-scope initialiser recorded on a global, as a typed IRConstant in
// the current function: a const's every read, and a mutable static's value
// at entry.  InvalidIRValue when there is nothing to build.
IRValueID IRBuilder::materialiseInitialiser(const IRTypeInfo& type,
                                            const std::vector<float>& values,
                                            const std::vector<int64_t>& intValues)
{
    if (intValues.size() == 1)
    {
        const int64_t raw = intValues[0];
        switch (type.baseType)
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
    if (values.empty()) return InvalidIRValue;
    if (values.size() == 1)
    {
        const float raw = values[0];
        switch (type.baseType)
        {
        case IRType::Bool:
            return createConstant(raw != 0.0f);
        case IRType::Int32:
            return createConstant(static_cast<int32_t>(raw));
        case IRType::UInt32:
            return createConstant(static_cast<uint32_t>(raw));
        default:
            return createConstant(type, raw);
        }
    }
    return createConstant(type, values, intValues);
}

// Element `index` of a static const file-scope array, folded from the
// initialiser recorded on the global; InvalidIRValue when the initialiser
// does not cover it.
IRValueID IRBuilder::foldStaticConstArrayElement(const IRGlobal& global, int index)
{
    IRTypeInfo element = global.type;
    element.arraySize = 0;
    const size_t width = static_cast<size_t>(element.componentCount());
    const size_t at = static_cast<size_t>(index) * width;
    std::vector<float> lanes;
    std::vector<int64_t> intLanes;
    if (at + width <= global.initialValue.size())
        lanes.assign(global.initialValue.begin() + at, global.initialValue.begin() + at + width);
    if (at + width <= global.initialIntValues.size())
        intLanes.assign(global.initialIntValues.begin() + at, global.initialIntValues.begin() + at + width);
    if (lanes.empty() && intLanes.empty()) return InvalidIRValue;
    return materialiseInitialiser(element, lanes, intLanes);
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
            const IRValueID folded = materialiseInitialiser(global->type, global->initialValue,
                                                            global->initialIntValues);
            if (folded != InvalidIRValue) return folded;
        }

        // Emit load from global
        auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
            currentFunction_->allocateValueId(), global->type);
        inst->targetName = global->name;  // Use targetName for uniform name
        inst->uniformSource = global->valueId;
        currentBlock_->addInstruction(std::move(inst));
        return currentFunction_->nextValueId - 1;
    }

    if (flattenedUniformStructParams_.count(expr->name))
    {
        error(expr->loc, "uniform struct entry parameter '" + expr->name +
              "' is flattened into its members; using it as a whole value (a call "
              "argument, a return) is not supported (uniform-struct-entry-parameter)");
        return InvalidIRValue;
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

    // A CONSTANT DIVISION BY ZERO IS NOT ONE CASE BUT THREE, and the
    // reference distinguishes them (zero-division-constant-fold, measured on sce-cgc 475 by
    // reading every instruction and every const block of its container):
    //
    //   every denominator non-zero            fold per lane, like any op
    //   every denominator zero AND every       fold each lane to its
    //     numerator zero                       NUMERATOR, sign preserved
    //   anything else                          DO NOT FOLD - the reference
    //                                          emits a runtime reciprocal
    //
    // 0/0 folding to the NUMERATOR rather than to +0 is measured, not
    // assumed: -0.0/0.0 gives -0.0 and 0.0/-0.0 gives +0.0.  (At vector
    // width the +0 and -0 lanes are then merged by the const-block packing
    // rule, since they compare equal - that is literal-vector-dedup-swizzle's job, not this
    // one's.)
    //
    // The third case is why this cannot be "drop the guard": the reference
    // leaves x/0 as a runtime RCP, and it leaves a MIXED denominator vector
    // alone even when every zero-denominator lane has a zero numerator -
    // float4(0,0,2,3) / float4(0,0,2,1) is emitted as RCP/MUL, not folded.
    if (op == IROp::Div)
    {
        bool everyDenominatorZero = true;
        bool everyZeroDenominatorHasZeroNumerator = true;
        bool anyDenominatorZero = false;
        for (size_t i = 0; i < n; ++i)
        {
            if (b[i] == 0.0f)
            {
                anyDenominatorZero = true;
                if (a[i] != 0.0f) everyZeroDenominatorHasZeroNumerator = false;
            }
            else
            {
                everyDenominatorZero = false;
            }
        }
        if (anyDenominatorZero &&
            !(everyDenominatorZero && everyZeroDenominatorHasZeroNumerator))
        {
            return InvalidIRValue;
        }
    }

    std::vector<float> r(n, 0.0f);
    for (size_t i = 0; i < n; ++i)
    {
        switch (op)
        {
        case IROp::Add: r[i] = a[i] + b[i]; break;
        case IROp::Sub: r[i] = a[i] - b[i]; break;
        case IROp::Mul: r[i] = a[i] * b[i]; break;
        case IROp::Div:
            // Reaching here with a zero denominator means the whole
            // expression is 0/0 in every lane, checked above; the result is
            // the numerator itself so that -0 survives as -0.
            if (b[i] == 0.0f) { r[i] = a[i]; break; }
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
        broadcastFirst(rawComponents, static_cast<size_t>(resultType.vectorSize));
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
    // `a, b`: evaluate a for its effects, then the value is b.
    if (expr->op == BinaryOp::Comma)
    {
        buildExpr(expr->left.get());
        return buildExpr(expr->right.get());
    }

    // Handle assignment specially
    if (TypeOperations::isAssignmentOp(expr->op))
    {
        if (expr->op == BinaryOp::Assign && expr->right->kind == ExprKind::Constructor &&
            getStructFields(expr->left->resolvedType.get()))
        {
            // A constructor supplies leaf bindings, not a whole-struct SSA value.
            // Collect its complete RHS before replacing any destination leaf.
            std::string destination;
            ExprNode* root = expr->left.get();
            while (root && root->kind == ExprKind::MemberAccess)
                root = static_cast<MemberAccessExpr*>(root)->object.get();
            auto* identifier = root && root->kind == ExprKind::Identifier
                ? static_cast<IdentifierExpr*>(root) : nullptr;
            const DeclNode* declaration = identifier ? identifier->resolvedDecl : nullptr;
            bool supportedDestination = declaration && declaration->kind == DeclKind::Variable &&
                                        !globalDeclarations_.count(declaration);
            if (declaration && declaration->kind == DeclKind::Parameter &&
                currentFunction_->isEntryPoint && currentFunctionDecl_ && inlineStack_.empty())
            {
                const auto* parameter = static_cast<const ParamDecl*>(declaration);
                supportedDestination = (parameter->storage == StorageQualifier::Out ||
                                        parameter->storage == StorageQualifier::InOut) &&
                    std::any_of(currentFunctionDecl_->parameters.begin(), currentFunctionDecl_->parameters.end(),
                        [&](const auto& own) { return own.get() == parameter; });
            }
            if (module_->shaderStage != ShaderStage::Vertex || expr != statementExpr_ ||
                !supportedDestination || !arrayStorageKey(expr->left.get(), destination))
            {
                error(expr->loc, "struct constructor assignment requires a named local or entry output "
                                 "and a standalone vertex statement (struct-constructor-assignment)");
                return InvalidIRValue;
            }
            if (buildLocalStructInitializer(destination, expr->left->resolvedType.get(), expr->right.get(), true))
                emitEntryStructOutputs(expr->left.get());
            return InvalidIRValue;
        }
        // `G = IN` where IN is a flattened uniform struct entry parameter:
        // IN has no whole-struct value, so copy its member bindings onto
        // G's member keys (libretro: `IN_global = IN;` and helpers that
        // read IN_global.texture_size).  A chain `H = (G = IN)` copies to
        // every destination.  The source members are SNAPSHOT before any
        // destination is cleared, so `IN = IN` (or an overlapping path)
        // cannot erase what it copies (review: codex); each destination's
        // old members are dropped so an absent source member cannot leave
        // an older value behind.
        if (expr->op == BinaryOp::Assign)
        {
            std::vector<ExprNode*> destinations{expr->left.get()};
            ExprNode* source = expr->right.get();
            while (source && source->kind == ExprKind::Binary &&
                   static_cast<BinaryExpr*>(source)->op == BinaryOp::Assign)
            {
                destinations.push_back(static_cast<BinaryExpr*>(source)->left.get());
                source = static_cast<BinaryExpr*>(source)->right.get();
            }
            if (source && source->kind == ExprKind::Identifier &&
                flattenedUniformStructParams_.count(static_cast<IdentifierExpr*>(source)->name) &&
                !nameToValue_.count(static_cast<IdentifierExpr*>(source)->name))
            {
                const std::string from = static_cast<IdentifierExpr*>(source)->name;
                // The copy produces no value: used as one (f(G = IN), (G = IN).a)
                // it would read nothing, so it refuses by name instead.
                if (expr != statementExpr_)
                {
                    error(expr->loc, "a copy of uniform struct entry parameter '" + from +
                          "' is only supported as a statement, not used as a value "
                          "(uniform-struct-entry-parameter)");
                    return InvalidIRValue;
                }
                std::vector<std::pair<std::string, IRValueID>> members;
                for (const auto& entry : nameToValue_)
                    if (entry.first.compare(0, from.size() + 1, from + ".") == 0)
                        members.emplace_back(entry.first.substr(from.size()), entry.second);
                for (ExprNode* destination : destinations)
                {
                    std::string to;
                    if (!arrayStorageKey(destination, to))
                    {
                        error(expr->loc, "a uniform struct entry parameter can only be copied into a "
                                         "named struct variable (uniform-struct-entry-parameter)");
                        return InvalidIRValue;
                    }
                    for (auto it = nameToValue_.begin(); it != nameToValue_.end(); )
                        if (it->first.compare(0, to.size() + 1, to + ".") == 0) it = nameToValue_.erase(it);
                        else ++it;
                    for (const auto& member : members) nameToValue_[to + member.first] = member.second;
                }
                return InvalidIRValue;
            }
        }
        IRValueID rhsValue = buildExpr(expr->right.get());

        if (expr->op == BinaryOp::Assign)
        {
            std::string destination;
            if (arrayStorageKey(expr->left.get(), destination))
                copyArrayAggregate(destination, expr->right.get(), expr->left->resolvedType.get());
        }

        // For compound assignments, compute the new value
        if (expr->op != BinaryOp::Assign)
        {
            // Resolve an array lvalue once. Reading and then rebuilding its
            // selector for the write would execute a[k++] twice.
            std::string arrayKey;
            int32_t arrayIndex = 0;
            const bool arrayTarget = expr->left->kind == ExprKind::Index &&
                getExprType(static_cast<IndexExpr*>(expr->left.get())->array.get()).isArray();
            if (arrayTarget && !resolveTrackedArrayElement(
                    static_cast<IndexExpr*>(expr->left.get()), arrayKey, arrayIndex))
                return InvalidIRValue;
            IRValueID lhsValue = arrayTarget
                ? readTrackedArrayElement(static_cast<IndexExpr*>(expr->left.get()), arrayKey, arrayIndex)
                : buildExpr(expr->left.get());
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
            // a op= v with a scalar a and a vector v is a op v.x, computed in
            // the COMMON type of a and v's elements (float over half over int)
            // and only then converted to a's (measured: int a = 3;
            // a *= float2(1.5, 9) is 4; float a = .5; a *= int2(3, 9) is 1.5;
            // float a = 1.000244140625; a *= half2(1, 2) keeps the float a;
            // half a *= float2 multiplies in float - review: codex).
            bool narrowedCompound = false;
            {
                const IRTypeInfo leftType = getExprType(expr->left.get());
                const IRTypeInfo rightType = getExprType(expr->right.get());
                if (!leftType.isVector() && !leftType.isMatrix() && leftType.arraySize == 0 &&
                    rightType.isVector() && !rightType.isMatrix() && rightType.arraySize == 0 &&
                    leftType.baseType != IRType::Bool)
                {
                    IRTypeInfo laneType = leftType;
                    laneType.baseType = rightType.elementType;
                    const IRValueID lane = emitInstruction(IROp::VecExtract, laneType,
                        {rhsValue, createConstant(static_cast<int32_t>(0))}, expr->loc);
                    if (laneType.baseType == leftType.baseType)
                    {
                        rhsValue = emitBinaryOp(op, leftType, lhsValue, lane);
                    }
                    else
                    {
                        // Raise both to the common type, operate there, convert to a's.
                        const auto rank = [](IRType t) {
                            return t == IRType::Float32 ? 2 : t == IRType::Float16 ? 1 : 0;
                        };
                        const IRTypeInfo& common =
                            rank(laneType.baseType) >= rank(leftType.baseType) ? laneType : leftType;
                        const IRValueID wideLhs = emitScalarConversion(leftType, common, lhsValue, expr->loc);
                        const IRValueID wideLane = emitScalarConversion(laneType, common, lane, expr->loc);
                        const IRValueID wide = emitBinaryOp(op, common, wideLhs, wideLane);
                        rhsValue = emitScalarConversion(common, leftType, wide, expr->loc);
                    }
                    narrowedCompound = true;
                }
            }
            const IRTypeInfo compoundType = getExprType(expr->left.get());
            const IRTypeInfo rhsType = getExprType(expr->right.get());
            if (compoundType.isMatrix() && !compoundType.isArray() &&
                compoundType.elementType == IRType::Float32 && rhsType.isScalar() &&
                (op == IROp::Add || op == IROp::Sub || op == IROp::Mul || op == IROp::Div))
            {
                rhsValue = prepareFloatMatrixScalar(rhsType, rhsValue, expr->loc);
                if (rhsValue == InvalidIRValue) return rhsValue;
            }
            if (!narrowedCompound)
                rhsValue = emitBinaryOp(op, getExprType(expr->left.get()), lhsValue, rhsValue);
            if (arrayTarget)
            {
                rhsValue = coerceAssignmentValue(expr->left.get(), rhsValue);
                localArrayValues_.at(arrayKey)[static_cast<size_t>(arrayIndex)] = rhsValue;
                return rhsValue;
            }
        }

        if (expr->op == BinaryOp::Assign)
        {
            const IRTypeInfo targetType = getExprType(expr->left.get());
            const IRTypeInfo sourceType = getExprType(expr->right.get());
            if (targetType.isMatrix() && targetType.elementType == IRType::Float32 && sourceType.isScalar())
            {
                // Preserve the RHS declared type: inferring it from the
                // current binding alone can hide an untruncated int local.
                rhsValue = emitScalarMatrixBroadcast(sourceType, targetType, rhsValue, expr->loc);
                if (rhsValue == InvalidIRValue) return rhsValue;
            }
        }
        return buildAssignment(expr->left.get(), rhsValue);
    }

    // Regular binary expression
    const bool logical = expr->op == BinaryOp::LogicalAnd || expr->op == BinaryOp::LogicalOr;
    IRValueID leftValue = buildExpr(expr->left.get());
    if (logical)
    {
        leftValue = normalizeCondition(expr->left.get(), leftValue);
        ++shortCircuitRhsDepth_;
    }
    IRValueID rightValue = buildExpr(expr->right.get());
    if (logical)
    {
        // The comparison belongs to the RHS as well. Preserve its existing
        // short-circuit provenance and never evaluate either expression twice.
        rightValue = normalizeCondition(expr->right.get(), rightValue);
        --shortCircuitRhsDepth_;
    }

    IROp op = binaryOpToIROp(expr->op);
    IRTypeInfo resultType = getExprType(expr);

    if (resultType.isMatrix() && !resultType.isArray() &&
        resultType.elementType == IRType::Float32 &&
        (op == IROp::Add || op == IROp::Sub || op == IROp::Mul || op == IROp::Div))
    {
        const IRTypeInfo leftType = getExprType(expr->left.get());
        const IRTypeInfo rightType = getExprType(expr->right.get());
        if (leftType.isScalar())
            leftValue = prepareFloatMatrixScalar(leftType, leftValue, expr->loc);
        if (rightType.isScalar())
            rightValue = prepareFloatMatrixScalar(rightType, rightValue, expr->loc);
        if (leftValue == InvalidIRValue || rightValue == InvalidIRValue)
            return InvalidIRValue;
    }

    if (IRValueID folded = tryFoldBinaryOp(op, resultType, leftValue, rightValue);
        folded != InvalidIRValue)
    {
        return folded;
    }

    return emitBinaryOp(op, resultType, leftValue, rightValue, expr->loc);
}

IRValueID IRBuilder::buildUnaryExpr(UnaryExpr* expr)
{
    const bool increments = expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PreDecrement ||
        expr->op == UnaryOp::PostIncrement || expr->op == UnaryOp::PostDecrement;
    const bool arrayOperand = increments && expr->operand->kind == ExprKind::Index &&
        getExprType(static_cast<IndexExpr*>(expr->operand.get())->array.get()).isArray();
    std::string arrayKey;
    int32_t arrayIndex = 0;
    if (arrayOperand && !resolveTrackedArrayElement(
            static_cast<IndexExpr*>(expr->operand.get()), arrayKey, arrayIndex))
        return InvalidIRValue;
    IRValueID operandValue = arrayOperand
        ? readTrackedArrayElement(static_cast<IndexExpr*>(expr->operand.get()), arrayKey, arrayIndex)
        : buildExpr(expr->operand.get());
    if (expr->op == UnaryOp::LogicalNot)
        operandValue = normalizeCondition(expr->operand.get(), operandValue);

    // Handle increment/decrement specially
    if (expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PreDecrement ||
        expr->op == UnaryOp::PostIncrement || expr->op == UnaryOp::PostDecrement)
    {
        IRTypeInfo type = getExprType(expr->operand.get());
        IRValueID one = type.baseType == IRType::Int32 ? createConstant(int32_t(1)) :
            type.baseType == IRType::UInt32 ? createConstant(uint32_t(1)) : createConstant(1.0f);

        IROp op = (expr->op == UnaryOp::PreIncrement || expr->op == UnaryOp::PostIncrement)
            ? IROp::Add : IROp::Sub;

        IRValueID newValue = tryFoldBinaryOp(op, type, operandValue, one);
        if (newValue == InvalidIRValue)
            newValue = emitBinaryOp(op, type, operandValue, one);

        // Store back
        if (arrayOperand)
            localArrayValues_.at(arrayKey)[static_cast<size_t>(arrayIndex)] =
                coerceAssignmentValue(expr->operand.get(), newValue);
        else
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
    // modf(x, out ip), measured on the reference (FLR, then a predicated
    // correction on x < 0): fl = floor(x); f = x - fl; ip = fl - and where
    // x < 0, ip = fl + 1 and f = f - 1.  That is the usual split for every
    // negative non-integer, and the reference's own result at a negative
    // integer: modf(-2) is f = -1, ip = -1.  The second argument is a
    // destination, never read (the semantic pass checked it is an lvalue of
    // x's type).
    if (expr->functionName == "modf" && expr->resolvedFunction == nullptr &&
        expr->arguments.size() == 2)
    {
        const IRTypeInfo resultType = getExprType(expr);
        const IRValueID x = buildExpr(expr->arguments[0].get());
        const IRValueID fl = emitInstruction(IROp::Floor, resultType, {x}, expr->loc);
        const IRValueID fr = emitBinaryOp(IROp::Sub, resultType, x, fl, expr->loc);
        // createConstant(type, scalar) fills lane x only; build every lane.
        const size_t lanes = static_cast<size_t>(std::max(1, resultType.vectorSize));
        const IRValueID one = createConstant(resultType, std::vector<float>(lanes, 1.0f));
        IRTypeInfo boolType = resultType;
        if (resultType.isVector()) boolType.elementType = IRType::Bool;
        else boolType = IRTypeInfo::Bool();
        const IRValueID negative = emitBinaryOp(IROp::CmpLt, boolType, x,
                                                createConstant(resultType, std::vector<float>(lanes, 0.0f)), expr->loc);
        const IRValueID ip = emitInstruction(IROp::Select, resultType,
            {negative, emitBinaryOp(IROp::Add, resultType, fl, one, expr->loc), fl}, expr->loc);
        const IRValueID f = emitInstruction(IROp::Select, resultType,
            {negative, emitBinaryOp(IROp::Sub, resultType, fr, one, expr->loc), fr}, expr->loc);
        buildAssignment(expr->arguments[1].get(), ip);
        return f;
    }

    // Check for built-in function
    auto builtinOp = builtinToIROp(expr->functionName);

    // Build arguments.  An argument bound to a user function's `out`
    // parameter is a destination only: it is never read (the caller's
    // `float4 adjx;` is typically uninitialised), so no value is built for
    // it; the inliner assigns the helper's final value back to it.
    const auto* calleeDecl =
        (expr->resolvedFunction && expr->resolvedFunction->kind == DeclKind::Function)
            ? static_cast<const FunctionDecl*>(expr->resolvedFunction) : nullptr;
    std::vector<IRValueID> argValues;
    for (size_t i = 0; i < expr->arguments.size(); ++i)
    {
        if (calleeDecl && i < calleeDecl->parameters.size() &&
            calleeDecl->parameters[i]->storage == StorageQualifier::Out)
        {
            argValues.push_back(InvalidIRValue);
            continue;
        }
        argValues.push_back(buildExpr(expr->arguments[i].get()));
    }

    // AN OMITTED ARGUMENT IS THE DEFAULT EXPRESSION, BUILT HERE AT THE CALL
    // SITE (function-visibility).  Measured against the reference: `shade(t)` where
    // `float k = 0.5` produces a container BYTE-IDENTICAL to `shade(t, 0.5)`
    // written out, so the default is a call-site substitution and not a
    // property of the callee's body.
    //
    // IT IS THE EXPRESSION, NOT A FOLDED CONSTANT.  `uniform float g; float4
    // shade(float4 c, float k = g)` is accepted by the reference and its
    // container carries `g` as a real uniform record - shade(t) is
    // byte-identical to shade(t, g).  So this builds the default the same way
    // it builds any other argument; evaluating it to a constant would be
    // correct on every literal default and wrong on that one.
    //
    // A DEFAULT EXPRESSION BINDS ITS NAMES AT ITS DECLARATION, NOT AT THE CALL.
    // It is materialised here, so the instructions land in the caller's block -
    // but name resolution must not see the caller's locals.  Measured (codex,
    // function-visibility review):
    //
    //     uniform float g = 2;
    //     float4 shade(float4 t, float k = g) { return t * k; }
    //     ... main declares a LOCAL float g = 7, then calls shade(t)
    //
    // the reference reads the GLOBAL 2 - byte-identical to passing the saved
    // global explicitly - and we read the local 7.  A WRONG VALUE, silently.
    //
    // An earlier revision of this comment argued the caller's scope was safe
    // "because a default may not name another parameter of its own function
    // (C1102)".  That reasons about the wrong shadowing source entirely:
    // excluding the callee's PARAMETERS says nothing about the caller's LOCALS,
    // and buildIdentifierExpr consults nameToValue_ before the global lookup.
    //
    // So the default is built under a scope holding file-scope bindings ONLY.
    // A global the caller shadows is restored from the stash that
    // stashShadowedGlobal (inline-entry-parameter-scope) already keeps for exactly this shape -
    // an inlined helper naming a shadowed global reads the global - rather than
    // inventing a second mechanism for the same question.  A stash entry of
    // InvalidIRValue means "shadowed, never assigned", and leaving that name
    // unbound is what makes buildIdentifierExpr fall through to the global
    // load; that is also how a shadowed UNIFORM resolves, since uniforms are
    // deliberately kept out of nameToValue_.
    if (expr->resolvedFunction && expr->resolvedFunction->kind == DeclKind::Function)
    {
        auto* decl = static_cast<FunctionDecl*>(expr->resolvedFunction);
        if (argValues.size() < decl->parameters.size() &&
            decl->parameters[argValues.size()] &&
            decl->parameters[argValues.size()]->defaultValue)
        {
            ScopeState callerScope = scope_;
            scope_.names.clear();
            scope_.arrays.clear();
            scope_.shadowedGlobals.clear();
            scope_.shadowedGlobalArrays.clear();
            for (const auto& g : module_->globals)
            {
                auto stashed = callerScope.shadowedGlobals.find(g.name);
                if (stashed != callerScope.shadowedGlobals.end())
                {
                    if (stashed->second != InvalidIRValue)
                        scope_.names[g.name] = stashed->second;
                }
                else if (auto it = callerScope.names.find(g.name);
                         it != callerScope.names.end())
                {
                    scope_.names[g.name] = it->second;
                }

                auto stashedArr = callerScope.shadowedGlobalArrays.find(g.name);
                if (stashedArr != callerScope.shadowedGlobalArrays.end())
                {
                    if (!stashedArr->second.empty())
                        scope_.arrays[g.name] = stashedArr->second;
                }
                else if (auto ia = callerScope.arrays.find(g.name);
                         ia != callerScope.arrays.end())
                {
                    scope_.arrays[g.name] = ia->second;
                }
            }

            for (size_t i = argValues.size(); i < decl->parameters.size(); ++i)
            {
                const auto& param = decl->parameters[i];
                if (!param || !param->defaultValue) break;   // leave the arity
                                                            // error to the caller
                argValues.push_back(buildExpr(param->defaultValue.get()));
            }

            // RESTORING THE CALLER'S SCOPE MUST NOT UNDO THE DEFAULT'S WRITES.
            // The default is built under a file-scope-only scope so it cannot
            // READ a caller local; but anything it writes to a GLOBAL is a
            // real side effect that the caller can observe on the next line.
            // Dropping it wholesale left `static float G; float bump(){G=2;}
            // ... outer(x, k = bump()) ... return r + G;` reading the old G -
            // the reference makes that form byte-identical to passing bump()
            // explicitly (found by codex; on this base it surfaces as a
            // refusal, "ldunif of 'G' has no registered uniform source",
            // because the binding vanished rather than going stale).
            //
            // So: take the caller's scope back, then carry the global
            // bindings FORWARD from the default's scope - into the caller's
            // stash where the caller shadows that global, and into its names
            // where it does not.
            ScopeState afterDefault = scope_;
            scope_ = callerScope;
            for (const auto& g : module_->globals)
            {
                auto produced = afterDefault.names.find(g.name);
                if (produced == afterDefault.names.end()) continue;
                auto stashed = scope_.shadowedGlobals.find(g.name);
                if (stashed != scope_.shadowedGlobals.end())
                    stashed->second = produced->second;
                else
                    scope_.names[g.name] = produced->second;
            }
            for (const auto& g : module_->globals)
            {
                auto produced = afterDefault.arrays.find(g.name);
                if (produced == afterDefault.arrays.end()) continue;
                auto stashed = scope_.shadowedGlobalArrays.find(g.name);
                if (stashed != scope_.shadowedGlobalArrays.end())
                    stashed->second = produced->second;
                else
                    scope_.arrays[g.name] = produced->second;
            }
        }
    }

    IRTypeInfo resultType = getExprType(expr);

    // any(v) ORs and all(v) ANDs the lanes' truth (a numeric lane is true
    // for either sign of nonzero).  Measured on the reference: all(t.xy)
    // is SNE per lane then a product into the condition register.
    const bool reduceAll = expr->functionName == "all";
    if ((expr->functionName == "any" || reduceAll) && expr->resolvedFunction == nullptr &&
        argValues.size() == 1)
    {
        // Arguments were evaluated once above. Reduce that value, never the
        // argument AST: any(v++) must update v once regardless of its width.
        const IRTypeInfo argumentType = getExprType(expr->arguments[0].get());
        if (argumentType.isArray() ||
            (!argumentType.isScalar() && !argumentType.isVector()))
            return emitCall(expr->functionName, resultType, argValues);
        IRTypeInfo laneType = argumentType;
        if (argumentType.isVector())
        {
            laneType.baseType = argumentType.elementType;
            laneType.vectorSize = 1;
        }
        std::vector<float> constantLanes;
        const bool constantVector = argumentType.isVector() &&
            extractFloatComponents(*currentFunction_, argValues[0], constantLanes) &&
            constantLanes.size() == static_cast<size_t>(argumentType.vectorSize);
        const auto* constant = dynamic_cast<const IRConstant*>(
            currentFunction_->getValue(argValues[0]));
        IRValueID reduced = InvalidIRValue;
        for (int lane = 0; lane < argumentType.vectorSize; ++lane)
        {
            IRValueID value = argValues[0];
            if (constantVector && constant &&
                constant->intValues.size() == constantLanes.size())
            {
                const int64_t raw = constant->intValues[lane];
                switch (laneType.baseType)
                {
                case IRType::Int32: value = createConstant(static_cast<int32_t>(raw)); break;
                case IRType::UInt32: value = createConstant(static_cast<uint32_t>(raw)); break;
                case IRType::Bool: value = createConstant(raw != 0); break;
                default: value = createConstant(laneType, constantLanes[lane]); break;
                }
            }
            else if (constantVector &&
                (laneType.baseType == IRType::Float32 || laneType.baseType == IRType::Float16))
                value = createConstant(laneType, constantLanes[lane]);
            else if (argumentType.isVector())
                value = emitInstruction(IROp::VecExtract, laneType,
                    {value, createConstant(static_cast<int32_t>(lane))}, expr->loc);
            // Bool lanes are already normalized. Numeric lanes are true for
            // either sign of nonzero, not just for positive values.
            if (laneType.baseType != IRType::Bool)
            {
                const IRValueID zero = laneType.baseType == IRType::Int32
                    ? createConstant(int32_t{0})
                    : laneType.baseType == IRType::UInt32
                        ? createConstant(uint32_t{0})
                        : createConstant(laneType, 0.0f);
                value = emitBinaryOp(IROp::CmpNe, IRTypeInfo::Bool(), value, zero, expr->loc);
            }
            reduced = reduced == InvalidIRValue ? value
                : emitBinaryOp(reduceAll ? IROp::LogicalAnd : IROp::LogicalOr,
                               IRTypeInfo::Bool(), reduced, value, expr->loc);
        }
        return reduced;
    }

    // fmod(a, b): the remainder with the sign of a.  The reference lowers it
    // as r = frac(|a / b|) * |b|, then -r where a < 0 (measured, both
    // profiles: RCP+MUL or DIV, FRC |q|, MUL by |b|, a predicated negate on
    // a's sign).  Expanded here into those operations, lane by lane through
    // the ordinary vector paths.  Operands of differing width keep the call
    // (refused by name downstream) rather than guessing a promotion.
    if (expr->functionName == "fmod" && expr->resolvedFunction == nullptr &&
        argValues.size() == 2)
    {
        const IRTypeInfo ta = getExprType(expr->arguments[0].get());
        const IRTypeInfo tb = getExprType(expr->arguments[1].get());
        const bool floating = (resultType.isVector() ? resultType.elementType : resultType.baseType) == IRType::Float32 ||
                              (resultType.isVector() ? resultType.elementType : resultType.baseType) == IRType::Float16;
        // A scalar operand broadcasts (measured: fmod(a, b.x) and fmod(a.x, b)
        // are accepted); two vectors of different widths keep the call.
        const auto fits = [&](const IRTypeInfo& t) {
            return t.vectorSize == resultType.vectorSize || (t.vectorSize == 1 && !t.isMatrix());
        };
        if (floating && !resultType.isMatrix() && !resultType.isArray() && fits(ta) && fits(tb))
        {
            const auto splat = [&](IRValueID v, const IRTypeInfo& t) {
                if (!resultType.isVector() || t.vectorSize == resultType.vectorSize) return v;
                std::vector<IRValueID> lanes(static_cast<size_t>(resultType.vectorSize), v);
                return emitInstruction(IROp::VecConstruct, resultType, lanes, expr->loc);
            };
            const IRValueID a = splat(argValues[0], ta), b = splat(argValues[1], tb);
            const IRValueID q = emitBinaryOp(IROp::Div, resultType, a, b, expr->loc);
            const IRValueID aq = emitInstruction(IROp::Abs, resultType, {q}, expr->loc);
            const IRValueID f = emitInstruction(IROp::Frac, resultType, {aq}, expr->loc);
            const IRValueID ab = emitInstruction(IROp::Abs, resultType, {b}, expr->loc);
            const IRValueID r = emitBinaryOp(IROp::Mul, resultType, f, ab, expr->loc);
            const IRValueID nr = emitInstruction(IROp::Neg, resultType, {r}, expr->loc);
            IRTypeInfo boolType = resultType;
            if (resultType.isVector()) boolType.elementType = IRType::Bool;
            else boolType = IRTypeInfo::Bool();
            const IRValueID negative = emitBinaryOp(IROp::CmpLt, boolType, a,
                                                    createConstant(resultType, 0.0f), expr->loc);
            return emitInstruction(IROp::Select, resultType, {negative, nr, r}, expr->loc);
        }
    }

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
            // packed-depth-decode: these intrinsics decode packed RGB depth. Keep
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
                    "' conflicts with a source declaration; refusing ambiguous binding (packed-depth-decode)");
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
            if (const auto* global = module_->findGlobal(factorName))
                load->uniformSource = global->valueId;
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

        // VP literal math has profile-specific rounding. Keep this hook confined
        // to the four measured operations; other builtins and FP are unchanged.
        const bool trig = *builtinOp == IROp::Sin || *builtinOp == IROp::Cos ||
                          *builtinOp == IROp::Tan;
        if (module_->shaderStage == ShaderStage::Vertex && argValues.size() == 1 &&
            (trig || *builtinOp == IROp::Sqrt) &&
            (resultType.isScalar() || resultType.isVector()))
        {
            std::vector<float> values;
            bool constant = extractFloatComponents(*currentFunction_, argValues[0], values);
            if (!constant)
            {
                // Overload resolution permits numeric arguments without an IR
                // cast. Integer/bool literals must use the same fold and bound.
                uint32_t raw = 0;
                bool isUnsigned = false;
                if (extractIntegerScalar(*currentFunction_, argValues[0], raw, isUnsigned))
                {
                    values = {isUnsigned ? static_cast<float>(raw)
                                         : static_cast<float>(static_cast<int32_t>(raw))};
                    constant = true;
                }
            }
            if (constant && !values.empty())
            {
                for (float value : values)
                {
                    if (!std::isfinite(value) || (trig && std::fabs(value) > 65536.0f))
                    {
                        error(expr->loc, "our VP constant trig reduction bound is finite |x| <= 65536; "
                            "nonfinite constant math is also unsupported (constant-math-range)");
                        return InvalidIRValue;
                    }
                }
                std::vector<float> folded;
                std::vector<IRValueID> lanes;
                const IRType element = resultType.isVector() ? resultType.elementType : resultType.baseType;
                const IRTypeInfo scalarType = element == IRType::Float16 ? IRTypeInfo::Half() : IRTypeInfo::Float();
                bool allConstant = true;
                for (float value : values)
                {
                    if (*builtinOp == IROp::Sqrt && value <= 0.0f)
                    {
                        // Reference VP leaves these lanes as RSQ + RCP, even
                        // when adjacent positive lanes fold to constants.
                        allConstant = false;
                        folded.push_back(0.0f);
                        lanes.push_back(emitInstruction(IROp::Sqrt, scalarType,
                            {createConstant(scalarType, value)}));
                        continue;
                    }
                    float result = 0.0f;
                    const double input = static_cast<double>(value);
                    if (*builtinOp == IROp::Sqrt)
                    {
                        const float reciprocalSqrt = static_cast<float>(1.0 / std::sqrt(input));
                        result = static_cast<float>(1.0 / static_cast<double>(reciprocalSqrt));
                    }
                    else if (*builtinOp == IROp::Sin)
                        result = static_cast<float>(std::sin(input));
                    else if (*builtinOp == IROp::Cos)
                        result = static_cast<float>(std::cos(input));
                    else
                    {
                        const float sine = static_cast<float>(std::sin(input));
                        const float cosine = static_cast<float>(std::cos(input));
                        result = static_cast<float>(static_cast<double>(sine) / static_cast<double>(cosine));
                    }
                    folded.push_back(result);
                    lanes.push_back(createConstant(scalarType, result));
                }
                if (allConstant)
                    return folded.size() == 1 ? createConstant(resultType, folded[0]) : createConstant(resultType, folded);
                return lanes.size() == 1 ? lanes[0] : emitInstruction(IROp::VecConstruct, resultType, lanes);
            }
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


namespace { bool arrayStorageKey(ExprNode* expr, std::string& key); }

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
    // out / inout parameters are copy-out: after the body the parameter's
    // final value is assigned to the caller's argument (Cg copy-in/copy-out).
    // When two arguments name the same variable the LEFTMOST parameter's
    // value is the one that remains (measured: two(x, x) with a = 1, b = 2
    // leaves x = 1), so the copies are made right to left.  Supported for a scalar/vector/matrix parameter whose
    // argument is a plain caller lvalue; anything else is refused by name.
    std::vector<size_t> copyOut;
    for (size_t i = 0; i < callee->parameters.size(); ++i)
    {
        const ParamDecl* param = callee->parameters[i].get();
        if (param->storage != StorageQualifier::Out && param->storage != StorageQualifier::InOut)
            continue;
        const IRTypeInfo pt = getIRType(param->type.get());
        const ExprNode* arg = i < expr->arguments.size() ? expr->arguments[i].get() : nullptr;
        if (arg && getExprType(expr->arguments[i].get()).isMatrix() && !pt.isMatrix())
        {
            error(arg->loc, "matrix component conversion for out/inout parameters is not supported");
            return false;
        }
        const bool lvalue = arg && (arg->kind == ExprKind::Identifier ||
                                    (arg->kind == ExprKind::MemberAccess &&
                                     static_cast<const MemberAccessExpr*>(arg)->object &&
                                     static_cast<const MemberAccessExpr*>(arg)->object->kind == ExprKind::Identifier));
        if (pt.isArray() || getStructFields(param->type.get()) || param->name.empty() || !lvalue)
        {
            error(expr->loc, "cannot inline user function '" + name +
                             "': out/inout parameter '" + param->name +
                             "' needs a scalar, vector or matrix type and a plain variable argument");
            return false;
        }
        copyOut.push_back(i);
    }

    auto savedDecls = declToValue_;
    const ScopeState savedScope = scope_;   // the whole per-scope state, once
    const auto& savedNames = savedScope.names;
    const auto& savedArrays = savedScope.arrays;
    auto savedSwizzles = identityPrefixSwizzleBase_;

    // calleeDirect is the parameters plus the body's own top-level
    // declarations actually reached: a name declared inside a nested block ends with
    // that block (buildBlockStmt), so a write to the FILE-SCOPE name after
    // the block is the global's and must reach the caller - a walked set
    // used to mark it callee-scoped and drop it (`{ float4 G = q*3; }
    // G = q*2;` left the caller reading the uniform; the reference reads 2q).
    std::unordered_set<std::string> calleeDirect;
    for (const auto& param : callee->parameters)
        if (!param->name.empty()) calleeDirect.insert(param->name);
    // Locals are added from the executed body's declaration frame below.
    // A declaration after return never shadows an earlier global write.
    // Ownership, not equality: a callee that binds `s` owns "s.f" too (review:
    // codex - the callee's own struct field write must not be routed to the
    // caller's stash as a write to the global struct of the same name).
    auto calleeOwns = [&](const std::string& key) -> bool
    {
        for (const auto& n : calleeDirect)
            if (key == n || (key.size() > n.size() && key[n.size()] == '.' && key.compare(0, n.size(), n) == 0))
                return true;
        return false;
    };

    for (size_t i = 0; i < callee->parameters.size(); ++i)
    {
        ParamDecl* param = callee->parameters[i].get();
        IRValueID boundValue = args[i];
        ExprNode* argument = i < expr->arguments.size() ? expr->arguments[i].get() : param->defaultValue.get();
        if (argument)
        {
            const IRTypeInfo sourceType = getExprType(argument);
            const IRTypeInfo targetType = getIRType(param->type.get());
            if (sourceType.isMatrix() && !targetType.isMatrix() &&
                (targetType.isScalar() || targetType.isVector()))
            {
                boundValue = emitMatrixComponents(sourceType, targetType, boundValue, argument->loc);
            }
        }
        declToValue_[param] = boundValue;
        // A parameter that shadows a file-scope name moves the global's
        // binding into the stash, exactly as the entry function's does
        // (inline-entry-parameter-scope): a helper called from this body then reads the
        // global, and its write to the global lands in the stash and
        // comes back out below (inline-global-stash - this used to be REFUSED).
        stashShadowedGlobal(param->name);
        if (!param->name.empty())
            nameToValue_[param->name] = boundValue;

        // Array-bearing arguments are values, not caller spelling aliases.
        // Take the call's completed argument state (the reference's measured
        // g(s, s.a[0]=...) cell sees that write), then rename its qualified
        // fields into this parameter's scope. savedScope also prevents an
        // earlier parameter binding from corrupting a later argument.
        auto arrayFields = [&](auto& self, TypeNode* type, const std::string& key,
                               std::vector<std::pair<std::string, int>>& fields) -> void {
            const auto irType = getIRType(type);
            if (irType.isArray()) { fields.emplace_back(key, irType.arraySize); return; }
            if (const auto* members = getStructFields(type))
                for (const auto& member : *members)
                    if (member.type) self(self, member.type.get(), key + "." + member.name, fields);
        };
        std::vector<std::pair<std::string, int>> fields;
        arrayFields(arrayFields, param->type.get(), param->name, fields);
        // Preserve the existing entry-uniform array alias instead of
        // intercepting it with an all-uninitialised local element map.
        const bool uniformArrayAlias = getIRType(param->type.get()).isArray() &&
            std::any_of(currentFunction_->parameters.begin(), currentFunction_->parameters.end(),
                [&](const IRParameter& p) { return p.valueId == args[i] && p.type.isArray() &&
                    p.storage == StorageQualifier::Uniform; });
        if (uniformArrayAlias)
        {
            // The caller can have a local array with this parameter's name.
            // Its snapshot is restored after the call; it must not intercept
            // reads through the uniform parameter while the helper executes.
            localArrayValues_.erase(param->name);
            fields.clear();
        }
        if (!fields.empty())
        {
            // Entry struct inputs carry semantic/uniform provenance in their
            // original parameter value, not in local element storage. An
            // invented all-undefined array map hides that existing load path.
            // Still overlay any real tracked fields from the completed call
            // state below; local aggregates keep their normal undefined rows.
            const bool entryStructAlias = getStructFields(param->type.get()) &&
                std::any_of(currentFunction_->parameters.begin(), currentFunction_->parameters.end(),
                    [&](const IRParameter& p) { return p.valueId == args[i]; });
            const std::string parameterPrefix = param->name + ".";
            for (auto it = nameToValue_.begin(); it != nameToValue_.end(); )
                if (it->first.compare(0, parameterPrefix.size(), parameterPrefix) == 0)
                    it = nameToValue_.erase(it);
                else ++it;
            for (const auto& field : fields)
                if (entryStructAlias) localArrayValues_.erase(field.first);
                else localArrayValues_[field.first].assign(static_cast<size_t>(field.second), InvalidIRValue);

            std::string source;
            if (i < expr->arguments.size() && arrayStorageKey(expr->arguments[i].get(), source))
            {
                const std::string prefix = source + ".";
                auto belongs = [&](const std::string& key) {
                    return key == source || key.compare(0, prefix.size(), prefix) == 0;
                };
                for (const auto& entry : savedArrays)
                    if (belongs(entry.first))
                        localArrayValues_[param->name + entry.first.substr(source.size())] = entry.second;
                for (const auto& entry : savedNames)
                    if (entry.first != source && belongs(entry.first))
                        nameToValue_[param->name + entry.first.substr(source.size())] = entry.second;
            }

            // A static const file-scope array passed whole has no tracked
            // elements to copy - its value is its initialiser - so the
            // parameter's elements are its folded elements (inferred-array-
            // to-sized-param: `g(cx)` with `float g(float v[3])` read v[2]
            // as undefined and emit refused).  Only when no caller binding
            // of the name (a local array) shadows the global.
            if (i < expr->arguments.size() && expr->arguments[i] &&
                expr->arguments[i]->kind == ExprKind::Identifier)
            {
                const std::string& name = static_cast<IdentifierExpr*>(expr->arguments[i].get())->name;
                IRGlobal* g = savedArrays.count(name) || savedNames.count(name)
                    ? nullptr : module_->findGlobal(name);
                auto slot = localArrayValues_.find(param->name);
                if (g && g->storage == StorageQualifier::Const && g->declaredStatic &&
                    g->type.isArray() && slot != localArrayValues_.end() &&
                    slot->second.size() == static_cast<size_t>(g->type.arraySize))
                {
                    for (int e = 0; e < g->type.arraySize; ++e)
                        slot->second[static_cast<size_t>(e)] = foldStaticConstArrayElement(*g, e);
                }
            }
        }
    }

    // Names the caller shadows with a local: for the body, the name means
    // the GLOBAL.  Bind its stashed value, or unbind the name so a read
    // falls through to the global load when nothing ever assigned it.
    // Only the callee's PARAMETERS are exempt here - they were bound above
    // and every occurrence in the body names them.  A top-level local the
    // callee declares LATER is not: a read before that declaration names
    // the global (review: codex - `D = G; float4 G = ...;` read the
    // enclosing helper's parameter when the whole-body ownership set was
    // used as the exemption; the declaration rebinds the name when it is
    // reached, as buildDeclStmt does for any shadowing local).
    auto calleeParamOwns = [&](const std::string& key) -> bool
    {
        for (const auto& param : callee->parameters)
        {
            const std::string& n = param->name;
            if (n.empty()) continue;
            if (key == n || (key.size() > n.size() && key[n.size()] == '.' && key.compare(0, n.size(), n) == 0))
                return true;
        }
        return false;
    };
    for (const auto& kv : shadowedGlobals_)
    {
        if (calleeParamOwns(kv.first)) continue;
        if (kv.second != InvalidIRValue) nameToValue_[kv.first] = kv.second;
        else nameToValue_.erase(kv.first);
    }
    for (const auto& kv : shadowedGlobalArrays_)
    {
        if (calleeParamOwns(kv.first)) continue;
        if (!kv.second.empty()) localArrayValues_[kv.first] = kv.second;
        else localArrayValues_.erase(kv.first);
    }

    // The nested-helper refusal that stood here ("names file-scope X while
    // an enclosing helper's parameter or local of that name is in scope")
    // is gone (inline-global-stash): the enclosing helper's parameter now stashes
    // the global at binding, its locals stash at declaration and unstash
    // at block exit, so a nested helper reads and writes the GLOBAL by
    // name through the stash like any other caller shadow.
    const auto& savedStash = savedScope.shadowedGlobals;
    const auto& savedStashArrays = savedScope.shadowedGlobalArrays;
    inlineStack_.push_back(callee);
    blockDeclared_.emplace_back();   // the callee's top-level locals are its own, never the caller's block's
    const bool ok = buildInlineFunctionBody(callee, result);
    calleeDirect.insert(blockDeclared_.back().begin(), blockDeclared_.back().end());
    blockDeclared_.pop_back();       // discarded: scope_ = savedScope below drops them
    inlineStack_.pop_back();
    // A stash entry the callee CREATED (its own parameter or local shadowing
    // a global) is the callee's scope and ends with it - but the VALUE it
    // holds is the global's, as a nested helper left it, and that write
    // reaches the caller below (createdStash), exactly as block exit
    // unstashes.  An entry that existed before the call keeps the callee's
    // update to the global.
    std::unordered_map<std::string, IRValueID> createdStash;
    std::unordered_map<std::string, std::vector<IRValueID>> createdStashArrays;
    for (auto it = shadowedGlobals_.begin(); it != shadowedGlobals_.end(); )
        if (!savedStash.count(it->first)) { createdStash[it->first] = it->second; it = shadowedGlobals_.erase(it); } else ++it;
    for (auto it = shadowedGlobalArrays_.begin(); it != shadowedGlobalArrays_.end(); )
        if (!savedStashArrays.count(it->first)) { createdStashArrays[it->first] = it->second; it = shadowedGlobalArrays_.erase(it); } else ++it;

    std::vector<IRValueID> copyOutValues;
    for (size_t i : copyOut)
    {
        auto it = nameToValue_.find(callee->parameters[i]->name);
        copyOutValues.push_back(it == nameToValue_.end() ? InvalidIRValue : it->second);
    }
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
    // codex, scope-param-rebound; inline-entry-parameter-scope) - a reassigned local lost
    // the same way.
    auto callerLocalBinding = [&](const std::string& name, IRValueID) -> bool
    {
        return shadowedGlobals_.count(name) != 0;
    };
    for (const auto& kv : bodyNames)
    {
        if (calleeOwns(kv.first)) continue;
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
        if (calleeOwns(kv.first)) continue;
        IRGlobal* global = module_->findGlobal(kv.first);
        if (!global || !global->type.isArray()) continue;
        if (callerLocalArray(kv.first)) { shadowedGlobalArrays_[kv.first] = kv.second; continue; }
        localArrayValues_[kv.first] = kv.second;
    }
    // Global writes that landed in a stash entry the callee CREATED (a nested
    // helper wrote the global while this callee's parameter or local shadowed
    // it): the callee's own binding is gone, the global's new value goes
    // where the caller keeps the global - its stash if it shadows, its name
    // otherwise.  Not skipped by ownership: the key is the callee's binding
    // NAME but the value is the GLOBAL's.
    for (const auto& kv : createdStash)
    {
        if (kv.second == InvalidIRValue || !module_->findGlobal(kv.first)) continue;
        if (shadowedGlobals_.count(kv.first)) shadowedGlobals_[kv.first] = kv.second;
        else nameToValue_[kv.first] = kv.second;
    }
    for (const auto& kv : createdStashArrays)
    {
        if (kv.second.empty()) continue;
        IRGlobal* global = module_->findGlobal(kv.first);
        if (!global || !global->type.isArray()) continue;
        if (shadowedGlobalArrays_.count(kv.first)) shadowedGlobalArrays_[kv.first] = kv.second;
        else localArrayValues_[kv.first] = kv.second;
    }
    for (const auto& kv : inlineSwizzles)
        identityPrefixSwizzleBase_.try_emplace(kv.first, kv.second);
    // Copy-out into the caller's own scope (restored above), so the
    // argument names the CALLER's variable; right to left, see above.
    for (size_t k = copyOut.size(); ok && k-- > 0; )
    {
        const ParamDecl* param = callee->parameters[copyOut[k]].get();
        if (copyOutValues[k] == InvalidIRValue)
        {
            error(expr->loc, "cannot inline user function '" + callee->name +
                             "': out parameter '" + param->name + "' is never assigned");
            return false;
        }
        buildAssignment(expr->arguments[copyOut[k]].get(), copyOutValues[k]);
    }
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
        // The statement builder will prove the trip sequence at the call site.
        // Continue rejecting exits nested anywhere in the loop body beforehand.
        return inlineBlockedShape(static_cast<const ForStmt*>(stmt)->body.get(), why);
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
    std::vector<StmtNode*> statements;
    for (const auto& s : callee->body->statements) statements.push_back(s.get());
    // A helper called from inside another helper's arm has its OWN
    // continuation: the caller's tail is not part of this body.
    const std::vector<StmtNode*> outerContinuation = std::move(inlineContinuation_);
    const bool outerRan = inlineContinuationRun_;
    inlineContinuation_.clear();
    const bool ran = runInlineStatements(callee, statements, result, sawReturn);
    inlineContinuation_ = outerContinuation;
    inlineContinuationRun_ = outerRan;
    if (!ran)
        return false;

    if (!sawReturn && !returnsVoid)
    {
        error(callee->loc, "cannot inline user function '" + callee->name +
                           "': no return expression");
        return false;
    }
    return returnsVoid || result != InvalidIRValue;
}

// An if whose arms reach a return through nothing but blocks and nested ifs
// (no loop, switch, break or continue around the return): the shape
// runInlineStatements lowers by running each arm with the rest of the body.
static bool returnOnlyShape(const StmtNode* stmt, bool& hasReturn)
{
    if (!stmt) return true;
    switch (stmt->kind)
    {
    case StmtKind::Return: hasReturn = true; return true;
    case StmtKind::Block:
        for (const auto& inner : static_cast<const BlockStmt*>(stmt)->statements)
            if (!returnOnlyShape(inner.get(), hasReturn)) return false;
        return true;
    case StmtKind::If:
    {
        const auto* ifs = static_cast<const IfStmt*>(stmt);
        return returnOnlyShape(ifs->thenBranch.get(), hasReturn) &&
               returnOnlyShape(ifs->elseBranch.get(), hasReturn);
    }
    case StmtKind::For:
    {
        bool inner = false;
        return returnOnlyShape(static_cast<const ForStmt*>(stmt)->body.get(), inner) && !inner;
    }
    case StmtKind::While: case StmtKind::DoWhile: case StmtKind::Switch:
    case StmtKind::Case: case StmtKind::Default:
    case StmtKind::Break: case StmtKind::Continue:
        return false;
    default:
        return true;
    }
}

bool IRBuilder::runInlineStatements(FunctionDecl* callee, const std::vector<StmtNode*>& statements,
                                    IRValueID& result, bool& sawReturn)
{
    const bool returnsVoid =
        getIRType(callee->returnType.get()).baseType == IRType::Void;
    for (size_t index = 0; index < statements.size(); ++index)
    {
        StmtNode* stmt = statements[index];
        if (!stmt) continue;

        // This path has returned (including a taken constant branch).
        // Its suffix is unreachable; callers still unwind scopes and copy
        // out parameters normally. Runtime joins handle their own tails.
        if (sawReturn)
            break;

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
            // Normalize a scalar return to the declared vector type before
            // branch results are joined. Otherwise `return 0` gives the
            // Select an int type and truncates the other vector arm to x.
            // Reuse constructor conversion/folding and evaluate the returned
            // expression once, just as an explicit float4(expression) does.
            const IRTypeInfo returnType = getIRType(callee->returnType.get());
            const IRTypeInfo valueType = getExprType(ret->value.get());
            if (result != InvalidIRValue && returnType.isVector() && returnType.arraySize == 0 &&
                valueType.isScalar() && valueType.arraySize == 0)
            {
                const IRValueID folded = tryFoldVecConstruct(returnType, {result},
                    callee->returnType->baseType);
                result = folded != InvalidIRValue ? folded :
                    emitInstruction(IROp::VecConstruct, returnType, {result}, ret->loc);
            }
            result = narrowToScalar(callee->returnType.get(), ret->value.get(), result);
            sawReturn = true;
            continue;
        }

        if (stmt->kind == StmtKind::Empty)
            continue;

        // An if whose condition is a compile-time constant runs only its
        // taken branch, inline, as if its statements were written here: a
        // return in that branch is then an ordinary top-level return.  The
        // untaken branch is never built - it is dead code the reference
        // never emits (libretro decode_input: `if (linearize_input) {
        // if (assume_opaque_alpha) return ...; else return ...; } else
        // return color;` over `static const bool`s).
        if (stmt->kind == StmtKind::If)
        {
            auto* ifs = static_cast<IfStmt*>(stmt);
            if (const std::optional<bool> taken = constCondition(ifs->condition.get()))
            {
                StmtNode* branch = *taken ? ifs->thenBranch.get() : ifs->elseBranch.get();
                const bool block = branch && branch->kind == StmtKind::Block;
                std::vector<StmtNode*> inner;
                if (block)
                    for (const auto& s : static_cast<BlockStmt*>(branch)->statements)
                        inner.push_back(s.get());
                else if (branch)
                    inner.push_back(branch);
                // The branch's braces are still a block: its own
                // declarations end with it, as buildBlockStmt ends them.
                const ScopeState pre = scope_;
                if (block) blockDeclared_.emplace_back();
                const std::vector<StmtNode*> inherited = inlineContinuation_;
                std::vector<StmtNode*> tail(statements.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                                            statements.end());
                tail.insert(tail.end(), inherited.begin(), inherited.end());
                inlineContinuation_ = tail;
                inlineContinuationRun_ = false;
                const size_t savedFrame = inlineContinuationFrame_;
                inlineContinuationFrame_ = block ? blockDeclared_.size() - 1 : blockDeclared_.size();
                const bool ok = runInlineStatements(callee, inner, result, sawReturn);
                inlineContinuationFrame_ = savedFrame;
                inlineContinuation_ = inherited;
                const bool tailRan = inlineContinuationRun_;
                if (block)
                {
                    const std::unordered_set<std::string> declared = std::move(blockDeclared_.back());
                    blockDeclared_.pop_back();
                    for (const auto& name : declared)
                        exitBlockBinding(name, pre);
                }
                if (!ok)
                    return false;
                if (tailRan)
                    return true;   // a run-time return inside ran this list's tail already
                continue;
            }
        }

        // A return under a RUN-TIME condition.  Each arm runs as its own
        // statements followed - when the arm does not itself return - by the
        // rest of the body, and the if-join merges the two arms' results
        // (and every local and global they wrote) with Select, the
        // straight-line predicated program the reference emits.  The
        // result travels through a reserved scope key no source name can
        // spell.  An arm's braces are still a block: its own declarations
        // end before the rest of the body runs.
        if (stmt->kind == StmtKind::If)
        {
            auto* ifs = static_cast<IfStmt*>(stmt);
            bool hasReturn = false;
            if (returnOnlyShape(ifs, hasReturn) && hasReturn &&
                (module_->shaderStage == ShaderStage::Fragment ||
                 module_->shaderStage == ShaderStage::Vertex))
            {
                // Both backends lower the joined result and side effects
                // through Select. Vertex helpers need the same continuation
                // handling as fragment helpers; no entry Return is emitted.
                // A constant part of the condition that the guard declines to
                // fold (mixed int/float or half comparisons, aliases of them)
                // would be folded here by the evaluator anyway, at our
                // precision rather than the reference's: refuse it by name.
                if (conditionFoldHazard(ifs->condition.get()))
                {
                    error(ifs->loc, "cannot inline user function '" + callee->name +
                                    "': a return inside control flow under a constant comparison "
                                    "this compiler cannot fold at the reference's precision");
                    return false;
                }
                // Each level copies the rest of the body into both arms, so
                // n sequential early returns build 2^n tails: bounded.
                if (inlineReturnDepth_ >= 8)
                {
                    error(ifs->loc, "cannot inline user function '" + callee->name +
                                    "': more than 8 nested run-time returns");
                    return false;
                }
                struct DepthGuard { int& d; explicit DepthGuard(int& x) : d(x) { ++d; } ~DepthGuard() { --d; } } guard(inlineReturnDepth_);
                static const std::string kReturnKey = "#inline-return";
                // The rest of the body: this list's tail, then whatever an
                // enclosing arm or constant branch still has to run after it.
                // The inherited continuation belongs OUTSIDE the enclosing
                // arm's block; running it in here would let a local that
                // block declared shadow the outer name it reads (an inner
                // `float4 k` returned for the outer `return k`).  Refused by
                // name until block bindings are unwound for it.
                if (!inlineContinuation_.empty())
                    for (size_t f = inlineContinuationFrame_; f < blockDeclared_.size(); ++f)
                        if (!blockDeclared_[f].empty())
                        {
                            error(ifs->loc, "cannot inline user function '" + callee->name +
                                            "': a return inside control flow inside a block that "
                                            "declares locals, with statements after the block");
                            return false;
                        }
                std::vector<StmtNode*> rest(statements.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                                            statements.end());
                rest.insert(rest.end(), inlineContinuation_.begin(), inlineContinuation_.end());
                const std::vector<StmtNode*> inherited = inlineContinuation_;
                bool armsOk = true;
                auto runArm = [&](StmtNode* branch) {
                    if (!armsOk) return;
                    IRValueID armResult = InvalidIRValue;
                    bool armReturned = false;
                    std::vector<StmtNode*> own;
                    const bool block = branch && branch->kind == StmtKind::Block;
                    if (block)
                        for (const auto& s : static_cast<BlockStmt*>(branch)->statements) own.push_back(s.get());
                    else if (branch)
                        own.push_back(branch);
                    const ScopeState pre = scope_;
                    if (block) blockDeclared_.emplace_back();
                    inlineContinuation_ = rest;   // a nested run-time return runs it
                    const size_t savedFrame = inlineContinuationFrame_;
                    inlineContinuationFrame_ = block ? blockDeclared_.size() - 1 : blockDeclared_.size();
                    bool ok = runInlineStatements(callee, own, armResult, armReturned);
                    inlineContinuationFrame_ = savedFrame;
                    inlineContinuation_.clear();  // `rest` already carries it
                    if (block)
                    {
                        const std::unordered_set<std::string> declared = std::move(blockDeclared_.back());
                        blockDeclared_.pop_back();
                        for (const auto& name : declared)
                            exitBlockBinding(name, pre);
                    }
                    if (ok && !armReturned)
                        ok = runInlineStatements(callee, rest, armResult, armReturned);
                    inlineContinuation_ = inherited;
                    if (ok && !armReturned && !returnsVoid)
                    {
                        error(ifs->loc, "cannot inline user function '" + callee->name +
                                        "': a path through it has no return expression");
                        ok = false;
                    }
                    if (ok && !returnsVoid && armResult == InvalidIRValue)
                        ok = false;
                    armsOk = armsOk && ok;
                    if (ok && !returnsVoid)
                        nameToValue_[kReturnKey] = armResult;
                };
                const std::function<void()> thenArm = [&] { runArm(ifs->thenBranch.get()); };
                const std::function<void()> elseArm = [&] { runArm(ifs->elseBranch.get()); };
                buildConditional(ifs->condition.get(), ifs->loc, thenArm, &elseArm);
                if (!armsOk)
                    return false;
                if (!returnsVoid)
                {
                    auto it = nameToValue_.find(kReturnKey);
                    if (it == nameToValue_.end() || it->second == InvalidIRValue)
                        return false;
                    result = it->second;
                    nameToValue_.erase(it);
                }
                sawReturn = true;
                inlineContinuationRun_ = true;   // the enclosing lists' tails ran in the arms
                return true;
            }
        }

        // if/else and nested blocks inline through the ordinary statement
        // builder: the if-join merges the callee's locals (and its
        // parameters, which are plain names here) exactly as it merges the
        // entry function's, so `if (mag > 1) { mv /= mag; }` inside a
        // helper lowers to the same select/predicate shape it would at the
        // call site.  The shapes the join cannot carry are refused by name.
        if (stmt->kind == StmtKind::Decl || stmt->kind == StmtKind::Expr ||
            stmt->kind == StmtKind::If || stmt->kind == StmtKind::Block ||
            stmt->kind == StmtKind::For)
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

    return true;
}

std::optional<bool> IRBuilder::constCondition(const ExprNode* e)
{
    // Not folded (nullopt), so the caller refuses by name rather than guess:
    //  - any name the function binds itself (a local or parameter) shadows
    //    the file-scope constant of that name;
    //  - a half or fixed operand: the reference does not compare at the
    //    reduced precision (static const half H = 0.1; H == 0.1 is TRUE
    //    there), and its exact rule is unmeasured;
    //  - a comparison between a float and an integer operand: the reference
    //    does not round the integer to float (G = 16777217.0, the float
    //    16777216, against the int 16777217 is UNEQUAL there, while float
    //    promotion here would make them equal);
    //  - an expression kind not walked below.
    const auto category = [](const ExprNode* x) -> int {
        if (!x || !x->resolvedType) return -1;
        switch (x->resolvedType->baseType)
        {
        case BaseType::Bool:  return 0;
        case BaseType::Float: return 2;
        case BaseType::Half:
        case BaseType::Fixed: return 3;
        default:              return 1;   // the integer kinds
        }
    };
    int depth = 0;
    std::function<bool(const ExprNode*)> namesLocal = [&](const ExprNode* x) -> bool {
        if (!x) return false;
        if (category(x) == 3) return true;
        if (x->kind == ExprKind::Binary)
        {
            const auto* b = static_cast<const BinaryExpr*>(x);
            switch (b->op)
            {
            case BinaryOp::Equal: case BinaryOp::NotEqual: case BinaryOp::Less:
            case BinaryOp::LessEqual: case BinaryOp::Greater: case BinaryOp::GreaterEqual:
            {
                const int l = category(b->left.get()), r = category(b->right.get());
                if (l < 0 || r < 0 || l != r) return true;
                break;
            }
            default:
                break;
            }
        }
        switch (x->kind)
        {
        case ExprKind::Literal:
            return false;
        case ExprKind::Identifier:
        {
            const auto* id = static_cast<const IdentifierExpr*>(x);
            if (nameToValue_.count(id->name) != 0) return true;
            // A static const carries its initialiser's precision rules with
            // it (review: codex - static const bool B = (G == 16777217);
            // if (B) bypassed the comparison guard, and the reference keeps
            // static const float Z = 16777217; as 16777217, unequal to the
            // float 16777217.0).  Walk the initialiser with the same guard,
            // and decline when its category differs from the declared one.
            const DeclNode* d = id->resolvedDecl;
            if (!d || !globalDeclarations_.count(d) || d->kind != DeclKind::Variable)
                return false;   // not a file-scope variable: the evaluator decides
            const auto* v = static_cast<const VarDecl*>(d);
            if (v->type && (v->type->baseType == BaseType::Half || v->type->baseType == BaseType::Fixed))
                return true;
            if (!v->initializer) return false;
            if (++depth > 16) return true;
            const int declared = !v->type ? -1
                               : v->type->baseType == BaseType::Bool  ? 0
                               : v->type->baseType == BaseType::Float ? 2 : 1;
            const bool bad = category(v->initializer.get()) != declared ||
                             namesLocal(v->initializer.get());
            --depth;
            return bad;
        }
        case ExprKind::Unary:
            return namesLocal(static_cast<const UnaryExpr*>(x)->operand.get());
        case ExprKind::Binary:
        {
            const auto* b = static_cast<const BinaryExpr*>(x);
            return namesLocal(b->left.get()) || namesLocal(b->right.get());
        }
        case ExprKind::Ternary:
        {
            const auto* t = static_cast<const TernaryExpr*>(x);
            return namesLocal(t->condition.get()) || namesLocal(t->thenExpr.get()) ||
                   namesLocal(t->elseExpr.get());
        }
        case ExprKind::Cast:
            return namesLocal(static_cast<const CastExpr*>(x)->operand.get());
        case ExprKind::MemberAccess:
            return namesLocal(static_cast<const MemberAccessExpr*>(x)->object.get());
        case ExprKind::Constructor:
            for (const auto& arg : static_cast<const ConstructorExpr*>(x)->arguments)
                if (namesLocal(arg.get())) return true;
            return false;
        default:
            return true;
        }
    };
    if (!e || namesLocal(e)) return std::nullopt;
    // The file-scope initialiser evaluator is the one constant semantics:
    // typed scalar arithmetic and comparisons (1 == 2 is false, not "both
    // truthy"), and only a STATIC const is a constant - a plain file-scope
    // const is a uniform with a default in the reference.
    ConstLanes lanes;
    ConstShape shape;
    if (!evaluateConstValue(e, lanes, shape, module_.get()) || lanes.size() != 1 || !shape.isScalar())
        return std::nullopt;
    return lanes[0].isTruthy();
}

bool IRBuilder::conditionFoldHazard(const ExprNode* e)
{
    if (!e) return false;
    // A name the function binds itself is a run-time value here, whatever a
    // file-scope constant of that name would evaluate to.
    std::function<bool(const ExprNode*)> mentionsLocal = [&](const ExprNode* x) -> bool {
        if (!x) return false;
        switch (x->kind)
        {
        case ExprKind::Identifier: return nameToValue_.count(static_cast<const IdentifierExpr*>(x)->name) != 0;
        case ExprKind::Unary: return mentionsLocal(static_cast<const UnaryExpr*>(x)->operand.get());
        case ExprKind::Binary: { const auto* b = static_cast<const BinaryExpr*>(x);
                                 return mentionsLocal(b->left.get()) || mentionsLocal(b->right.get()); }
        case ExprKind::Ternary: { const auto* t = static_cast<const TernaryExpr*>(x);
                                  return mentionsLocal(t->condition.get()) || mentionsLocal(t->thenExpr.get()) ||
                                         mentionsLocal(t->elseExpr.get()); }
        case ExprKind::Cast: return mentionsLocal(static_cast<const CastExpr*>(x)->operand.get());
        case ExprKind::MemberAccess: return mentionsLocal(static_cast<const MemberAccessExpr*>(x)->object.get());
        default: return false;
        }
    };
    ConstLanes lanes;
    ConstShape shape;
    if (!mentionsLocal(e) &&
        evaluateConstValue(e, lanes, shape, module_.get()) && lanes.size() == 1 && shape.isScalar() &&
        !constCondition(e))
        return true;
    switch (e->kind)
    {
    case ExprKind::Unary:
        return conditionFoldHazard(static_cast<const UnaryExpr*>(e)->operand.get());
    case ExprKind::Binary:
    {
        const auto* b = static_cast<const BinaryExpr*>(e);
        return conditionFoldHazard(b->left.get()) || conditionFoldHazard(b->right.get());
    }
    case ExprKind::Ternary:
    {
        const auto* t = static_cast<const TernaryExpr*>(e);
        return conditionFoldHazard(t->condition.get()) || conditionFoldHazard(t->thenExpr.get()) ||
               conditionFoldHazard(t->elseExpr.get());
    }
    case ExprKind::Cast:
        return conditionFoldHazard(static_cast<const CastExpr*>(e)->operand.get());
    case ExprKind::MemberAccess:
        return conditionFoldHazard(static_cast<const MemberAccessExpr*>(e)->object.get());
    case ExprKind::Constructor:
        for (const auto& arg : static_cast<const ConstructorExpr*>(e)->arguments)
            if (conditionFoldHazard(arg.get())) return true;
        return false;
    case ExprKind::Call:
        for (const auto& arg : static_cast<const CallExpr*>(e)->arguments)
            if (conditionFoldHazard(arg.get())) return true;
        return false;
    default:
        return false;
    }
}

// The load of a varying struct parameter's member, as `valueId`, into the
// current block: the lazy first read, and an if-join's untouched side of a
// member one arm assigned before any read.  False when `param` is not a
// struct parameter with that member (nothing is emitted).
bool IRBuilder::emitInputMemberLoad(const ParamDecl* param, const std::string& member,
                                    IRValueID valueId)
{
    if (!param || !param->type || param->type->baseType != BaseType::Struct) return false;
    const std::vector<StructField>* fields = getStructFields(param->type.get());
    if (!fields) return false;
    for (size_t fieldIdx = 0; fieldIdx < fields->size(); ++fieldIdx)
    {
        const auto& field = (*fields)[fieldIdx];
        if (field.name != member) continue;
        IRTypeInfo fieldType = getIRType(field.type.get());
        auto inst = std::make_unique<IRInstruction>(IROp::LoadAttribute, valueId, fieldType);
        inst->semanticName     = field.semantic.name;
        inst->rawSemanticName  = field.semantic.rawName;
        inst->inferredSemantic = field.semantic.inferred;
        inst->semanticIndex    = field.semantic.isEmpty()
            ? static_cast<int>(fieldIdx)
            : field.semantic.index;
        inst->structParamName  = param->name;       // e.g. "input"
        inst->fieldName        = member;            // e.g. "pos"
        currentBlock_->addInstruction(std::move(inst));
        return true;
    }
    return false;
}

IRValueID IRBuilder::buildMemberAccessExpr(MemberAccessExpr* expr)
{
    // A temporary aggregate has no qualified name to bind. Do not allocate
    // an undefined scalar value for a field of S(...), including nested fields.
    ExprNode* root = expr->object.get();
    while (root && root->kind == ExprKind::MemberAccess)
        root = static_cast<MemberAccessExpr*>(root)->object.get();
    if (root && root->kind == ExprKind::Constructor &&
        getStructFields(static_cast<ConstructorExpr*>(root)->constructedType.get()))
    {
        error(expr->loc, "struct constructor member access is not supported (local-struct-initializer)");
        return InvalidIRValue;
    }
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
            if (emitInputMemberLoad(static_cast<ParamDecl*>(ident->resolvedDecl),
                                    expr->member, valueId))
                return valueId;
        }

        // Check if this is a uniform struct global - emit LoadUniform with qualified name
        IRGlobal* global = module_->findGlobal(ident->name);
        if (global && global->storage == StorageQualifier::Uniform)
        {
            IRTypeInfo memberType = getExprType(expr);
            auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
                valueId, memberType);
            inst->targetName = compositeName;  // Use qualified name e.g. "u_data.color"
            if (const auto* memberGlobal = module_->findGlobal(compositeName))
                inst->uniformSource = memberGlobal->valueId;
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
        // A nested member of a FILE-SCOPE uniform struct ("u.in1.a") is a
        // flattened uniform global of that full name: load it, the way the
        // single-level path above loads "u.b".  Without this the value was
        // allocated with no instruction behind it and lowering refused an
        // unresolved operand - the shape a helper reading the global hits
        // when an entry parameter of the same name shadows it.
        if (const IRGlobal* member = module_->findGlobal(compositeName);
            member && member->storage == StorageQualifier::Uniform)
        {
            auto inst = std::make_unique<IRInstruction>(IROp::LoadUniform,
                valueId, getExprType(expr));
            inst->targetName = compositeName;
            inst->uniformSource = member->valueId;
            currentBlock_->addInstruction(std::move(inst));
        }
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
// wrongly, until the typed constant evaluator (static-const-qualifiers) serves this
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
            // u[0] (measured, float-array-index-conversion).  Arithmetic over float leaves
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
                // measured on static-const-qualifiers), so u[(int)4294967296.0] is out of
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

namespace {
bool arrayStorageKey(ExprNode* expr, std::string& key)
{
    if (expr->kind == ExprKind::Identifier)
    {
        auto* id = static_cast<IdentifierExpr*>(expr);
        if (!id->resolvedDecl) return false;
        key = id->name;
        return true;
    }
    if (expr->kind != ExprKind::MemberAccess) return false;
    auto* member = static_cast<MemberAccessExpr*>(expr);
    if (member->isSwizzle || !arrayStorageKey(member->object.get(), key)) return false;
    key += "." + member->member;
    return true;
}
}

bool IRBuilder::buildLocalStructInitializer(const std::string& destination, TypeNode* type, ExprNode* source,
                                           bool replaceExisting)
{
    // A struct has qualified leaf bindings, not a scalar IR value. Snapshot
    // every source leaf in source order before installing any destination.
    // Clone only side-effect-free member paths; calls returning a whole struct
    // and indexed aggregate paths remain named gaps, never evaluated per leaf.
    const auto clonePath = [&](const auto& self, ExprNode* expr) -> std::unique_ptr<ExprNode> {
        if (!expr) return nullptr;
        std::unique_ptr<ExprNode> result;
        if (expr->kind == ExprKind::Identifier)
        {
            auto* id = static_cast<IdentifierExpr*>(expr);
            auto copy = std::make_unique<IdentifierExpr>(id->loc, id->name);
            copy->resolvedDecl = id->resolvedDecl;
            result = std::move(copy);
        }
        else if (expr->kind == ExprKind::MemberAccess)
        {
            auto* member = static_cast<MemberAccessExpr*>(expr);
            if (member->isSwizzle) return nullptr;
            auto object = self(self, member->object.get());
            if (!object) return nullptr;
            result = std::make_unique<MemberAccessExpr>(member->loc, std::move(object), member->member);
        }
        if (result) result->resolvedType = expr->resolvedType;
        return result;
    };
    std::vector<std::pair<std::string, IRValueID>> leaves;
    const auto collect = [&](const auto& self, const std::string& key,
                             TypeNode* target, ExprNode* value) -> bool {
        if (!target || !value) return false;
        if (target->isArray() || target->isSampler())
        {
            error(value->loc, "array or sampler struct initializer field is not supported (local-struct-initializer)");
            return false;
        }
        if (const auto* fields = getStructFields(target))
        {
            ConstructorExpr* ctor = value->kind == ExprKind::Constructor
                ? static_cast<ConstructorExpr*>(value) : nullptr;
            if (ctor && ctor->arguments.size() != fields->size())
            {
                error(value->loc, "struct initializer field count mismatch (local-struct-initializer)");
                return false;
            }
            for (size_t i = 0; i < fields->size(); ++i)
            {
                const auto& field = (*fields)[i];
                if (ctor)
                {
                    if (!self(self, key + "." + field.name, field.type.get(), ctor->arguments[i].get()))
                        return false;
                }
                else
                {
                    auto object = clonePath(clonePath, value);
                    if (!object)
                    {
                        error(value->loc, "struct initializer source is not a named member path (local-struct-initializer)");
                        return false;
                    }
                    auto member = std::make_unique<MemberAccessExpr>(value->loc, std::move(object), field.name);
                    member->resolvedType = field.type;
                    if (!self(self, key + "." + field.name, field.type.get(), member.get()))
                        return false;
                }
            }
            return true;
        }
        // This is a lowering limitation, not an invalid constructor type.
        // Keep it here so a valid fixed/short constructor in an unreachable
        // helper cannot reject an otherwise supported entry point. Check the
        // AST kinds before IRTypeInfo erases fixed/narrow-integer semantics.
        const auto supportedKind = [](const TypeNode* type) {
            return type && (type->baseType == BaseType::Float ||
                            type->baseType == BaseType::Half ||
                            type->baseType == BaseType::Int);
        };
        if (!supportedKind(target) || !supportedKind(value->resolvedType.get()))
        {
            error(value->loc, "struct initializer numeric kind is not supported (local-struct-initializer)");
            return false;
        }
        IRValueID v = buildExpr(value);
        if (v == InvalidIRValue) return false;
        const auto from = getExprType(value), to = getIRType(target);
        const auto f = from.isVector() || from.isMatrix() ? from.elementType : from.baseType;
        const auto t = to.isVector() || to.isMatrix() ? to.elementType : to.baseType;
        if (f != t)
        {
            IROp op;
            if (f == IRType::Float32 && t == IRType::Float16) op = IROp::FloatToHalf;
            else if (f == IRType::Float16 && t == IRType::Float32) op = IROp::HalfToFloat;
            else if ((f == IRType::Float32 || f == IRType::Float16) &&
                     (t == IRType::Int32 || t == IRType::UInt32)) op = IROp::FloatToInt;
            else if ((f == IRType::Int32 || f == IRType::UInt32) &&
                     (t == IRType::Float32 || t == IRType::Float16)) op = IROp::IntToFloat;
            else
            {
                error(value->loc, "struct initializer field conversion is not supported (local-struct-initializer)");
                return false;
            }
            v = emitInstruction(op, to, {v}, value->loc);
        }
        leaves.emplace_back(key, v);
        return true;
    };
    if (!collect(collect, destination, type, source)) return false;
    if (replaceExisting)
    {
        // A former partial write must not survive replacement of its parent.
        // All source values, including overlapping paths, are already captured.
        for (auto it = nameToValue_.begin(); it != nameToValue_.end(); )
            if (it->first == destination || it->first.compare(0, destination.size() + 1, destination + ".") == 0)
                it = nameToValue_.erase(it);
            else ++it;
    }
    for (const auto& leaf : leaves) nameToValue_[leaf.first] = leaf.second;
    return true;
}

void IRBuilder::emitEntryStructOutputs(ExprNode* target)
{
    if (!target || !currentFunction_->isEntryPoint || !currentFunctionDecl_ ||
        !inlineStack_.empty() || module_->shaderStage != ShaderStage::Vertex) return;
    std::vector<std::string> members;
    ExprNode* root = target;
    while (root->kind == ExprKind::MemberAccess)
    {
        auto* member = static_cast<MemberAccessExpr*>(root);
        if (member->isSwizzle) return;
        members.push_back(member->member);
        root = member->object.get();
    }
    if (root->kind != ExprKind::Identifier) return;
    const auto* identifier = static_cast<IdentifierExpr*>(root);
    if (!identifier->resolvedDecl || identifier->resolvedDecl->kind != DeclKind::Parameter) return;
    const auto* parameter = static_cast<ParamDecl*>(identifier->resolvedDecl);
    if ((parameter->storage != StorageQualifier::Out && parameter->storage != StorageQualifier::InOut) ||
        !std::any_of(currentFunctionDecl_->parameters.begin(), currentFunctionDecl_->parameters.end(),
            [&](const auto& own) { return own.get() == parameter; })) return;
    TypeNode* type = parameter->type.get();
    if (!getStructFields(type)) return;
    std::string path;
    const StructField* selected = nullptr;
    for (auto it = members.rbegin(); it != members.rend(); ++it)
    {
        const auto* fields = getStructFields(type);
        if (!fields) return;
        selected = nullptr;
        for (const auto& field : *fields)
            if (field.name == *it) { selected = &field; break; }
        if (!selected) return;
        path += (path.empty() ? "" : ".") + *it;
        type = selected->type.get();
    }
    const auto emit = [&](auto& self, TypeNode* leafType, const std::string& fieldPath,
                          const StructField* field) -> void {
        if (const auto* fields = getStructFields(leafType))
        {
            for (const auto& child : *fields)
                self(self, child.type.get(), fieldPath.empty() ? child.name : fieldPath + "." + child.name, &child);
            return;
        }
        if (!field || field->semantic.isEmpty()) return;
        const IRTypeInfo irType = getIRType(leafType);
        if (!leafType || leafType->baseType != BaseType::Float || irType.isArray() || irType.isMatrix())
        {
            error(target->loc, "entry struct output assignment requires float scalar/vector leaves "
                               "(struct-constructor-assignment)");
            return;
        }
        const auto value = nameToValue_.find(identifier->name + "." + fieldPath);
        if (value == nameToValue_.end())
        {
            error(target->loc, "entry struct output has no assigned leaf '" + fieldPath + "'");
            return;
        }
        // Only a later write in this same block supersedes the earlier output.
        // A predecessor's store may still be needed on another control-flow path.
        auto& instructions = currentBlock_->instructions;
        for (auto it = instructions.begin(); it != instructions.end(); )
            if ((*it)->op == IROp::StoreOutput && (*it)->semanticName == field->semantic.name &&
                (*it)->semanticIndex == field->semantic.index)
                it = instructions.erase(it);
            else ++it;
        auto store = std::make_unique<IRInstruction>(IROp::StoreOutput, InvalidIRValue, irType);
        store->addOperand(value->second);
        store->semanticName = field->semantic.name;
        store->rawSemanticName = field->semantic.rawName;
        store->inferredSemantic = field->semantic.inferred;
        store->semanticIndex = field->semantic.index;
        store->fieldName = fieldPath;
        store->loc = target->loc;
        currentBlock_->addInstruction(std::move(store));
    };
    emit(emit, type, path, selected);
}

bool IRBuilder::copyArrayAggregate(const std::string& destination, ExprNode* source, TypeNode* type)
{
    // The standalone helper IR has no caller element bindings. Its body is
    // rebuilt at each inline call with those bindings installed below.
    if (!currentFunction_->isEntryPoint && inlineStack_.empty()) return false;
    auto hasArray = [&](auto& self, TypeNode* t) -> bool {
        if (!t) return false;
        if (getIRType(t).isArray()) return true;
        if (const auto* fields = getStructFields(t))
            for (const auto& field : *fields)
                if (self(self, field.type.get())) return true;
        return false;
    };
    if (!getStructFields(type) || !hasArray(hasArray, type)) return false;
    std::string origin;
    // An assignment expression denotes its newly assigned destination.
    while (source && source->kind == ExprKind::Binary &&
           static_cast<BinaryExpr*>(source)->op == BinaryOp::Assign)
        source = static_cast<BinaryExpr*>(source)->left.get();
    if (!source || !arrayStorageKey(source, origin))
    {
        error("array-bearing struct copy requires a tracked source (member-array-storage)");
        return true;
    }
    const auto arrays = localArrayValues_;
    const auto names = nameToValue_;
    auto copy = [&](auto& self, TypeNode* t, const std::string& from, const std::string& to) -> void {
        const auto ir = getIRType(t);
        if (ir.isArray())
        {
            const auto found = arrays.find(from);
            if (found == arrays.end())
                error("array-bearing struct copy requires tracked elements (member-array-storage)");
            else localArrayValues_[to] = found->second;
            return;
        }
        if (const auto* fields = getStructFields(t))
        {
            for (const auto& field : *fields)
                if (field.type) self(self, field.type.get(), from + "." + field.name, to + "." + field.name);
            return;
        }
        // An absent source leaf must not leave an older destination value.
        for (auto it = nameToValue_.begin(); it != nameToValue_.end(); )
            if (it->first == to || it->first.compare(0, to.size() + 1, to + ".") == 0)
                it = nameToValue_.erase(it);
            else ++it;
        for (const auto& entry : names)
            if (entry.first == from || entry.first.compare(0, from.size() + 1, from + ".") == 0)
                nameToValue_[to + entry.first.substr(from.size())] = entry.second;
    };
    copy(copy, type, origin, destination);
    return true;
}

bool IRBuilder::resolveTrackedArrayElement(IndexExpr* expr, std::string& key, int32_t& index)
{
    const auto type = getExprType(expr->array.get());
    if (!type.isArray() || !arrayStorageKey(expr->array.get(), key))
    {
        error(expr->loc, "member-array storage path is not supported (member-array-storage)");
        return false;
    }
    auto found = localArrayValues_.find(key);
    if (found == localArrayValues_.end())
    {
        // Preserve file-scope promotion. A scoped declaration initializes
        // its own array map, so it cannot inherit a global's element values.
        const auto* global = module_->findGlobal(key);
        // The global's own id as the name's binding is the module-level
        // placeholder a mutable static carries into the first function
        // built, not a scoped shadow: a helper writing `g[0]` of a static
        // array must still reach the global (initialiser_side_effect_array).
        const auto bound = nameToValue_.find(key);
        const bool shadowed = bound != nameToValue_.end() && global &&
                              bound->second != global->valueId;
        if (global && global->type.isArray() && !shadowed)
            found = localArrayValues_.emplace(key,
                std::vector<IRValueID>(static_cast<size_t>(type.arraySize), InvalidIRValue)).first;
    }
    if (found == localArrayValues_.end())
    {
        error(expr->loc, "member-array storage is not tracked (member-array-storage)");
        return false;
    }
    const IRValueID selector = buildExpr(expr->index.get());
    if (selector == InvalidIRValue) return false;
    if (!extractIntScalar(*currentFunction_, selector, index))
    {
        auto* constant = dynamic_cast<IRConstant*>(currentFunction_->getValue(selector));
        std::vector<float> components;
        if (!constant || !constant->type.isScalar() ||
            !extractFloatComponents(*currentFunction_, selector, components) || components.size() != 1)
        {
            error(expr->loc, "member-array store requires a constant index (member-array-storage)");
            return false;
        }
        const double truncated = std::trunc(static_cast<double>(components[0]));
        if (!std::isfinite(truncated) || truncated < 0 || truncated >= type.arraySize)
        {
            error(expr->loc, "member-array index outside OUR supported range (member-array-storage; reference accepts measured forwarding cases)");
            return false;
        }
        index = static_cast<int32_t>(truncated);
    }
    if (index < 0 || index >= type.arraySize)
    {
        error(expr->loc, "member-array index outside OUR supported range (member-array-storage; reference accepts measured forwarding cases)");
        return false;
    }
    return true;
}

IRValueID IRBuilder::readTrackedArrayElement(IndexExpr* expr, const std::string& key, int32_t index)
{
    const IRValueID value = localArrayValues_.at(key)[static_cast<size_t>(index)];
    if (value != InvalidIRValue) return value;
    ExprNode* root = expr->array.get();
    while (root->kind == ExprKind::MemberAccess)
        root = static_cast<MemberAccessExpr*>(root)->object.get();
    const auto* global = module_->findGlobal(key);
    if (root->kind != ExprKind::Identifier || !global ||
        !globalDeclarations_.count(static_cast<IdentifierExpr*>(root)->resolvedDecl))
        return InvalidIRValue;
    // A promoted global's untouched elements still come from the uniform.
    // Compound assignment must use this value, never an undefined local.
    const IRValueID id = currentFunction_->allocateValueId();
    auto load = std::make_unique<IRInstruction>(IROp::LoadUniform, id, getExprType(expr));
    load->targetName = key;
    load->uniformSource = global->valueId;
    load->componentIndex = index;
    load->arrayIndexKind = IRInstruction::ArrayIndexKind::Constant;
    load->loc = expr->loc;
    currentBlock_->addInstruction(std::move(load));
    return id;
}

IRValueID IRBuilder::buildIndexExpr(IndexExpr* expr)
{
    std::string trackedKey;
    if (getExprType(expr->array.get()).isArray() &&
        arrayStorageKey(expr->array.get(), trackedKey) && localArrayValues_.count(trackedKey))
    {
        const IRGlobal* global = module_->findGlobal(trackedKey);
        // Entry setup seeds initialized mutable static arrays by element,
        // even after a preceding function has cleared their name bindings.
        // Read that tracked state using the once-built IR selector: an
        // unrolled induction variable is constant here, though its AST is
        // still an identifier. Preserve writes instead of refolding the
        // initializer, and require global declaration provenance so a local
        // shadow cannot acquire this path through its spelling alone.
        const bool initializedStatic = global && global->declaredStatic &&
            global->storage == StorageQualifier::Static && !global->type.isMatrix() &&
            (!global->initialValue.empty() || !global->initialIntValues.empty()) &&
            expr->array->kind == ExprKind::Identifier &&
            globalDeclarations_.count(static_cast<IdentifierExpr*>(expr->array.get())->resolvedDecl);
        if (nameToValue_.count(trackedKey) || !global || initializedStatic)
        {
            int32_t index = 0;
            if (!resolveTrackedArrayElement(expr, trackedKey, index)) return InvalidIRValue;
            return readTrackedArrayElement(expr, trackedKey, index);
        }
    }
    // A UNIFORM ARRAY element is a LoadUniform that names how the element
    // was chosen (IRInstruction::arrayIndexKind), never a VecExtract: the
    // VecExtract lowering reads its selector as a LANE and falls back to
    // lane 0, which for an array base would silently read element 0 for
    // every index (constant-uniform-array-index).
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
        // ASSIGNED is promoted to a value array by the store (user-function-inlining):
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

                // A file-scope CONST array read at a constant index is its
                // element, folded here like a const scalar's read: there is
                // no uniform record behind a const, so a LoadUniform of it
                // refused in the lowering ("array index 0 out of bounds for
                // array uniform", `static const float k[3] = {...}`).
                // STATIC const only: a non-static `const` array is a uniform
                // with a default on the reference (it lists k[0..2] as
                // parameters), the separate t_528b9869 gap, and folding it
                // would drop those parameters silently.
                if (global && global->storage == StorageQualifier::Const && global->declaredStatic)
                {
                    const IRValueID folded = foldStaticConstArrayElement(*global, elementIndex);
                    if (folded != InvalidIRValue) return folded;
                }
            }
            else
            {
                indexValue = buildExpr(expr->index.get());
                if (indexValue == InvalidIRValue)
                    return InvalidIRValue;
                // A proven unrolled loop binds its induction variable to an
                // IR constant, although the AST still contains an identifier.
                // Evaluate the selector once (including any increment), then
                // use that bound integer for a static const array's element.
                // Non-static arrays retain their uniform records and loads.
                int32_t boundIndex = 0;
                if (global && global->storage == StorageQualifier::Const &&
                    global->declaredStatic &&
                    extractIntScalar(*currentFunction_, indexValue, boundIndex))
                {
                    if (boundIndex < 0 || boundIndex >= arrayCount)
                    {
                        error(expr->loc, "array index " + std::to_string(boundIndex) +
                              " out of bounds for '" + arrayName + "[" +
                              std::to_string(arrayCount) + "]'");
                        return InvalidIRValue;
                    }
                    const IRValueID folded = foldStaticConstArrayElement(*global, boundIndex);
                    if (folded != InvalidIRValue) return folded;
                    error(expr->loc, "cannot fold static const array element (static-array-loop)");
                    return InvalidIRValue;
                }
                // A FLOAT or HALF index is int(index): the reference emits
                // the same address-register load for u[idx] and u[int(idx)]
                // with a float idx (float-array-index-conversion).  The cast is what the
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
            inst->uniformSource = arrayParam ? arrayParam->valueId : global->valueId;
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
    bool constantIndex = extractIntScalar(*currentFunction_, indexValue, constIdx);
    const IRTypeInfo aggregateType = getExprType(expr->array.get());
    auto* constantAggregate = dynamic_cast<IRConstant*>(currentFunction_->getValue(arrayValue));
    // Instruction results are not entries in the function's constant/value
    // table. The expression type also covers varying loads and local temps.
    if ((aggregateType.isVector() ||
        (constantAggregate && aggregateType.isMatrix())) && !constantIndex)
    {
        auto* index = dynamic_cast<IRConstant*>(currentFunction_->getValue(indexValue));
        std::vector<float> components;
        if (index && index->type.isScalar() &&
            extractFloatComponents(*currentFunction_, indexValue, components) &&
            components.size() == 1)
        {
            // The reference truncates the evaluated index once, for both
            // matrix rows and vector lanes: [1.9] selects 1, [-0.9] selects
            // 0, [1.7*1.2] selects 2. Check before integer conversion,
            // including nonfinite/overflowing values. Keep the aggregate's
            // payload untouched: integer lane extraction below stays exact.
            // Runtime vector operands use this same selector rule; matrix
            // normalization remains limited to constant matrix operands.
            const double element = std::trunc(static_cast<double>(components[0]));
            const int count = aggregateType.isMatrix()
                ? aggregateType.matrixRows : aggregateType.vectorSize;
            if (!std::isfinite(element) || element < 0 || element >= count)
            {
                error(expr->index->loc, aggregateType.isMatrix()
                    ? "matrix row index out of bounds" : "vector index out of bounds");
                return InvalidIRValue;
            }
            constIdx = static_cast<int32_t>(element);
            constantIndex = true;
            // VecExtract consumes indexValue, not constIdx. A float operand
            // would otherwise survive this check and still lower as lane 0.
            if (aggregateType.isVector())
                indexValue = createConstant(constIdx);
        }
    }
    // A RUN-TIME index into a VECTOR (not an array, not a matrix row) is
    // refused, as the reference does (C1011 "cannot index a non-array value",
    // measured on sce-cgc 475 2026-09-25 for float, int and varying indices,
    // both profiles, and for m[0][s]; m[s] and a[s] are accepted there).
    // Accepting it lowered the VecExtract's selector as lane 0, so t[s]
    // silently returned t.x whatever s was.
    if (aggregateType.isVector() && !constantIndex)
    {
        error(expr->loc, "cannot index a non-array value with a run-time index "
                         "(the reference refuses this too: C1011)");
        return InvalidIRValue;
    }
    if (constantIndex)
    {
        if (aggregateType.isMatrix())
        {
            const IRValueID row = constructedMatrixRow(arrayValue, constIdx, resultType);
            if (row != InvalidIRValue) return row;
        }
        if (constantAggregate && constantAggregate->type.isMatrix())
        {
            // Matrix constants are flat row-major payloads. M[r] selects a
            // complete vector, whereas the scalar fallback below selects one
            // component. Check signed bounds before converting to size_t.
            const int rows = constantAggregate->type.matrixRows;
            const int cols = constantAggregate->type.matrixCols;
            if (constIdx < 0 || constIdx >= rows)
            {
                error(expr->index->loc, "matrix row index out of bounds");
                return InvalidIRValue;
            }
            std::vector<float> components;
            if (cols <= 0 || resultType.isMatrix() ||
                resultType.componentCount() != cols ||
                !extractFloatComponents(*currentFunction_, arrayValue, components) ||
                components.size() != static_cast<size_t>(rows * cols) ||
                (!constantAggregate->intValues.empty() && constantAggregate->intValues.size() != components.size()))
            {
                error(expr->loc, "constant matrix row has an inconsistent payload");
                return InvalidIRValue;
            }
            const size_t begin = static_cast<size_t>(constIdx * cols);
            std::vector<float> row(components.begin() + begin,
                                   components.begin() + begin + cols);
            std::vector<int64_t> integers;
            if (!constantAggregate->intValues.empty())
                integers.assign(constantAggregate->intValues.begin() + begin,
                                constantAggregate->intValues.begin() + begin + cols);
            return createConstant(resultType, row, integers);
        }
    }
    if (constantIndex && constIdx >= 0)
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
    condValue = normalizeCondition(expr->condition.get(), condValue);
    IRValueID thenValue = buildExpr(expr->thenExpr.get());
    IRValueID elseValue = buildExpr(expr->elseExpr.get());

    IRTypeInfo resultType = getExprType(expr);

    // Match the fragment if-join's no-write path for a local declaration,
    // e.g. `float4 q; q = c ? value : q`. The reference emits the same
    // program as `if (c) q = value`. Do not turn arbitrary unresolved ids,
    // globals, parameters, or two undefined arms into initialized values.
    // Vertex no-write behavior remains unmeasured and keeps its refusal.
    if (module_->shaderStage == ShaderStage::Fragment &&
        (resultType.isScalar() || resultType.isVector()))
    {
        const auto isUnwrittenLocal = [&](IRValueID id) {
            if (id == InvalidIRValue || id >= IRModule::kGlobalIdBase)
                return false;
            for (const auto& binding : declToValue_)
            {
                if (binding.second != id || binding.first->kind != DeclKind::Variable)
                    continue;
                const auto* declaration = static_cast<const VarDecl*>(binding.first);
                if (!declaration->initializer) return true;
            }
            return false;
        };
        const bool thenUnwritten = isUnwrittenLocal(thenValue);
        const bool elseUnwritten = isUnwrittenLocal(elseValue);
        if (thenUnwritten != elseUnwritten &&
            thenValue != InvalidIRValue && elseValue != InvalidIRValue)
        {
            const int count = std::max(1, resultType.componentCount());
            const auto zero = createConstant(resultType,
                std::vector<float>(static_cast<size_t>(count), 0.0f),
                std::vector<int64_t>(static_cast<size_t>(count), 0));
            (thenUnwritten ? thenValue : elseValue) = zero;
        }
    }

    auto inst = std::make_unique<IRInstruction>(IROp::Select,
        currentFunction_->allocateValueId(), resultType);
    inst->addOperand(condValue);
    inst->addOperand(thenValue);
    inst->addOperand(elseValue);
    currentBlock_->addInstruction(std::move(inst));

    return currentFunction_->nextValueId - 1;
}

IRValueID IRBuilder::emitVectorNarrowing(const IRTypeInfo& sourceType,
                                        const IRTypeInfo& targetType,
                                        IRValueID operandValue,
                                        SourceLocation loc,
                                        std::optional<BaseType> baseTypeOverride)
{
    const int tgtWidth = targetType.isVector() ? targetType.vectorSize : (targetType.isScalar() ? 1 : 0);
    if (tgtWidth <= 0 || tgtWidth > 4)
        return InvalidIRValue;

    if ((targetType.isVector() ? targetType.elementType : targetType.baseType) == IRType::Bool &&
        sourceType.elementType != IRType::Bool)
    {
        IRTypeInfo numericType = targetType;
        if (tgtWidth == 1) numericType.baseType = sourceType.elementType;
        numericType.elementType = sourceType.elementType;
        IRValueID narrowed = emitVectorNarrowing(sourceType, numericType, operandValue, loc);
        return emitNumericToBool(numericType, targetType, narrowed, loc);
    }

    BaseType targetBase = BaseType::Float;
    if (baseTypeOverride.has_value())
    {
        targetBase = *baseTypeOverride;
    }
    else
    {
        switch (targetType.elementType)
        {
        case IRType::Bool:    targetBase = BaseType::Bool; break;
        case IRType::Int32:   targetBase = BaseType::Int; break;
        case IRType::UInt32:  targetBase = BaseType::UInt; break;
        case IRType::Float16: targetBase = BaseType::Half; break;
        case IRType::Float32: targetBase = BaseType::Float; break;
        default: targetBase = BaseType::Float; break;
        }
    }

    // Try constant folding first
    IRValue* v = currentFunction_->getValue(operandValue);
    auto* c = v ? dynamic_cast<IRConstant*>(v) : nullptr;
    std::vector<float> objComps;
    if (c && extractFloatComponents(*currentFunction_, operandValue, objComps) &&
        objComps.size() >= static_cast<size_t>(tgtWidth))
    {
        std::vector<ConstEvalScalar> rawComponents;
        rawComponents.reserve(tgtWidth);
        for (size_t i = 0; i < static_cast<size_t>(tgtWidth); ++i)
        {
            if (!c->intValues.empty() && i < c->intValues.size())
            {
                if (c->type.elementType == IRType::Int32)
                    rawComponents.push_back(ConstEvalScalar::fromInt(c->intValues[i]));
                else if (c->type.elementType == IRType::UInt32)
                    rawComponents.push_back(ConstEvalScalar::fromUInt(static_cast<uint64_t>(c->intValues[i])));
                else if (c->type.elementType == IRType::Bool)
                    rawComponents.push_back(ConstEvalScalar::fromBool(c->intValues[i] != 0));
                else
                    rawComponents.push_back(ConstEvalScalar::fromFloat(objComps[i]));
            }
            else
            {
                rawComponents.push_back(ConstEvalScalar::fromFloat(objComps[i]));
            }
        }

        const bool isIntOrBoolTarget = (targetBase == BaseType::Int ||
                                        targetBase == BaseType::UInt ||
                                        targetBase == BaseType::Bool ||
                                        targetBase == BaseType::Short ||
                                        targetBase == BaseType::UShort ||
                                        targetBase == BaseType::Char ||
                                        targetBase == BaseType::UChar);

        std::vector<float> all;
        std::vector<int64_t> allInts;
        all.reserve(tgtWidth);
        if (isIntOrBoolTarget)
            allInts.reserve(tgtWidth);

        bool convertOk = true;
        for (size_t i = 0; i < static_cast<size_t>(tgtWidth); ++i)
        {
            ConstEvalScalar converted;
            if (!convertScalar(rawComponents[i], targetBase, converted))
            {
                convertOk = false;
                break;
            }
            all.push_back(static_cast<float>(converted.asDouble()));
            if (isIntOrBoolTarget)
                allInts.push_back(converted.asInt64());
        }

        if (convertOk)
        {
            if (tgtWidth == 1)
            {
                if (isIntOrBoolTarget && !allInts.empty())
                {
                    switch (targetType.baseType)
                    {
                    case IRType::Int32:  return createConstant(static_cast<int32_t>(allInts[0]));
                    case IRType::UInt32: return createConstant(static_cast<uint32_t>(allInts[0]));
                    case IRType::Bool:   return createConstant(allInts[0] != 0);
                    default: break;
                    }
                }
                return createConstant(targetType, all[0]);
            }
            return createConstant(targetType, all, allInts);
        }
    }

    static const char* const kNarrowSwizzles[] = { "", "x", "xy", "xyz", "xyzw" };
    IRTypeInfo narrowType;
    narrowType.elementType = sourceType.elementType;
    narrowType.vectorSize = tgtWidth;
    narrowType.matrixRows = 0;
    narrowType.matrixCols = 0;
    narrowType.arraySize = 0;
    switch (tgtWidth)
    {
    case 1: narrowType.baseType = sourceType.elementType; break;
    case 2: narrowType.baseType = IRType::Vec2; break;
    case 3: narrowType.baseType = IRType::Vec3; break;
    case 4: narrowType.baseType = IRType::Vec4; break;
    default: narrowType.baseType = IRType::Vec4; break;
    }

    const IRValueID shuffleId = currentFunction_->allocateValueId();
    auto shuffleInst = std::make_unique<IRInstruction>(
        IROp::VecShuffle, shuffleId, narrowType);
    shuffleInst->addOperand(operandValue);
    shuffleInst->swizzleMask = IRUtils::encodeSwizzle(kNarrowSwizzles[tgtWidth]);
    shuffleInst->loc = loc;
    currentBlock_->addInstruction(std::move(shuffleInst));
    identityPrefixSwizzleBase_[shuffleId] = operandValue;

    if (sourceType.elementType == targetType.elementType)
        return shuffleId;

    IROp convOp = IROp::Bitcast;
    const IRType srcElem = sourceType.elementType;
    const IRType dstElem = targetType.elementType;

    if ((srcElem == IRType::Int32 || srcElem == IRType::UInt32 || srcElem == IRType::Bool) &&
        (dstElem == IRType::Float32 || dstElem == IRType::Float16))
    {
        convOp = IROp::IntToFloat;
    }
    else if ((srcElem == IRType::Float32 || srcElem == IRType::Float16) &&
             (dstElem == IRType::Int32 || dstElem == IRType::UInt32 || dstElem == IRType::Bool))
    {
        convOp = IROp::FloatToInt;
    }
    else if (srcElem == IRType::Float32 && dstElem == IRType::Float16)
    {
        convOp = IROp::FloatToHalf;
    }
    else if (srcElem == IRType::Float16 && dstElem == IRType::Float32)
    {
        convOp = IROp::HalfToFloat;
    }

    if (IRValueID folded = tryFoldUnaryOp(convOp, targetType, shuffleId);
        folded != InvalidIRValue)
    {
        return folded;
    }
    return emitUnaryOp(convOp, targetType, shuffleId);
}

IRValueID IRBuilder::emitMatrixNarrowing(const IRTypeInfo& sourceType,
                                        const IRTypeInfo& targetType,
                                        IRValueID operandValue,
                                        SourceLocation loc)
{
    static const char* const kNarrowSwizzles[] = { "", "x", "xy", "xyz", "xyzw" };
    const int r = targetType.matrixRows;
    const int c = targetType.matrixCols;
    const int C = sourceType.matrixCols;

    std::vector<IRValueID> rowValues;
    rowValues.reserve(r);

    for (int i = 0; i < r; ++i)
    {
        IRTypeInfo srcRowType;
        srcRowType.elementType = sourceType.elementType;
        srcRowType.vectorSize = C;
        srcRowType.matrixRows = 0;
        srcRowType.matrixCols = 0;
        srcRowType.arraySize = 0;
        switch (C)
        {
        case 1: srcRowType.baseType = sourceType.elementType; break;
        case 2: srcRowType.baseType = IRType::Vec2; break;
        case 3: srcRowType.baseType = IRType::Vec3; break;
        case 4: srcRowType.baseType = IRType::Vec4; break;
        default: srcRowType.baseType = IRType::Vec4; break;
        }

        const IRValueID constIdx = createConstant(static_cast<int32_t>(i));
        const IRValueID extractId = currentFunction_->allocateValueId();
        auto extractInst = std::make_unique<IRInstruction>(
            IROp::VecExtract, extractId, srcRowType);
        extractInst->addOperand(operandValue);
        extractInst->addOperand(constIdx);
        extractInst->loc = loc;
        currentBlock_->addInstruction(std::move(extractInst));
        IRValueID rowVal = extractId;

        if (c < C)
        {
            IRTypeInfo narrowRowType;
            narrowRowType.elementType = sourceType.elementType;
            narrowRowType.vectorSize = c;
            narrowRowType.matrixRows = 0;
            narrowRowType.matrixCols = 0;
            narrowRowType.arraySize = 0;
            switch (c)
            {
            case 1: narrowRowType.baseType = sourceType.elementType; break;
            case 2: narrowRowType.baseType = IRType::Vec2; break;
            case 3: narrowRowType.baseType = IRType::Vec3; break;
            case 4: narrowRowType.baseType = IRType::Vec4; break;
            default: narrowRowType.baseType = IRType::Vec4; break;
            }

            const IRValueID shuffleId = currentFunction_->allocateValueId();
            auto shuffleInst = std::make_unique<IRInstruction>(
                IROp::VecShuffle, shuffleId, narrowRowType);
            shuffleInst->addOperand(rowVal);
            shuffleInst->swizzleMask = IRUtils::encodeSwizzle(kNarrowSwizzles[c]);
            shuffleInst->loc = loc;
            currentBlock_->addInstruction(std::move(shuffleInst));
            identityPrefixSwizzleBase_[shuffleId] = rowVal;
            rowVal = shuffleId;
        }

        if (sourceType.elementType != targetType.elementType)
        {
            IRTypeInfo dstRowType;
            dstRowType.elementType = targetType.elementType;
            dstRowType.vectorSize = c;
            dstRowType.matrixRows = 0;
            dstRowType.matrixCols = 0;
            dstRowType.arraySize = 0;
            switch (c)
            {
            case 1: dstRowType.baseType = targetType.elementType; break;
            case 2: dstRowType.baseType = IRType::Vec2; break;
            case 3: dstRowType.baseType = IRType::Vec3; break;
            case 4: dstRowType.baseType = IRType::Vec4; break;
            default: dstRowType.baseType = IRType::Vec4; break;
            }

            if (targetType.elementType == IRType::Bool)
            {
                // A numeric bool cast tests nonzero, including fractional values.
                // Describe the narrowed row rather than its original width.
                IRTypeInfo sourceRowType = dstRowType;
                sourceRowType.elementType = sourceType.elementType;
                if (c == 1) sourceRowType.baseType = sourceType.elementType;
                rowVal = emitNumericToBool(sourceRowType, dstRowType, rowVal, loc);
                if (rowVal == InvalidIRValue) return InvalidIRValue;
                rowValues.push_back(rowVal);
                continue;
            }

            IROp convOp = IROp::Bitcast;
            if (sourceType.elementType == IRType::Float32 && targetType.elementType == IRType::Float16)
                convOp = IROp::FloatToHalf;
            else if (sourceType.elementType == IRType::Float16 && targetType.elementType == IRType::Float32)
                convOp = IROp::HalfToFloat;
            else if ((sourceType.elementType == IRType::Int32 || sourceType.elementType == IRType::UInt32 || sourceType.elementType == IRType::Bool) &&
                     (targetType.elementType == IRType::Float32 || targetType.elementType == IRType::Float16))
                convOp = IROp::IntToFloat;
            else if ((sourceType.elementType == IRType::Float32 || sourceType.elementType == IRType::Float16) &&
                     (targetType.elementType == IRType::Int32 || targetType.elementType == IRType::UInt32))
                convOp = IROp::FloatToInt;

            if (convOp != IROp::Bitcast)
            {
                if (IRValueID folded = tryFoldUnaryOp(convOp, dstRowType, rowVal);
                    folded != InvalidIRValue)
                {
                    rowVal = folded;
                }
                else
                {
                    rowVal = emitUnaryOp(convOp, dstRowType, rowVal);
                }
            }
        }

        rowValues.push_back(rowVal);
    }

    const IRValueID matId = currentFunction_->allocateValueId();
    auto matInst = std::make_unique<IRInstruction>(
        IROp::MatConstruct, matId, targetType);
    for (IRValueID rVal : rowValues)
        matInst->addOperand(rVal);
    matInst->loc = loc;
    currentBlock_->addInstruction(std::move(matInst));
    return matId;
}

IRValueID IRBuilder::buildCastExpr(CastExpr* expr)
{
    IRValueID operandValue = buildExpr(expr->operand.get());
    if (operandValue == InvalidIRValue)
        return InvalidIRValue;

    IRTypeInfo targetType = getIRType(expr->targetType.get());
    IRTypeInfo sourceType = getExprType(expr->operand.get());

    // Same base and element type is not the same SHAPE for matrices: every
    // non-square matrix shares the Mat4x4 base tag, so (float3x4)float4x4
    // must still narrow (rectangular-matrices).
    if (sourceType.baseType == targetType.baseType &&
        sourceType.elementType == targetType.elementType &&
        sourceType.matrixRows == targetType.matrixRows &&
        sourceType.matrixCols == targetType.matrixCols &&
        sourceType.vectorSize == targetType.vectorSize)
    {
        return operandValue;
    }

    // Matrix cast handling
    if (sourceType.isMatrix() || targetType.isMatrix())
    {
        if (sourceType.isMatrix() && (targetType.isScalar() || targetType.isVector()))
            return emitMatrixComponents(sourceType, targetType, operandValue, expr->loc);
        if (targetType.isMatrix() && targetType.elementType == IRType::Float32 && sourceType.isScalar() &&
            (sourceType.baseType == IRType::Float32 || sourceType.baseType == IRType::Int32))
            return emitScalarMatrixBroadcast(sourceType, targetType, operandValue, expr->loc);
        if (targetType.isMatrix() && targetType.elementType == IRType::Bool && sourceType.isScalar())
        {
            error(expr->loc, "scalar-to-bool-matrix casts are not implemented; use a bool matrix constructor");
            return InvalidIRValue;
        }
        if (sourceType.isMatrix() && targetType.isMatrix())
        {
            if (targetType.matrixRows > sourceType.matrixRows ||
                targetType.matrixCols > sourceType.matrixCols)
            {
                error(expr->loc, "error C1033: cast not allowed");
                return InvalidIRValue;
            }

            return emitMatrixNarrowing(sourceType, targetType, operandValue, expr->loc);
        }

        // Casting between matrix and vector/non-scalar is refused
        if ((sourceType.isMatrix() && !targetType.isMatrix()) ||
            (!sourceType.isMatrix() && targetType.isMatrix() && !sourceType.isScalar()))
        {
            error(expr->loc, "error C1033: cast not allowed");
            return InvalidIRValue;
        }
    }

    // Vector / scalar cast handling
    const int srcWidth = sourceType.isVector() ? sourceType.vectorSize : (sourceType.isScalar() ? 1 : 0);
    const int tgtWidth = targetType.isVector() ? targetType.vectorSize : (targetType.isScalar() ? 1 : 0);

    if (srcWidth > 0 && tgtWidth > 0 && !sourceType.isMatrix() && !targetType.isMatrix())
    {
        // Vector widening is refused
        if (sourceType.isVector() && tgtWidth > srcWidth)
        {
            error(expr->loc, "error C1033: cast not allowed");
            return InvalidIRValue;
        }

        // Vector narrowing: N -> M (M < N), including vector to scalar (M = 1)
        if (sourceType.isVector() && tgtWidth < srcWidth)
        {
            return emitVectorNarrowing(sourceType, targetType, operandValue, expr->loc,
                expr->targetType ? std::optional<BaseType>(expr->targetType->baseType) : std::nullopt);
        }
    }

    if (targetType.arraySize == 0 && !targetType.isMatrix() &&
        (targetType.isVector() ? targetType.elementType : targetType.baseType) == IRType::Bool)
        return emitNumericToBool(sourceType, targetType, operandValue, expr->loc);

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
    // reference-SDK sweep (half-temporary-allocation).
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
    if (getStructFields(expr->constructedType.get()))
    {
        error(expr->loc, "struct constructor used as a value is not supported (local-struct-initializer)");
        return InvalidIRValue;
    }
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
        if (argValues[0] == InvalidIRValue)
            return InvalidIRValue;

        IRTypeInfo argType = getExprType(expr->arguments[0].get());

        // Matrix single argument constructor
        if (resultType.isMatrix() || argType.isMatrix())
        {
            if (argType.isMatrix() && (resultType.isScalar() || resultType.isVector()))
                return emitMatrixComponents(argType, resultType, argValues[0], expr->loc);
            if (resultType.isMatrix() && resultType.elementType == IRType::Float32 && argType.isScalar() &&
                (argType.baseType == IRType::Float32 || argType.baseType == IRType::Int32))
                return emitScalarMatrixBroadcast(argType, resultType, argValues[0], expr->loc);
            if (resultType.isMatrix() && argType.isMatrix())
            {
                if (resultType.matrixRows > argType.matrixRows ||
                    resultType.matrixCols > argType.matrixCols)
                {
                    error(expr->loc, "error C1033: cast not allowed");
                    return InvalidIRValue;
                }

                if (resultType.matrixRows == argType.matrixRows &&
                    resultType.matrixCols == argType.matrixCols &&
                    resultType.elementType == argType.elementType)
                {
                    return argValues[0];
                }

                return emitMatrixNarrowing(argType, resultType, argValues[0], expr->loc);
            }

            if (resultType.isMatrix() && resultType.elementType == IRType::Bool && argType.isScalar())
            {
                const IRValueID value = emitNumericToBool(argType, IRTypeInfo::Bool(), argValues[0], expr->loc);
                if (value == InvalidIRValue) return InvalidIRValue;
                return emitInstruction(IROp::MatConstruct, resultType,
                    std::vector<IRValueID>(resultType.matrixRows * resultType.matrixCols, value), expr->loc);
            }

            // A vector whose lanes exactly fill the matrix constructs it
            // row-major (measured: float2x2(float4) is accepted, rows a.xy and
            // a.zw).  Only a cast of a different shape is C1033.
            if (!argType.isMatrix() && resultType.isMatrix() && argType.isVector() &&
                argType.vectorSize == resultType.matrixRows * resultType.matrixCols)
            {
                IRTypeInfo laneType = IRTypeInfo::Float();
                laneType.baseType = argType.elementType;
                if (resultType.elementType == IRType::Bool)
                {
                    IRTypeInfo boolType = argType;
                    boolType.elementType = IRType::Bool;
                    argValues[0] = emitNumericToBool(argType, boolType, argValues[0], expr->loc);
                    if (argValues[0] == InvalidIRValue) return InvalidIRValue;
                    laneType = IRTypeInfo::Bool();
                }
                std::vector<IRValueID> lanes;
                for (int k = 0; k < argType.vectorSize; ++k)
                    lanes.push_back(emitInstruction(IROp::VecExtract, laneType,
                        {argValues[0], createConstant(static_cast<int32_t>(k))}, expr->loc));
                return emitInstruction(IROp::MatConstruct, resultType, lanes, expr->loc);
            }
            if ((argType.isMatrix() && !resultType.isMatrix()) ||
                (!argType.isMatrix() && resultType.isMatrix() && !argType.isScalar()))
            {
                error(expr->loc, "error C1033: cast not allowed");
                return InvalidIRValue;
            }
        }

        // Vector / scalar single argument constructor
        const int srcWidth = argType.isVector() ? argType.vectorSize : (argType.isScalar() ? 1 : 0);
        const int tgtWidth = resultType.isVector() ? resultType.vectorSize : (resultType.isScalar() ? 1 : 0);

        if (srcWidth > 0 && tgtWidth > 0 && !resultType.isMatrix() && !argType.isMatrix())
        {
            if (argType.isVector() && tgtWidth > srcWidth)
            {
                error(expr->loc, "error C1033: cast not allowed");
                return InvalidIRValue;
            }

            if (argType.isVector() && tgtWidth < srcWidth)
            {
                return emitVectorNarrowing(argType, resultType, argValues[0], expr->loc,
                    expr->constructedType ? std::optional<BaseType>(expr->constructedType->baseType) : std::nullopt);
            }
        }

        if (argType.componentCount() == resultType.componentCount())
        {
            if (resultType.arraySize == 0 && !resultType.isMatrix() &&
                (resultType.isVector() ? resultType.elementType : resultType.baseType) == IRType::Bool)
                return emitNumericToBool(argType, resultType, argValues[0], expr->loc);
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
        if (resultType.arraySize == 0 && resultType.elementType == IRType::Bool)
        {
            for (size_t i = 0; i < argValues.size(); ++i)
            {
                const IRTypeInfo sourceType = getExprType(expr->arguments[i].get());
                IRTypeInfo boolType = sourceType;
                if (sourceType.isVector()) boolType.elementType = IRType::Bool;
                else boolType = IRTypeInfo::Bool();
                argValues[i] = emitNumericToBool(sourceType, boolType, argValues[i], expr->loc);
            }
        }
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
        if (resultType.elementType == IRType::Bool)
        {
            for (size_t i = 0; i < argValues.size(); ++i)
            {
                const IRTypeInfo sourceType = getExprType(expr->arguments[i].get());
                IRTypeInfo boolType = sourceType;
                if (sourceType.isVector()) boolType.elementType = IRType::Bool;
                else boolType = IRTypeInfo::Bool();
                argValues[i] = emitNumericToBool(sourceType, boolType, argValues[i], expr->loc);
                if (argValues[i] == InvalidIRValue) return InvalidIRValue;
            }
        }
        // Component widths of the arguments (a matrix argument is not
        // flattened here).
        std::vector<int> widths;
        bool anyMatrixArg = false;
        for (size_t i = 0; i < argValues.size() && i < expr->arguments.size(); ++i)
        {
            const IRTypeInfo t = getExprType(expr->arguments[i].get());
            anyMatrixArg = anyMatrixArg || t.isMatrix();
            widths.push_back(t.isMatrix() ? 0 : std::max(1, t.vectorSize));
        }
        const bool rowVectors = widths.size() == static_cast<size_t>(resultType.matrixRows) &&
            std::all_of(widths.begin(), widths.end(), [&](int w) { return w == resultType.matrixCols; });
        int total = 0;
        for (int w : widths) total += w;
        // Mixed vector/scalar arguments fill the matrix in row-major order
        // (measured: float4x4(a, b, a.x .. a.w, b.x .. b.w) has rows a, b,
        // a, b; float4x4 from eight float2s; float2x2(float4)).  Flatten to
        // scalar lanes when the arguments are not exactly the row vectors.
        if (!rowVectors && !anyMatrixArg && widths.size() == argValues.size() &&
            total == resultType.matrixRows * resultType.matrixCols &&
            argValues.size() != static_cast<size_t>(total))
        {
            std::vector<IRValueID> lanes;
            for (size_t i = 0; i < argValues.size(); ++i)
            {
                if (widths[i] == 1) { lanes.push_back(argValues[i]); continue; }
                const IRTypeInfo t = getExprType(expr->arguments[i].get());
                IRTypeInfo laneType = IRTypeInfo::Float();
                laneType.baseType = t.elementType;
                if (resultType.elementType == IRType::Bool) laneType = IRTypeInfo::Bool();
                for (int k = 0; k < widths[i]; ++k)
                    lanes.push_back(emitInstruction(IROp::VecExtract, laneType,
                        {argValues[i], createConstant(static_cast<int32_t>(k))}, expr->loc));
            }
            argValues = std::move(lanes);
        }
        // Verify we have the right number of row/column vectors
        int expectedVectors = resultType.matrixRows; // Assume row-major: N rows of M-component vectors
        if (argValues.size() == (size_t)expectedVectors && rowVectors)
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

// True when building `e` could change state: ++/--, any assignment, or a
// call (a user function may write a global or an out argument).  Used where
// an lvalue's subexpressions would otherwise be built more than once.
static bool exprHasSideEffects(const ExprNode* e)
{
    if (!e) return false;
    switch (e->kind)
    {
    case ExprKind::Unary: {
        const auto* u = static_cast<const UnaryExpr*>(e);
        if (u->op == UnaryOp::PreIncrement || u->op == UnaryOp::PreDecrement ||
            u->op == UnaryOp::PostIncrement || u->op == UnaryOp::PostDecrement)
            return true;
        return exprHasSideEffects(u->operand.get());
    }
    case ExprKind::Binary: {
        const auto* b = static_cast<const BinaryExpr*>(e);
        switch (b->op)
        {
        case BinaryOp::Assign: case BinaryOp::AddAssign: case BinaryOp::SubAssign:
        case BinaryOp::MulAssign: case BinaryOp::DivAssign: case BinaryOp::ModAssign:
            return true;
        default:
            return exprHasSideEffects(b->left.get()) || exprHasSideEffects(b->right.get());
        }
    }
    case ExprKind::Call:
        return true;
    case ExprKind::MemberAccess:
        return exprHasSideEffects(static_cast<const MemberAccessExpr*>(e)->object.get());
    case ExprKind::Index: {
        const auto* i = static_cast<const IndexExpr*>(e);
        return exprHasSideEffects(i->array.get()) || exprHasSideEffects(i->index.get());
    }
    case ExprKind::Ternary: {
        const auto* t = static_cast<const TernaryExpr*>(e);
        return exprHasSideEffects(t->condition.get()) || exprHasSideEffects(t->thenExpr.get()) ||
               exprHasSideEffects(t->elseExpr.get());
    }
    case ExprKind::Cast:
        return exprHasSideEffects(static_cast<const CastExpr*>(e)->operand.get());
    case ExprKind::Constructor: {
        for (const auto& a : static_cast<const ConstructorExpr*>(e)->arguments)
            if (exprHasSideEffects(a.get())) return true;
        return false;
    }
    default:
        return false;
    }
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
            // A FILE-SCOPE vector (implicitly uniform) written through a
            // swizzle - `ToonConfig.xyz = 4;` in mudlord toon - has no binding
            // yet: its current value IS the uniform.  Load it and bind the
            // name, so the read/modify/write below rebinds it and later
            // reads see the written lanes (the reference uses the written 4
            // and leaves ToonConfig unread; we kept reading the uniform -
            // wrong pixels, found by the pixel judge).
            if (ident && fields.empty() && !nameToValue_.count(objectName))
            {
                const IRGlobal* global = module_->findGlobal(objectName);
                if (global && global->storage == StorageQualifier::Uniform &&
                    !global->type.isArray() && global->type.isVector())
                {
                    const IRValueID loaded = buildExpr(memberExpr->object.get());
                    if (loaded != InvalidIRValue) nameToValue_[objectName] = loaded;
                }
            }
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
                if (!fields.empty()) emitEntryStructOutputs(memberExpr->object.get());
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

            // Entry parameter leaves need immediate stores. Local struct
            // returns keep their existing deferred, declaration-order stores.
            emitEntryStructOutputs(target);

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
            emitEntryStructOutputs(target);
            return value;
        }
    }

    // Handle array index assignment
    if (target->kind == ExprKind::Index)
    {
        auto* indexExpr = static_cast<IndexExpr*>(target);
        if (getExprType(indexExpr->array.get()).isArray())
        {
            std::string key;
            int32_t index = 0;
            if (!resolveTrackedArrayElement(indexExpr, key, index)) return InvalidIRValue;
            localArrayValues_.at(key)[static_cast<size_t>(index)] = value;
            return value;
        }
        // A CONSTANT index into a vector or a matrix is a lane or a row:
        // `int4 b; b[2] = x;` is `b.z = x` (4xbrz) and `float4x4 w; w[0] =
        // r;` replaces row 0 (jinc2, lanczos, biquad).  Rebuild the whole
        // value with that part replaced and assign it through the same
        // lvalue path as the aggregate itself, so locals, struct members and
        // outputs all keep their own handling.  The index follows the read
        // rule: an integer exactly, a float constant truncated once; out of
        // range refuses.
        const IRTypeInfo aggregateType = getExprType(indexExpr->array.get());
        if (!aggregateType.isArray() && (aggregateType.isVector() || aggregateType.isMatrix()) &&
            (exprHasSideEffects(indexExpr->array.get()) || exprHasSideEffects(indexExpr->index.get())))
        {
            // This path reads the aggregate and then assigns it through its
            // own lvalue, so the lvalue's subexpressions build TWICE: `a[i++][1]
            // = x` would step i twice (review: codex).  Refuse the
            // side-effecting shapes rather than build them twice.
            error(target->loc, "lane or row store through an lvalue with side effects "
                               "(++, --, an assignment or a call) is not supported; refusing");
            return value;
        }
        if (!aggregateType.isArray() && (aggregateType.isVector() || aggregateType.isMatrix()))
        {
            const IRValueID indexValue = buildExpr(indexExpr->index.get());
            int32_t k = 0;
            bool constant = extractIntScalar(*currentFunction_, indexValue, k);
            if (!constant)
            {
                std::vector<float> components;
                auto* idx = dynamic_cast<IRConstant*>(currentFunction_->getValue(indexValue));
                if (idx && idx->type.isScalar() &&
                    extractFloatComponents(*currentFunction_, indexValue, components) &&
                    components.size() == 1 && std::isfinite(components[0]))
                {
                    const double t = std::trunc(static_cast<double>(components[0]));
                    if (t >= -1.0 && t <= 64.0)
                    {
                        k = static_cast<int32_t>(t);
                        constant = true;
                    }
                }
            }
            if (constant)
            {
                const bool matrix = aggregateType.isMatrix();
                const int count = matrix ? aggregateType.matrixRows : aggregateType.vectorSize;
                if (k < 0 || k >= count)
                {
                    error(indexExpr->index->loc, matrix ? "matrix row index out of bounds"
                                                        : "vector index out of bounds");
                    return value;
                }
                IRTypeInfo partType;
                partType.elementType = aggregateType.elementType;
                partType.matrixRows = 0;
                partType.matrixCols = 0;
                partType.arraySize = 0;
                const int width = matrix ? aggregateType.matrixCols : 1;
                partType.vectorSize = width;
                switch (width)
                {
                case 2: partType.baseType = IRType::Vec2; break;
                case 3: partType.baseType = IRType::Vec3; break;
                case 4: partType.baseType = IRType::Vec4; break;
                default: partType.baseType = aggregateType.elementType; break;
                }
                // The caller hands over the RHS already coerced to the
                // element's type (the value of an assignment expression).
                const IRValueID current = buildExpr(indexExpr->array.get());
                std::vector<IRValueID> parts;
                for (int i = 0; i < count; ++i)
                {
                    if (i == k) { parts.push_back(value); continue; }
                    if (matrix)
                    {
                        const IRValueID row = constructedMatrixRow(current, i, partType);
                        if (row != InvalidIRValue) { parts.push_back(row); continue; }
                    }
                    parts.push_back(emitInstruction(IROp::VecExtract, partType,
                                                    {current, createConstant(static_cast<int32_t>(i))},
                                                    target->loc));
                }
                const IRValueID rebuilt = emitInstruction(matrix ? IROp::MatConstruct : IROp::VecConstruct,
                                                          aggregateType, parts, target->loc);
                buildAssignment(indexExpr->array.get(), rebuilt);
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

// Convert one scalar lane between numeric element types.
IRValueID IRBuilder::emitScalarConversion(const IRTypeInfo& fromType, const IRTypeInfo& targetType,
                                          IRValueID lane, const SourceLocation& loc)
{
    const IRType from = fromType.baseType, to = targetType.baseType;
    if (from == to) return lane;
    if ((from == IRType::Float32 || from == IRType::Float16) &&
        (to == IRType::Int32 || to == IRType::UInt32))
        return emitInstruction(IROp::FloatToInt, targetType, {lane}, loc);
    if (from == IRType::Float32 && to == IRType::Float16)
        return emitInstruction(IROp::FloatToHalf, targetType, {lane}, loc);
    if (from == IRType::Float16 && to == IRType::Float32)
        return emitInstruction(IROp::HalfToFloat, targetType, {lane}, loc);
    if ((from == IRType::Int32 || from == IRType::UInt32) &&
        (to == IRType::Float32 || to == IRType::Float16))
        return emitInstruction(IROp::IntToFloat, targetType, {lane}, loc);
    return lane;
}

// A vector stored into a numeric scalar keeps lane x, converted to the
// scalar's type (measured on the reference; allowsScalarNarrowing).
IRValueID IRBuilder::emitScalarNarrowing(const IRTypeInfo& sourceType, const IRTypeInfo& targetType,
                                         IRValueID value, const SourceLocation& loc)
{
    IRTypeInfo laneType = targetType;
    laneType.baseType = sourceType.elementType;
    const IRValueID lane = emitInstruction(IROp::VecExtract, laneType,
        {value, createConstant(static_cast<int32_t>(0))}, loc);
    return emitScalarConversion(laneType, targetType, lane, loc);
}

// A vector returned from a function declared to return a numeric scalar
// keeps lane x (measured: float f() { return t.zyx; } is t.z).
IRValueID IRBuilder::narrowToScalar(TypeNode* declared, ExprNode* valueExpr, IRValueID value)
{
    if (!declared || !valueExpr || value == InvalidIRValue) return value;
    const IRTypeInfo to = getIRType(declared);
    const IRTypeInfo from = getExprType(valueExpr);
    if (from.isMatrix() && (to.isScalar() || to.isVector()))
        return emitMatrixComponents(from, to, value, valueExpr->loc);
    if (to.isVector() || to.isMatrix() || to.arraySize != 0 || to.baseType == IRType::Void ||
        to.baseType == IRType::Bool || getStructFields(declared))
        return value;
    if (!from.isVector() || from.isMatrix() || from.arraySize != 0)
        return value;
    return emitScalarNarrowing(from, to, value, valueExpr->loc);
}

IRValueID IRBuilder::emitMatrixComponents(const IRTypeInfo& sourceType,
                                         const IRTypeInfo& targetType,
                                         IRValueID value, const SourceLocation& loc)
{
    if (value == InvalidIRValue) return value;
    const int width = targetType.isVector() ? targetType.vectorSize : 1;
    const bool scalar = targetType.isScalar();
    if (sourceType.arraySize != 0 || targetType.arraySize != 0 ||
        !sourceType.isMatrix() || sourceType.elementType != IRType::Float32 ||
        (!scalar && !targetType.isVector()) ||
        (scalar ? targetType.baseType : targetType.elementType) != IRType::Float32 ||
        sourceType.matrixRows < 1 || sourceType.matrixRows > 4 ||
        sourceType.matrixCols < 1 || sourceType.matrixCols > 4 || width < 1 || width > 4 ||
        (!scalar && ((sourceType.matrixRows != 1 && sourceType.matrixCols != 1) ||
                     width > std::max(sourceType.matrixRows, sourceType.matrixCols))))
    {
        error(loc, "matrix component conversion requires a float scalar or a non-widening float vector from a one-dimensional float matrix");
        return InvalidIRValue;
    }
    const IRTypeInfo rowType = sourceType.matrixCols == 1 ? IRTypeInfo::Float() :
        sourceType.matrixCols == 2 ? IRTypeInfo::Float2() :
        sourceType.matrixCols == 3 ? IRTypeInfo::Float3() : IRTypeInfo::Float4();
    std::vector<IRValueID> lanes;
    for (int i = 0; i < width; ++i)
    {
        // Extract rows first: a matrix's VecExtract index denotes a row,
        // while a vector's denotes a lane. Reuse the already evaluated value.
        const int row = sourceType.matrixCols == 1 ? i : 0;
        IRValueID lane = emitInstruction(IROp::VecExtract, rowType,
            {value, createConstant(static_cast<int32_t>(row))}, loc);
        if (sourceType.matrixCols != 1)
            lane = emitInstruction(IROp::VecExtract, IRTypeInfo::Float(),
                {lane, createConstant(static_cast<int32_t>(i))}, loc);
        lanes.push_back(lane);
    }
    return scalar ? lanes.front() : emitInstruction(IROp::VecConstruct, targetType, lanes, loc);
}

IRValueID IRBuilder::emitScalarMatrixBroadcast(const IRTypeInfo& sourceType,
                                               const IRTypeInfo& targetType,
                                               IRValueID value, const SourceLocation& loc)
{
    if (value == InvalidIRValue) return value;
    if (!sourceType.isScalar() || sourceType.arraySize != 0 || targetType.arraySize != 0 ||
        !targetType.isMatrix() || targetType.elementType != IRType::Float32 ||
        targetType.matrixRows < 1 || targetType.matrixRows > 4 ||
        targetType.matrixCols < 1 || targetType.matrixCols > 4 ||
        (sourceType.baseType != IRType::Float32 && sourceType.baseType != IRType::Int32))
    {
        error(loc, "scalar-to-matrix conversion requires a float/int scalar and float matrix (scalar-matrix-broadcast)");
        return InvalidIRValue;
    }
    value = prepareFloatMatrixScalar(sourceType, value, loc);
    if (value == InvalidIRValue) return value;
    return emitInstruction(IROp::MatConstruct, targetType,
        std::vector<IRValueID>(targetType.matrixRows * targetType.matrixCols, value), loc);
}

IRValueID IRBuilder::prepareFloatMatrixScalar(const IRTypeInfo& sourceType,
                                              IRValueID value, const SourceLocation& loc)
{
    if (value == InvalidIRValue) return value;
    if (!sourceType.isScalar() || sourceType.isArray() ||
        (sourceType.baseType != IRType::Float32 && sourceType.baseType != IRType::Int32))
    {
        error(loc, "float matrix arithmetic requires a float/int scalar (scalar-matrix-source-type)");
        return InvalidIRValue;
    }
    // Convert once, then reuse that SSA value. A postincrement argument
    // must not run again for each lane, and a scalar id is not a matrix.
    if (sourceType.baseType == IRType::Int32)
    {
        // Existing implicit scalar integer bindings can retain an
        // untruncated float, even behind an int-typed arithmetic result.
        // Admit proven integer constants and explicit truncation results;
        // do not turn that upstream debt into a newly accepted wrong matrix.
        const auto* constant = dynamic_cast<IRConstant*>(currentFunction_->getValue(value));
        bool provenInteger = constant && constant->type.baseType == IRType::Int32;
        for (const auto& block : currentFunction_->blocks)
            for (const auto& instruction : block->instructions)
                if (instruction->result == value && instruction->op == IROp::FloatToInt &&
                    instruction->resultType.baseType == IRType::Int32)
                    provenInteger = true;
        if (!provenInteger)
        {
            error(loc, "integer matrix broadcast needs an explicit int conversion or integer constant (scalar-matrix-source-type)");
            return InvalidIRValue;
        }
        const IRValueID folded = tryFoldUnaryOp(IROp::IntToFloat, IRTypeInfo::Float(), value);
        value = folded != InvalidIRValue ? folded :
            emitScalarConversion(sourceType, IRTypeInfo::Float(), value, loc);
    }
    return value;
}

IRValueID IRBuilder::coerceAssignmentValue(ExprNode* target, IRValueID value)
{
    if (value == InvalidIRValue || !target || !currentFunction_)
        return value;

    const IRTypeInfo targetType = getExprType(target);
    IRValue* srcValue = currentFunction_->getValue(value);
    if (targetType.arraySize == 0 && (targetType.isScalar() || targetType.isVector()))
    {
        IRTypeInfo sourceType = srcValue ? srcValue->type : IRTypeInfo::Void();
        for (const auto& parameter : currentFunction_->parameters)
            if (parameter.valueId == value) sourceType = parameter.type;
        for (const auto& block : currentFunction_->blocks)
            for (const auto& instruction : block->instructions)
                if (instruction->result == value) sourceType = instruction->resultType;
        if (sourceType.isMatrix())
            return emitMatrixComponents(sourceType, targetType, value, target->loc);
    }
    if (targetType.arraySize == 0 && !targetType.isMatrix() &&
        (targetType.isVector() ? targetType.elementType : targetType.baseType) == IRType::Bool)
    {
        // The value map holds constants; inputs and instruction results have
        // their types on parameters and instructions instead.
        IRTypeInfo sourceType = srcValue ? srcValue->type : IRTypeInfo::Void();
        for (const auto& parameter : currentFunction_->parameters)
            if (parameter.valueId == value) sourceType = parameter.type;
        for (const auto& block : currentFunction_->blocks)
            for (const auto& instruction : block->instructions)
                if (instruction->result == value) sourceType = instruction->resultType;
        if (sourceType.baseType != IRType::Void && sourceType.arraySize == 0 && !sourceType.isMatrix())
        {
            if (sourceType.isVector() && sourceType.componentCount() > targetType.componentCount())
                return emitVectorNarrowing(sourceType, targetType, value, target->loc);
            return emitNumericToBool(sourceType, targetType, value, target->loc);
        }
    }
    // A vector into a numeric scalar keeps lane x, converted to the scalar's
    // type (allowsScalarNarrowing in the semantic pass; measured).  The
    // source's type is found the way the bool path above finds it: the value
    // map holds constants only.
    if (targetType.arraySize == 0 && !targetType.isMatrix() && !targetType.isVector())
    {
        IRTypeInfo sourceType = srcValue ? srcValue->type : IRTypeInfo::Void();
        for (const auto& parameter : currentFunction_->parameters)
            if (parameter.valueId == value) sourceType = parameter.type;
        for (const auto& block : currentFunction_->blocks)
            for (const auto& instruction : block->instructions)
                if (instruction->result == value) sourceType = instruction->resultType;
        if (sourceType.isVector() && sourceType.arraySize == 0 && !sourceType.isMatrix())
            return emitScalarNarrowing(sourceType, targetType, value, target->loc);
    }
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

IRValueID IRBuilder::normalizeCondition(ExprNode* expr, IRValueID value)
{
    const IRTypeInfo sourceType = getExprType(expr);
    if (sourceType.arraySize == 0 && !sourceType.isMatrix() &&
        (sourceType.isVector() ? sourceType.elementType : sourceType.baseType) == IRType::Bool)
        return value;
    IRTypeInfo boolType = sourceType;
    if (sourceType.isVector()) boolType.elementType = IRType::Bool;
    else boolType = IRTypeInfo::Bool();
    // Reuse the conversion's eager constants and typed zero. A raw float is
    // neither a blend weight nor 1-x when used as a logical condition.
    return emitNumericToBool(sourceType, boolType, value, expr->loc);
}

IRValueID IRBuilder::emitNumericToBool(const IRTypeInfo& sourceType,
                                      const IRTypeInfo& targetType,
                                      IRValueID operand, const SourceLocation& loc)
{
    if (operand == InvalidIRValue) return operand;
    if (sourceType.arraySize != 0 || targetType.arraySize != 0 ||
        sourceType.isMatrix() || targetType.isMatrix())
    {
        error(loc, "numeric-to-bool matrix/array conversion is not implemented");
        return InvalidIRValue;
    }
    const IRType element = sourceType.isVector() ? sourceType.elementType : sourceType.baseType;
    if (element != IRType::Bool && element != IRType::Float32 &&
        element != IRType::Float16 && element != IRType::Int32 && element != IRType::UInt32)
    {
        error(loc, "numeric-to-bool conversion requires a numeric scalar or vector");
        return InvalidIRValue;
    }
    const int width = sourceType.isVector() ? sourceType.vectorSize : 1;
    const int targetWidth = targetType.isVector() ? targetType.vectorSize : 1;
    if (width != targetWidth && width != 1)
    {
        error(loc, "numeric-to-bool conversion requires matching vector widths");
        return InvalidIRValue;
    }
    // Preserve eager constants, including vectors and splats: later indexing
    // and loop recognition need their values before optimization passes run.
    auto* constant = dynamic_cast<IRConstant*>(currentFunction_->getValue(operand));
    if (constant)
    {
        std::vector<int64_t> lanes;
        if (auto* raw = std::get_if<bool>(&constant->value)) lanes = {*raw ? 1 : 0};
        else if (auto* raw = std::get_if<float>(&constant->value)) lanes = {*raw != 0.0f ? 1 : 0};
        else if (auto* raw = std::get_if<int32_t>(&constant->value)) lanes = {*raw != 0 ? 1 : 0};
        else if (auto* raw = std::get_if<uint32_t>(&constant->value)) lanes = {*raw != 0 ? 1 : 0};
        else if (auto* raw = std::get_if<std::vector<float>>(&constant->value))
        {
            for (size_t i = 0; i < raw->size(); ++i)
            {
                const bool nonzero = i < constant->intValues.size()
                    ? constant->intValues[i] != 0 : (*raw)[i] != 0.0f;
                lanes.push_back(nonzero ? 1 : 0);
            }
        }
        if (lanes.size() == static_cast<size_t>(width))
        {
            if (targetWidth == 1) return createConstant(lanes[0] != 0);
            if (width == 1) lanes.resize(targetWidth, lanes[0]);
            std::vector<float> values(lanes.begin(), lanes.end());
            return createConstant(targetType, values, lanes);
        }
    }
    IRValueID value = operand;
    if (element != IRType::Bool)
    {
        IRValueID zero;
        if (element == IRType::UInt32) zero = createConstant(uint32_t(0));
        else if (sourceType.isVector())
        {
            const bool integer = element == IRType::Int32 || element == IRType::UInt32;
            zero = createConstant(sourceType, std::vector<float>(width, 0.0f),
                                  integer ? std::vector<int64_t>(width, 0) : std::vector<int64_t>());
        }
        else if (element == IRType::Int32) zero = createConstant(int32_t(0));
        else zero = createConstant(sourceType, 0.0f);
        IRTypeInfo compareType = width == targetWidth ? targetType : IRTypeInfo::Bool();
        // CmpNe compares every lane, including negative and fractional values.
        // Keep the already-evaluated operand: repeating the source expression
        // here would duplicate side effects and texture fetches.
        value = emitBinaryOp(IROp::CmpNe, compareType, operand, zero, loc);
    }
    if (width == targetWidth) return value;
    return emitInstruction(IROp::VecConstruct, targetType,
                           std::vector<IRValueID>(targetWidth, value), loc);
}

IRValueID IRBuilder::constructedMatrixRow(IRValueID matrix, int row,
                                          const IRTypeInfo& rowType) const
{
    // Rebuilding a matrix after w[k]=value must not keep an artificial read
    // of an overwritten row. Forward the already-evaluated row operand of a
    // row-form constructor; DCE can then remove the obsolete constructions.
    // An actually unwritten row is still its unresolved original extract,
    // never an invented zero. Scalar-form constructors use the normal path.
    if (row < 0 || !rowType.isVector() || rowType.arraySize != 0 ||
        rowType.elementType != IRType::Float32)
        return InvalidIRValue;
    for (const auto& block : currentFunction_->blocks)
        for (const auto& instruction : block->instructions)
        {
            if (instruction->result != matrix) continue;
            const IRTypeInfo& type = instruction->resultType;
            if (instruction->op != IROp::MatConstruct || !type.isMatrix() ||
                type.arraySize != 0 || type.elementType != IRType::Float32 ||
                type.matrixCols != rowType.vectorSize || row >= type.matrixRows ||
                instruction->operands.size() != static_cast<size_t>(type.matrixRows))
                return InvalidIRValue;
            const IRValueID selected = instruction->operands[static_cast<size_t>(row)];
            IRTypeInfo selectedType = IRTypeInfo::Void();
            if (const IRValue* value = currentFunction_->getValue(selected))
                selectedType = value->type;
            for (const IRParameter& parameter : currentFunction_->parameters)
                if (parameter.valueId == selected) selectedType = parameter.type;
            for (const auto& sourceBlock : currentFunction_->blocks)
                for (const auto& source : sourceBlock->instructions)
                    if (source->result == selected) selectedType = source->resultType;
            if (!selectedType.isVector() || selectedType.arraySize != 0 ||
                selectedType.vectorSize != rowType.vectorSize ||
                selectedType.elementType != rowType.elementType)
                return InvalidIRValue;
            return selected;
        }
    return InvalidIRValue;
}

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
            inst->inferredSemantic = field.semantic.inferred;
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
        {"lit", IROp::Lit},
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
        {"transpose", IROp::Transpose},
        {"tex1D", IROp::TexSample},
        {"tex2D", IROp::TexSample},
        {"tex3D", IROp::TexSample},
        {"texCUBE", IROp::TexSample},
        {"texRECT", IROp::TexSample},
        {"tex2Dbias", IROp::TexSampleBias},
        {"tex2Dlod", IROp::TexSampleLod},
        {"tex2Dproj", IROp::TexSampleProj},
        {"texDepth2D", IROp::TexSample},
        {"texDepth2D_precise", IROp::TexSample},
        {"f1tex1D", IROp::TexSample}, {"f2tex1D", IROp::TexSample}, {"f3tex1D", IROp::TexSample}, {"f4tex1D", IROp::TexSample},
        {"h1tex1D", IROp::TexSample}, {"h2tex1D", IROp::TexSample}, {"h3tex1D", IROp::TexSample}, {"h4tex1D", IROp::TexSample},
        {"f1tex2D", IROp::TexSample}, {"f2tex2D", IROp::TexSample}, {"f3tex2D", IROp::TexSample}, {"f4tex2D", IROp::TexSample},
        {"h1tex2D", IROp::TexSample}, {"h2tex2D", IROp::TexSample}, {"h3tex2D", IROp::TexSample}, {"h4tex2D", IROp::TexSample},
        {"f1tex3D", IROp::TexSample}, {"f2tex3D", IROp::TexSample}, {"f3tex3D", IROp::TexSample}, {"f4tex3D", IROp::TexSample},
        {"h1tex3D", IROp::TexSample}, {"h2tex3D", IROp::TexSample}, {"h3tex3D", IROp::TexSample}, {"h4tex3D", IROp::TexSample},
        {"f1texCUBE", IROp::TexSample}, {"f2texCUBE", IROp::TexSample}, {"f3texCUBE", IROp::TexSample}, {"f4texCUBE", IROp::TexSample},
        {"h1texCUBE", IROp::TexSample}, {"h2texCUBE", IROp::TexSample}, {"h3texCUBE", IROp::TexSample}, {"h4texCUBE", IROp::TexSample},
        {"f1texRECT", IROp::TexSample}, {"f2texRECT", IROp::TexSample}, {"f3texRECT", IROp::TexSample}, {"f4texRECT", IROp::TexSample},
        {"h1texRECT", IROp::TexSample}, {"h2texRECT", IROp::TexSample}, {"h3texRECT", IROp::TexSample}, {"h4texRECT", IROp::TexSample},
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
