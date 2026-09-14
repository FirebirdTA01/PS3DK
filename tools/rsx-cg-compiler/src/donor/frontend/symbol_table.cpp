#include "symbol_table.h"
#include <algorithm>
#include <limits>

namespace
{
// preferOverload used to live here: for an exact signature tie it let a source
// declaration shadow a registered builtin.  PARTITION BY NAME subsumes it -
// when any source declaration of the name is visible the builtins are not
// candidates at all, so a source-versus-builtin tie can no longer be reached,
// and every remaining tie is source-versus-source, which the reference calls
// C1101 rather than resolving by preference (t_36492ad8).
}

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
    // ASK THE DECLARATION, never assume.  This path registers builtins (decl
    // null, no defaults) and source functions alike.  Getting it wrong failed
    // in both directions during this slice: left at its zero initialiser it
    // made every BUILTIN viable for a call with too few arguments and turned
    // the whole builtin header ambiguous; hardcoded to paramTypes.size() it
    // refused every legitimate defaulted call.
    symbol->requiredParameterCount = paramTypes.size();
    if (decl)
    {
        // COUNT BACK OVER TRAILING DEFAULTS - and over ALL of them.
        //
        // An out/inout parameter that carries a default is NOT excluded here.
        // Measured on sce-cgc 475: such a candidate still takes part in
        // ranking.  `void f(float4)` beside `void f(float4, out float k=.5)`,
        // called `f(t)`, is "error C1101: ambiguous overloaded function
        // reference" in BOTH declaration orders, and a plain `float k=.5`
        // second parameter behaves identically - so the default-filled
        // candidate is viable and ties, and this is not an out/inout rule at
        // all.  Excluding it here would leave `f(float4)` the unique winner
        // and ACCEPT a program the reference refuses (verified: all four
        // out/inout ambiguity cells emitted a container).
        //
        // The out/inout restriction is real but applies AFTER a unique winner
        // is picked: the omitted argument is not an lvalue, which the semantic
        // analyser reports as "error C1111: non-lvalue actual parameter #N
        // cannot be out parameter".  Ruling: Fable, retracting the viability
        // exclusion on codex's six cells.
        size_t required = decl->parameters.size();
        while (required > 0 && decl->parameters[required - 1] &&
               decl->parameters[required - 1]->defaultValue != nullptr)
        {
            --required;
        }
        symbol->requiredParameterCount = required;
    }
    symbol->declIndex = declIndexCursor_;
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

bool SymbolTable::hasVisibleFunction(const std::string& name,
                                     size_t visibleThrough) const
{
    auto it = functionOverloads.find(name);
    if (it == functionOverloads.end()) return false;
    for (Symbol* sym : it->second)
    {
        if (sym->declIndex <= visibleThrough) return true;
    }
    return false;
}

std::optional<SymbolTable::OverloadCandidate> SymbolTable::resolveOverload(
    const std::string& name,
    const std::vector<CgType>& argumentTypes,
    bool* ambiguous,
    size_t visibleThrough) const
{
    if (ambiguous) *ambiguous = false;
    int bestNarrowing = 0;
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

    // STEP 2 needs one look ahead: does the name have ANY visible source
    // declaration?  If it does, the builtins of that name are out of
    // consideration entirely, viable or not.
    bool haveVisibleSource = false;
    for (Symbol* sym : overloads)
    {
        if (sym->declIndex <= visibleThrough && sym->declaration != nullptr)
        {
            haveVisibleSource = true;
            break;
        }
    }

    // STEP 1, AND THE REASON IT HAS TO HAPPEN HERE.  Every declaration gets
    // its own entry in functionOverloads - a prototype and its definition are
    // two entries with ONE signature.  They are one function, so they are
    // collapsed to the LATEST VISIBLE declaration before anything is ranked.
    //
    // Without this collapse the tie rule below would call every prototyped
    // program ambiguous, since the prototype and the definition are both
    // viable at the same cost.
    //
    // Collapsing to the LATEST is also rule 1 itself: the reference takes a
    // default from the declaration visible at the call, so
    //     proto k=.25 ; def k=.5 ; call        -> .5      (definition later)
    //     proto k=.25 ; call ; def k=.5        -> .25     (definition unseen)
    //     proto k=.25 ; def with NO default    -> C1103   (the default is gone)
    // all fall out of "keep the one with the greatest declIndex <=
    // visibleThrough", because requiredParameterCount travels with the
    // declaration that set it.
    auto sameSignature = [](const Symbol* a, const Symbol* b)
    {
        if (a->parameterTypes.size() != b->parameterTypes.size()) return false;
        for (size_t i = 0; i < a->parameterTypes.size(); ++i)
        {
            if (!a->parameterTypes[i].equals(b->parameterTypes[i])) return false;
        }
        return true;
    };

    std::vector<Symbol*> visible;
    for (Symbol* sym : overloads)
    {
        // A DECLARATION WRITTEN AFTER THIS CALL IS NOT A CANDIDATE: the
        // reference refuses a forward call with no prior prototype ("error
        // C1008: undefined variable") and does not let a source overload
        // written later hide a builtin.  Builtins carry index 0 and so are
        // never filtered here.
        if (sym->declIndex > visibleThrough) continue;
        bool merged = false;
        for (Symbol*& kept : visible)
        {
            if (!sameSignature(kept, sym)) continue;
            if (sym->declIndex >= kept->declIndex) kept = sym;
            merged = true;
            break;
        }
        if (!merged) visible.push_back(sym);
    }

    OverloadCandidate best;
    best.symbol = nullptr;
    best.conversionCost = std::numeric_limits<int>::max();
    best.exactMatch = false;
    bool tied = false;

    for (Symbol* sym : visible)
    {
        // STEP 2, PARTITION BY NAME.  A visible source declaration hides the
        // builtins of that name even when it is NOT VIABLE: measured, a source
        // `float sin(float, float)` with two required parameters makes
        // `sin(t.x)` C1103 "too few parameters", never the builtin sine.
        if (haveVisibleSource && sym->declaration == nullptr)
        {
            continue;
        }

        // STEP 3, ARITY.  A call may omit TRAILING parameters that carry
        // defaults; requiredParameterCount equals the parameter count for
        // every function without them, so this is exact equality for those.
        if (argumentTypes.size() > sym->parameterTypes.size() ||
            argumentTypes.size() < sym->requiredParameterCount)
        {
            continue;
        }

        // STEP 4, COST over the SUPPLIED arguments only.  Filling a default
        // costs nothing and so cannot break a tie - which is why
        // `f(float4)` beside `f(float4, float k = 1.0)` called f(t) is
        // ambiguous rather than resolved in favour of the exact arity.
        //
        // NARROWING IS COUNTED SEPARATELY AND COMPARED FIRST.  A single
        // scalar-cost total cannot express the reference's rule, and the
        // arithmetic collides in practice: with "a promotion costs its rank
        // distance, a demotion costs twice it", clamp(float,float,float) for
        // clamp(f, 0, 1) costs two promotions = 2, and clamp(int,int,int)
        // costs one demotion = 2.  They tie, and a tie is C1101 - which
        // refused five SDK shaders that the reference and our own parent both
        // compiled (MachoQHDR x4, stereo3D fp_anaglyph).
        //
        // Measured on sce-cgc 475, in both directions and with named
        // variables rather than literals, so this is a TYPE rule and not a
        // literal rule:
        //     clamp(float, int,   int  )  -> the float overload  (2 up)
        //     clamp(int,   float, float)  -> the float overload  (1 up)
        //     clamp(float, half,  half )  -> the float overload  (2 up)
        // The second is the one that settles it: the all-widening candidate
        // wins even when it needs FEWER conversions than the narrowing one
        // needs, so no exchange rate reproduces it.  Any narrowing loses to a
        // sequence of widenings, whatever the counts, and cost only separates
        // candidates that narrow equally.
        int totalCost = 0;
        int totalNarrowing = 0;
        bool viable = true;

        for (size_t i = 0; i < argumentTypes.size(); ++i)
        {
            const CgType& argType = argumentTypes[i];
            const CgType& paramType = sym->parameterTypes[i];

            if (argType.equals(paramType))
            {
                continue;
            }

            int cost = TypeConversion::conversionCost(argType, paramType);
            if (cost < 0)
            {
                // No conversion possible
                viable = false;
                break;
            }

            totalCost += cost;
            totalNarrowing += TypeConversion::narrowingSteps(argType, paramType);
        }

        if (!viable) continue;

        const bool better = !best.symbol ||
            totalNarrowing < bestNarrowing ||
            (totalNarrowing == bestNarrowing && totalCost < best.conversionCost);
        const bool equal = best.symbol &&
            totalNarrowing == bestNarrowing && totalCost == best.conversionCost;

        if (better)
        {
            best.symbol = sym;
            best.conversionCost = totalCost;
            bestNarrowing = totalNarrowing;
            best.exactMatch = (totalCost == 0);
            tied = false;
        }
        else if (equal)
        {
            // TWO CANDIDATES AT THE BEST COST IS C1101, whatever distinguishes
            // them.  Arity does not break it, exactness does not break it, and
            // declaration order must not - measured on three shapes the
            // reference refuses and we used to accept:
            //     f(float4, float = .25) beside f(float4, int = 2)
            //     f(half4)               beside f(half4, float = .25)
            //     f(float4)              beside f(float4, float k = 1.0)
            tied = true;
        }
    }

    if (tied)
    {
        if (ambiguous) *ambiguous = true;
        return std::nullopt;
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
    addType("matrix", CgType::Float4x4());
    // Non-square matrices are real types on the reference: RxC = R rows of
    // C-wide vectors (t_bc130064 / t_69aeaa84).
    for (int r = 2; r <= 4; ++r)
        for (int c = 2; c <= 4; ++c)
        {
            if (r == c) continue;
            addType("float" + std::to_string(r) + "x" + std::to_string(c), CgType::Mat(ScalarKind::Float, r, c));
            addType("half" + std::to_string(r) + "x" + std::to_string(c), CgType::Mat(ScalarKind::Half, r, c));
        }

    // Sampler types
    addType("sampler1D", CgType::Sampler1D());
    addType("sampler2D", CgType::Sampler2D());
    addType("sampler3D", CgType::Sampler3D());
    addType("samplerCUBE", CgType::SamplerCube());
    addType("samplerRECT", CgType::SamplerRect());
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
    addScalarVectorOverloads("radians", CgType::Float(), CgType::Float());
    addScalarVectorOverloads("degrees", CgType::Float(), CgType::Float());

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

    // Screen-space derivatives. Widths 3/4 are registered so the lowering
    // path can issue the measured profile diagnostic instead of a generic
    // overload failure; slice 1 only emits scalar and float2.
    addFunction("ddx", CgType::Float(), {CgType::Float()}, {"x"}, nullptr, true);
    addFunction("ddy", CgType::Float(), {CgType::Float()}, {"x"}, nullptr, true);
    for (int size = 2; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("ddx", vec, {vec}, {"x"}, nullptr, true);
        addFunction("ddy", vec, {vec}, {"x"}, nullptr, true);
    }

    // The pack/unpack family (t_23f9d1a6): one NV40 fragment instruction each.
    // Measured on the reference: pack_2half / pack_2ushort take half2 or
    // float2 (a float scalar smears; a half scalar and a float3 are C1101
    // ambiguous, which the two overloads reproduce), pack_4ubyte / pack_4byte
    // take half4 or float4 (float3 is C1115), and the unpacks take a float:
    // unpack_2half yields half2, the others float.  Registered for both
    // profiles; the vertex lowering refuses them by name, as the reference
    // does (C1115 / C5201).
    for (const char* name : {"pack_2half", "pack_2ushort"})
    {
        addFunction(name, CgType::Float(), {CgType::Half2()}, {"a"}, nullptr, true);
        addFunction(name, CgType::Float(), {CgType::Float2()}, {"a"}, nullptr, true);
    }
    for (const char* name : {"pack_4ubyte", "pack_4byte"})
    {
        addFunction(name, CgType::Float(), {CgType::Half4()}, {"a"}, nullptr, true);
        addFunction(name, CgType::Float(), {CgType::Float4()}, {"a"}, nullptr, true);
    }
    addFunction("unpack_2half", CgType::Half2(), {CgType::Float()}, {"a"}, nullptr, true);
    addFunction("unpack_2ushort", CgType::Float2(), {CgType::Float()}, {"a"}, nullptr, true);
    addFunction("unpack_4ubyte", CgType::Float4(), {CgType::Float()}, {"a"}, nullptr, true);
    addFunction("unpack_4byte", CgType::Float4(), {CgType::Float()}, {"a"}, nullptr, true);

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
    // THE SCALAR distance, which the reference has and this table did not.
    // `distance(p.x, p.y)` has no exact candidate without it, so every
    // vector overload becomes viable by broadcasting BOTH arguments - three
    // candidates at one broadcast each, all tying.  The parent hid that by
    // taking the first viable one (and emitted 336 bytes where the reference
    // emits 288); once ties are C1101 it surfaced as a refusal.  The
    // reference lowers it to ADDR + |abs|, i.e. abs(a-b), and it is the
    // exact-match partner of the scalar `length` registered directly above.
    addFunction("distance", CgType::Float(), {CgType::Float(), CgType::Float()}, {"a", "b"}, nullptr, true);

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
    for (int size = 1; size <= 4; ++size)
    {
        CgType vec = CgType::Vec(ScalarKind::Float, size);
        addFunction("faceforward", vec, {vec, vec, vec}, {"n", "i", "nref"}, nullptr, true);
    }

    // mul - matrix multiplication, over every RxC shape (t_bc130064).
    // Measured on the reference: mul(M[RxC], v[C]) -> v[R] (one DP(C) per
    // row), mul(v[R], M[RxC]) -> v[C] (a MUL/MAD chain over the rows), and
    // mul(A[RxK], B[KxC]) -> M[RxC].  The square entries below are the same
    // ones this table always had; the loops add the rectangular shapes.
    for (int r = 2; r <= 4; ++r)
    {
        for (int c = 2; c <= 4; ++c)
        {
            const CgType m = CgType::Mat(ScalarKind::Float, r, c);
            // Matrix * vector
            addFunction("mul", CgType::Vec(ScalarKind::Float, r), {m, CgType::Vec(ScalarKind::Float, c)}, {"m", "v"}, nullptr, true);
            // Vector * matrix (row-major)
            addFunction("mul", CgType::Vec(ScalarKind::Float, c), {CgType::Vec(ScalarKind::Float, r), m}, {"v", "m"}, nullptr, true);
        }
    }
    // Matrix * matrix
    for (int r = 2; r <= 4; ++r)
        for (int k = 2; k <= 4; ++k)
            for (int c = 2; c <= 4; ++c)
                addFunction("mul", CgType::Mat(ScalarKind::Float, r, c),
                            {CgType::Mat(ScalarKind::Float, r, k), CgType::Mat(ScalarKind::Float, k, c)},
                            {"a", "b"}, nullptr, true);

    // transpose
    for (ScalarKind sk : {ScalarKind::Float, ScalarKind::Half})
    {
        for (int r = 2; r <= 4; ++r)
        {
            for (int c = 2; c <= 4; ++c)
            {
                addFunction("transpose", CgType::Mat(sk, c, r), {CgType::Mat(sk, r, c)}, {"m"}, nullptr, true);
            }
        }
    }

    // lit(NdotL, NdotH, m)
    for (ScalarKind lSk : {ScalarKind::Float, ScalarKind::Half})
    {
        for (ScalarKind hSk : {ScalarKind::Float, ScalarKind::Half})
        {
            for (ScalarKind mSk : {ScalarKind::Float, ScalarKind::Half})
            {
                const bool allHalf = (lSk == ScalarKind::Half && hSk == ScalarKind::Half && mSk == ScalarKind::Half);
                CgType retType = allHalf ? CgType::Half4() : CgType::Float4();
                addFunction("lit", retType, {CgType::Scalar(lSk), CgType::Scalar(hSk), CgType::Scalar(mSk)}, {"NdotL", "NdotH", "m"}, nullptr, true);
            }
        }
    }

    // determinant (3x3, 4x4)
    addFunction("determinant", CgType::Float(), {CgType::Float3x3()}, {"m"}, nullptr, true);
    addFunction("determinant", CgType::Float(), {CgType::Float4x4()}, {"m"}, nullptr, true);

    // any accepts a scalar too; register it explicitly rather than relying
    // on scalar-to-vector broadcasting during overload resolution.
    addFunction("any", CgType::Bool(), {CgType::Bool()}, {"v"}, nullptr, true);
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
    // tex1D, tex2D, tex3D, texCUBE, texRECT
    addFunction("tex1D", CgType::Float4(), {CgType::Sampler1D(), CgType::Float()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex1D", CgType::Float4(), {CgType::Sampler1D(), CgType::Float2()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex1D", CgType::Float4(), {CgType::Sampler1D(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex1D", CgType::Float4(), {CgType::Sampler1D(), CgType::Float4()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex1D", CgType::Float4(), {CgType::Sampler1D(), CgType::Half()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex1D", CgType::Float4(), {CgType::Sampler1D(), CgType::Half2()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2D", CgType::Float4(), {CgType::Sampler2D(), CgType::Float2()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2D", CgType::Float4(), {CgType::Sampler2D(), CgType::Half2()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2D", CgType::Float4(), {CgType::Sampler2D(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2D", CgType::Float4(), {CgType::Sampler2D(), CgType::Half3()}, {"sampler", "coord"}, nullptr, true);

    addFunction("tex3D", CgType::Float4(), {CgType::Sampler3D(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex3D", CgType::Float4(), {CgType::Sampler3D(), CgType::Half3()}, {"sampler", "coord"}, nullptr, true);

    addFunction("texCUBE", CgType::Float4(), {CgType::SamplerCube(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
    addFunction("texCUBE", CgType::Float4(), {CgType::SamplerCube(), CgType::Half3()}, {"sampler", "coord"}, nullptr, true);

    addFunction("texRECT", CgType::Float4(), {CgType::SamplerRect(), CgType::Float2()}, {"sampler", "coord"}, nullptr, true);
    addFunction("texRECT", CgType::Float4(), {CgType::SamplerRect(), CgType::Half2()}, {"sampler", "coord"}, nullptr, true);

    addFunction("texDepth2D", CgType::Float(), {CgType::Sampler2D(), CgType::Half2()}, {"sampler", "coord"}, nullptr, true);
    addFunction("texDepth2D_precise", CgType::Float(), {CgType::Sampler2D(), CgType::Float2()}, {"sampler", "coord"}, nullptr, true);

    // Typed texture lookup families: f1tex..f4tex and h1tex..h4tex
    auto regTexFamily = [this](const std::string& prefix, ScalarKind sk, int retWidth) {
        CgType retType = (retWidth == 1) ? CgType::Scalar(sk) : CgType::Vec(sk, retWidth);

        // 1D: sampler1D, float/half/float2/half2
        std::string name1D = prefix + "tex1D";
        addFunction(name1D, retType, {CgType::Sampler1D(), CgType::Float()}, {"sampler", "coord"}, nullptr, true);
        addFunction(name1D, retType, {CgType::Sampler1D(), CgType::Half()}, {"sampler", "coord"}, nullptr, true);
        addFunction(name1D, retType, {CgType::Sampler1D(), CgType::Float2()}, {"sampler", "coord"}, nullptr, true);
        addFunction(name1D, retType, {CgType::Sampler1D(), CgType::Half2()}, {"sampler", "coord"}, nullptr, true);

        // 2D: sampler2D, float2/half2/float3/half3
        std::string name2D = prefix + "tex2D";
        addFunction(name2D, retType, {CgType::Sampler2D(), CgType::Float2()}, {"sampler", "coord"}, nullptr, true);
        addFunction(name2D, retType, {CgType::Sampler2D(), CgType::Half2()}, {"sampler", "coord"}, nullptr, true);
        addFunction(name2D, retType, {CgType::Sampler2D(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
        addFunction(name2D, retType, {CgType::Sampler2D(), CgType::Half3()}, {"sampler", "coord"}, nullptr, true);

        // 3D: sampler3D, float3/half3
        std::string name3D = prefix + "tex3D";
        addFunction(name3D, retType, {CgType::Sampler3D(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
        addFunction(name3D, retType, {CgType::Sampler3D(), CgType::Half3()}, {"sampler", "coord"}, nullptr, true);

        // CUBE: samplerCUBE, float3/half3
        std::string nameCUBE = prefix + "texCUBE";
        addFunction(nameCUBE, retType, {CgType::SamplerCube(), CgType::Float3()}, {"sampler", "coord"}, nullptr, true);
        addFunction(nameCUBE, retType, {CgType::SamplerCube(), CgType::Half3()}, {"sampler", "coord"}, nullptr, true);

        // RECT: samplerRECT, float2/half2
        std::string nameRECT = prefix + "texRECT";
        addFunction(nameRECT, retType, {CgType::SamplerRect(), CgType::Float2()}, {"sampler", "coord"}, nullptr, true);
        addFunction(nameRECT, retType, {CgType::SamplerRect(), CgType::Half2()}, {"sampler", "coord"}, nullptr, true);
    };

    for (int w = 1; w <= 4; ++w) {
        regTexFamily("f" + std::to_string(w), ScalarKind::Float, w);
        regTexFamily("h" + std::to_string(w), ScalarKind::Half, w);
    }

    // With explicit derivatives
    addFunction("tex2D", CgType::Float4(),
                {CgType::Sampler2D(), CgType::Float2(), CgType::Float2(), CgType::Float2()},
                {"sampler", "coord", "ddx", "ddy"}, nullptr, true);

    // tex2Dlod, tex2Dbias
    addFunction("tex2Dlod", CgType::Float4(), {CgType::Sampler2D(), CgType::Float4()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2Dbias", CgType::Float4(), {CgType::Sampler2D(), CgType::Float4()}, {"sampler", "coord"}, nullptr, true);
    addFunction("tex2Dbias", CgType::Float4(), {CgType::Sampler2D(), CgType::Half4()}, {"sampler", "coord"}, nullptr, true);

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
