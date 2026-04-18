#include "symbol_table.h"
#include <algorithm>
#include <limits>

// ============================================================================
// Scope Implementation
// ============================================================================

Scope::Scope(Kind k, Scope* parent)
    : scopeKind(k), parentScope(parent)
{
}

bool Scope::addSymbol(std::unique_ptr<Symbol> symbol)
{
    if (!symbol) return false;

    // Check for duplicate in this scope
    if (symbolTable.find(symbol->name) != symbolTable.end())
    {
        return false;  // Already exists
    }

    symbolTable[symbol->name] = std::move(symbol);
    return true;
}

Symbol* Scope::lookupLocal(const std::string& name) const
{
    auto it = symbolTable.find(name);
    if (it != symbolTable.end())
    {
        return it->second.get();
    }
    return nullptr;
}

Symbol* Scope::lookup(const std::string& name) const
{
    // Check this scope first
    Symbol* sym = lookupLocal(name);
    if (sym) return sym;

    // Check parent scopes
    if (parentScope)
    {
        return parentScope->lookup(name);
    }

    return nullptr;
}

bool Scope::addType(const std::string& name, const CgType& type)
{
    if (typeTable.find(name) != typeTable.end())
    {
        return false;  // Already exists
    }
    typeTable[name] = type;
    return true;
}

std::optional<CgType> Scope::lookupType(const std::string& name) const
{
    auto it = typeTable.find(name);
    if (it != typeTable.end())
    {
        return it->second;
    }

    if (parentScope)
    {
        return parentScope->lookupType(name);
    }

    return std::nullopt;
}

// ============================================================================
// SymbolTable Implementation
// ============================================================================

SymbolTable::SymbolTable()
{
    // Create global scope
    pushScope(Scope::Kind::Global);
    globalScopePtr = scopeStack.back().get();
}

SymbolTable::~SymbolTable() = default;

void SymbolTable::pushScope(Scope::Kind kind)
{
    Scope* parent = scopeStack.empty() ? nullptr : scopeStack.back().get();
    scopeStack.push_back(std::make_unique<Scope>(kind, parent));
}

void SymbolTable::popScope()
{
    if (scopeStack.size() > 1)  // Don't pop global scope
    {
        scopeStack.pop_back();
    }
}

bool SymbolTable::addSymbol(std::unique_ptr<Symbol> symbol)
{
    if (!currentScope()) return false;
    return currentScope()->addSymbol(std::move(symbol));
}

bool SymbolTable::addSymbol(SymbolKind kind, const std::string& name, const CgType& type, DeclNode* decl)
{
    auto symbol = std::make_unique<Symbol>(kind, name, type);
    symbol->declaration = decl;
    if (decl)
    {
        symbol->loc = decl->loc;
    }
    return addSymbol(std::move(symbol));
}

Symbol* SymbolTable::lookup(const std::string& name) const
{
    if (!currentScope()) return nullptr;
    return currentScope()->lookup(name);
}

Symbol* SymbolTable::lookupLocal(const std::string& name) const
{
    if (!currentScope()) return nullptr;
    return currentScope()->lookupLocal(name);
}

bool SymbolTable::addType(const std::string& name, const CgType& type)
{
    if (!currentScope()) return false;
    return currentScope()->addType(name, type);
}

std::optional<CgType> SymbolTable::lookupType(const std::string& name) const
{
    if (!currentScope()) return std::nullopt;
    return currentScope()->lookupType(name);
}

bool SymbolTable::addFunction(const std::string& name,
                               const CgType& returnType,
                               const std::vector<CgType>& paramTypes,
                               const std::vector<std::string>& paramNames,
                               FunctionDecl* decl,
                               bool isIntrinsic,
                               const std::string& opcode)
{
    auto symbol = std::make_unique<Symbol>(SymbolKind::Function, name, returnType);
    symbol->parameterTypes = paramTypes;
    symbol->parameterNames = paramNames;
    symbol->declaration = decl;
    symbol->isIntrinsic = isIntrinsic;
    symbol->intrinsicOpcode = opcode;
    if (decl)
    {
        symbol->loc = decl->loc;
    }

    // Store the symbol in our persistent storage and get a raw pointer
    Symbol* symPtr = symbol.get();
    allFunctionSymbols.push_back(std::move(symbol));

    // Add to scope (only the first overload goes into the scope for name lookup)
    if (currentScope()->lookupLocal(name) == nullptr)
    {
        // Create a copy for the scope (or we could just use the pointer for lookup)
        auto scopeSymbol = std::make_unique<Symbol>(*symPtr);
        currentScope()->addSymbol(std::move(scopeSymbol));
    }

    // Track all overloads (including the first one)
    functionOverloads[name].push_back(symPtr);

    return true;
}

std::optional<SymbolTable::OverloadCandidate> SymbolTable::resolveOverload(
    const std::string& name,
    const std::vector<CgType>& argumentTypes) const
{
    auto it = functionOverloads.find(name);
    if (it == functionOverloads.end())
    {
        return std::nullopt;
    }

    const std::vector<Symbol*>& overloads = it->second;
    if (overloads.empty())
    {
        return std::nullopt;
    }

    OverloadCandidate best;
    best.symbol = nullptr;
    best.conversionCost = std::numeric_limits<int>::max();
    best.exactMatch = false;

    for (Symbol* sym : overloads)
    {
        // Check argument count
        if (sym->parameterTypes.size() != argumentTypes.size())
        {
            continue;  // Arity mismatch
        }

        // Calculate conversion cost
        int totalCost = 0;
        bool viable = true;
        bool exact = true;

        for (size_t i = 0; i < argumentTypes.size(); ++i)
        {
            const CgType& argType = argumentTypes[i];
            const CgType& paramType = sym->parameterTypes[i];

            if (argType.equals(paramType))
            {
                // Exact match for this parameter
                continue;
            }

            exact = false;

            int cost = TypeConversion::conversionCost(argType, paramType);
            if (cost < 0)
            {
                // No conversion possible
                viable = false;
                break;
            }

            totalCost += cost;
        }

        if (!viable) continue;

        // Check if this is a better match
        if (exact)
        {
            // Exact match beats everything
            if (!best.exactMatch || best.symbol == nullptr)
            {
                best.symbol = sym;
                best.conversionCost = 0;
                best.exactMatch = true;
            }
        }
        else if (!best.exactMatch && totalCost < best.conversionCost)
        {
            best.symbol = sym;
            best.conversionCost = totalCost;
            best.exactMatch = false;
        }
    }

    if (best.symbol)
    {
        return best;
    }

    return std::nullopt;
}

void SymbolTable::registerBuiltins()
{
    registerBuiltinTypes();
    registerBuiltinFunctions();
}

void SymbolTable::registerBuiltinTypes()
{
    // Register basic types
    addType("void", CgType::Void());
    addType("bool", CgType::Bool());
    addType("int", CgType::Int());
    addType("uint", CgType::UInt());
    addType("float", CgType::Float());
    addType("half", CgType::Half());
    addType("fixed", CgType::Fixed());
    addType("char", CgType::Char());
    addType("uchar", CgType::UChar());
    addType("short", CgType::Short());
    addType("ushort", CgType::UShort());

    // Vector types
    addType("float2", CgType::Float2());
    addType("float3", CgType::Float3());
    addType("float4", CgType::Float4());
    addType("int2", CgType::Int2());
    addType("int3", CgType::Int3());
    addType("int4", CgType::Int4());
    addType("half2", CgType::Half2());
    addType("half3", CgType::Half3());
    addType("half4", CgType::Half4());
    addType("bool2", CgType::Bool2());
    addType("bool3", CgType::Bool3());
    addType("bool4", CgType::Bool4());

    // Matrix types
    addType("float2x2", CgType::Float2x2());
    addType("float3x3", CgType::Float3x3());
    addType("float4x4", CgType::Float4x4());

    // Sampler types
    addType("sampler1D", CgType::Sampler1D());
    addType("sampler2D", CgType::Sampler2D());
    addType("sampler3D", CgType::Sampler3D());
    addType("samplerCUBE", CgType::SamplerCube());
    addType("isampler2D", CgType::ISampler2D());
    addType("usampler2D", CgType::USampler2D());
}

void SymbolTable::addScalarVectorOverloads(const std::string& name,
                                            const CgType& scalarResult,
                                            const CgType& scalarParam)
{
    // Scalar version
    addFunction(name, scalarResult, {scalarParam}, {"x"}, nullptr, true);

    // Vector versions
    for (int size = 2; size <= 4; ++size)
    {
        CgType vecParam = CgType::Vec(scalarParam.scalarKind(), size);
        CgType vecResult = CgType::Vec(scalarResult.scalarKind(), size);
        addFunction(name, vecResult, {vecParam}, {"x"}, nullptr, true);
    }
}

void SymbolTable::addMathFunctionOverloads(const std::string& name)
{
    // Add for float, half
    addScalarVectorOverloads(name, CgType::Float(), CgType::Float());
    addScalarVectorOverloads(name, CgType::Half(), CgType::Half());
}

void SymbolTable::registerBuiltinFunctions()
{
    registerMathFunctions();
    registerVectorFunctions();
    registerTextureSymbols();
}

void SymbolTable::registerMathFunctions()
{
    // Trigonometric functions
    addMathFunctionOverloads("sin");
    addMathFunctionOverloads("cos");
    addMathFunctionOverloads("tan");
    addMathFunctionOverloads("asin");
    addMathFunctionOverloads("acos");
    addMathFunctionOverloads("atan");

    // atan2(y, x)
    addFunction("atan2", CgType::Float(), {CgType::Float(), CgType::Float()}, {"y", "x"}, nullptr, true);
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("atan2", vec, {vec, vec}, {"y", "x"}, nullptr, true);
    }

    // Exponential functions
    addMathFunctionOverloads("exp");
    addMathFunctionOverloads("exp2");
    addMathFunctionOverloads("log");
    addMathFunctionOverloads("log2");
    addMathFunctionOverloads("log10");

    // Power functions
    addFunction("pow", CgType::Float(), {CgType::Float(), CgType::Float()}, {"x", "y"}, nullptr, true);
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("pow", vec, {vec, vec}, {"x", "y"}, nullptr, true);
    }

    addMathFunctionOverloads("sqrt");
    addMathFunctionOverloads("rsqrt");

    // Common functions
    addMathFunctionOverloads("abs");
    addMathFunctionOverloads("sign");
    addMathFunctionOverloads("floor");
    addMathFunctionOverloads("ceil");
    addMathFunctionOverloads("frac");
    addMathFunctionOverloads("round");
    addMathFunctionOverloads("trunc");
    addMathFunctionOverloads("saturate");

    // min, max, clamp
    for (ScalarKind sk : {ScalarKind::Float, ScalarKind::Half, ScalarKind::Int, ScalarKind::UInt})
    {
        CgType scalarType;
        switch (sk)
        {
        case ScalarKind::Float: scalarType = CgType::Float(); break;
        case ScalarKind::Half:  scalarType = CgType::Half(); break;
        case ScalarKind::Int:   scalarType = CgType::Int(); break;
        case ScalarKind::UInt:  scalarType = CgType::UInt(); break;
        default: continue;
        }

        addFunction("min", scalarType, {scalarType, scalarType}, {"a", "b"}, nullptr, true);
        addFunction("max", scalarType, {scalarType, scalarType}, {"a", "b"}, nullptr, true);
        addFunction("clamp", scalarType, {scalarType, scalarType, scalarType}, {"x", "minVal", "maxVal"}, nullptr, true);

        for (int size = 2; size <= 4; ++size)
        {
            CgType vec = CgType::Vec(sk, size);
            addFunction("min", vec, {vec, vec}, {"a", "b"}, nullptr, true);
            addFunction("max", vec, {vec, vec}, {"a", "b"}, nullptr, true);
            addFunction("clamp", vec, {vec, vec, vec}, {"x", "minVal", "maxVal"}, nullptr, true);
        }
    }

    // lerp/mix
    addFunction("lerp", CgType::Float(), {CgType::Float(), CgType::Float(), CgType::Float()}, {"a", "b", "t"}, nullptr, true);
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("lerp", vec, {vec, vec, CgType::Float()}, {"a", "b", "t"}, nullptr, true);
        addFunction("lerp", vec, {vec, vec, vec}, {"a", "b", "t"}, nullptr, true);
    }

    // step, smoothstep - float versions
    addFunction("step", CgType::Float(), {CgType::Float(), CgType::Float()}, {"edge", "x"}, nullptr, true);
    addFunction("smoothstep", CgType::Float(), {CgType::Float(), CgType::Float(), CgType::Float()}, {"edge0", "edge1", "x"}, nullptr, true);

    // step, smoothstep - half versions (for fragment shaders using half precision)
    addFunction("step", CgType::Half(), {CgType::Half(), CgType::Half()}, {"edge", "x"}, nullptr, true);
    addFunction("smoothstep", CgType::Half(), {CgType::Half(), CgType::Half(), CgType::Half()}, {"edge0", "edge1", "x"}, nullptr, true);

    for (int size = 2; size <= 4; ++size)
    {
        // Float vector versions
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("step", vec, {vec, vec}, {"edge", "x"}, nullptr, true);
        addFunction("step", vec, {CgType::Float(), vec}, {"edge", "x"}, nullptr, true);
        addFunction("smoothstep", vec, {vec, vec, vec}, {"edge0", "edge1", "x"}, nullptr, true);
        addFunction("smoothstep", vec, {CgType::Float(), CgType::Float(), vec}, {"edge0", "edge1", "x"}, nullptr, true);

        // Half vector versions
        CgType hvec = CgType::Vec(ScalarKind::Half, size);
        addFunction("step", hvec, {hvec, hvec}, {"edge", "x"}, nullptr, true);
        addFunction("step", hvec, {CgType::Half(), hvec}, {"edge", "x"}, nullptr, true);
        addFunction("smoothstep", hvec, {hvec, hvec, hvec}, {"edge0", "edge1", "x"}, nullptr, true);
        addFunction("smoothstep", hvec, {CgType::Half(), CgType::Half(), hvec}, {"edge0", "edge1", "x"}, nullptr, true);
    }

    // fmod
    addFunction("fmod", CgType::Float(), {CgType::Float(), CgType::Float()}, {"x", "y"}, nullptr, true);
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("fmod", vec, {vec, vec}, {"x", "y"}, nullptr, true);
    }

    // isnan, isinf, isfinite
    addFunction("isnan", CgType::Bool(), {CgType::Float()}, {"x"}, nullptr, true);
    addFunction("isinf", CgType::Bool(), {CgType::Float()}, {"x"}, nullptr, true);
    addFunction("isfinite", CgType::Bool(), {CgType::Float()}, {"x"}, nullptr, true);
}

void SymbolTable::registerVectorFunctions()
{
    // dot product
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("dot", CgType::Float(), {vec, vec}, {"a", "b"}, nullptr, true);

        CgType hvec = CgType::Vec(ScalarKind::Half, size);
        addFunction("dot", CgType::Half(), {hvec, hvec}, {"a", "b"}, nullptr, true);
    }

    // cross product (only for float3)
    addFunction("cross", CgType::Float3(), {CgType::Float3(), CgType::Float3()}, {"a", "b"}, nullptr, true);

    // length, distance
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("length", CgType::Float(), {vec}, {"v"}, nullptr, true);
        addFunction("distance", CgType::Float(), {vec, vec}, {"a", "b"}, nullptr, true);
    }
    addFunction("length", CgType::Float(), {CgType::Float()}, {"v"}, nullptr, true);

    // normalize
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("normalize", vec, {vec}, {"v"}, nullptr, true);
    }

    // reflect, refract
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("reflect", vec, {vec, vec}, {"i", "n"}, nullptr, true);
        addFunction("refract", vec, {vec, vec, CgType::Float()}, {"i", "n", "eta"}, nullptr, true);
    }

    // faceforward
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("faceforward", vec, {vec, vec, vec}, {"n", "i", "nref"}, nullptr, true);
    }

    // mul - matrix multiplication
    // Matrix * vector
    addFunction("mul", CgType::Float4(), {CgType::Float4x4(), CgType::Float4()}, {"m", "v"}, nullptr, true);
    addFunction("mul", CgType::Float3(), {CgType::Float3x3(), CgType::Float3()}, {"m", "v"}, nullptr, true);
    addFunction("mul", CgType::Float2(), {CgType::Float2x2(), CgType::Float2()}, {"m", "v"}, nullptr, true);

    // Vector * matrix (for row-major)
    addFunction("mul", CgType::Float4(), {CgType::Float4(), CgType::Float4x4()}, {"v", "m"}, nullptr, true);
    addFunction("mul", CgType::Float3(), {CgType::Float3(), CgType::Float3x3()}, {"v", "m"}, nullptr, true);
    addFunction("mul", CgType::Float2(), {CgType::Float2(), CgType::Float2x2()}, {"v", "m"}, nullptr, true);

    // Matrix * matrix
    addFunction("mul", CgType::Float4x4(), {CgType::Float4x4(), CgType::Float4x4()}, {"a", "b"}, nullptr, true);
    addFunction("mul", CgType::Float3x3(), {CgType::Float3x3(), CgType::Float3x3()}, {"a", "b"}, nullptr, true);
    addFunction("mul", CgType::Float2x2(), {CgType::Float2x2(), CgType::Float2x2()}, {"a", "b"}, nullptr, true);

    // transpose
    addFunction("transpose", CgType::Float4x4(), {CgType::Float4x4()}, {"m"}, nullptr, true);
    addFunction("transpose", CgType::Float3x3(), {CgType::Float3x3()}, {"m"}, nullptr, true);
    addFunction("transpose", CgType::Float2x2(), {CgType::Float2x2()}, {"m"}, nullptr, true);

    // determinant (3x3, 4x4)
    addFunction("determinant", CgType::Float(), {CgType::Float3x3()}, {"m"}, nullptr, true);
    addFunction("determinant", CgType::Float(), {CgType::Float4x4()}, {"m"}, nullptr, true);

    // any, all (for bool vectors)
    for (int size = 2; size <= 4; ++size)
    {
        CgType bvec = CgType::Vec(ScalarKind::Bool, size);
        addFunction("any", CgType::Bool(), {bvec}, {"v"}, nullptr, true);
        addFunction("all", CgType::Bool(), {bvec}, {"v"}, nullptr, true);
    }
}

void SymbolTable::registerTextureSymbols()
{
    // tex1D, tex2D, tex3D, texCUBE
    addFunction("tex1D", CgType::Float4(), {CgType::Sampler1D(), CgType::Float()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2D", CgType::Float4(), {CgType::Sampler2D(), CgType::Float2()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex3D", CgType::Float4(), {CgType::Sampler3D(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
    addFunction("texCUBE", CgType::Float4(), {CgType::SamplerCube(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);

    // With explicit derivatives
    addFunction("tex2D", CgType::Float4(),
                {CgType::Sampler2D(), CgType::Float2(), CgType::Float2(), CgType::Float2()},
                {"sampler", "coord", "ddx", "ddy"}, nullptr, true);

    // tex2Dlod, tex2Dbias
    addFunction("tex2Dlod", CgType::Float4(), {CgType::Sampler2D(), CgType::Float4()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2Dbias", CgType::Float4(), {CgType::Sampler2D(), CgType::Float4()}, {"sampler", "coord"}, nullptr, true);

    // tex2Dproj
    addFunction("tex2Dproj", CgType::Float4(), {CgType::Sampler2D(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2Dproj", CgType::Float4(), {CgType::Sampler2D(), CgType::Float4()}, {"sampler", "coord"}, nullptr, true);
}

// ============================================================================
// SymbolUtils Implementation
// ============================================================================

namespace SymbolUtils
{

std::unique_ptr<Symbol> symbolFromVarDecl(VarDecl* decl)
{
    if (!decl) return nullptr;

    auto sym = std::make_unique<Symbol>();
    sym->kind = SymbolKind::Variable;
    sym->name = decl->name;
    sym->type = CgType(decl->type);
    sym->declaration = decl;
    sym->loc = decl->loc;
    sym->storage = decl->storage;
    sym->semantic = decl->semantic;
    sym->isConst = (decl->storage == StorageQualifier::Const);

    return sym;
}

std::unique_ptr<Symbol> symbolFromParamDecl(ParamDecl* decl)
{
    if (!decl) return nullptr;

    auto sym = std::make_unique<Symbol>();
    sym->kind = SymbolKind::Parameter;
    sym->name = decl->name;
    sym->type = CgType(decl->type);
    sym->declaration = decl;
    sym->loc = decl->loc;
    sym->storage = decl->storage;
    sym->semantic = decl->semantic;

    return sym;
}

std::unique_ptr<Symbol> symbolFromFunctionDecl(FunctionDecl* decl)
{
    if (!decl) return nullptr;

    auto sym = std::make_unique<Symbol>();
    sym->kind = SymbolKind::Function;
    sym->name = decl->name;
    sym->type = CgType(decl->returnType);
    sym->declaration = decl;
    sym->loc = decl->loc;
    sym->isIntrinsic = decl->isIntrinsic;
    sym->intrinsicOpcode = decl->intrinsicOpcode;

    for (const auto& param : decl->parameters)
    {
        sym->parameterTypes.push_back(CgType(param->type));
        sym->parameterNames.push_back(param->name);
    }

    return sym;
}

std::unique_ptr<Symbol> symbolFromStructDecl(StructDecl* decl)
{
    if (!decl) return nullptr;

    auto sym = std::make_unique<Symbol>();
    sym->kind = SymbolKind::Type;
    sym->name = decl->name;
    sym->type = CgType::Struct(decl->name, decl->fields);
    sym->declaration = decl;
    sym->loc = decl->loc;

    return sym;
}

std::vector<CgType> getParameterTypes(FunctionDecl* decl)
{
    std::vector<CgType> types;
    if (!decl) return types;

    for (const auto& param : decl->parameters)
    {
        types.push_back(CgType(param->type));
    }

    return types;
}

std::string formatFunctionSignature(const std::string& name, const std::vector<CgType>& paramTypes)
{
    std::string sig = name + "(";
    for (size_t i = 0; i < paramTypes.size(); ++i)
    {
        if (i > 0) sig += ", ";
        sig += paramTypes[i].toString();
    }
    sig += ")";
    return sig;
}

} // namespace SymbolUtils
