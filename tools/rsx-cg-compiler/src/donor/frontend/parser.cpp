#include "parser.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdlib>

// ============================================================================
// Constructor and initialization
// ============================================================================

Parser::Parser(const std::vector<Token>& tokens, const std::string& filename)
    : tokens(tokens), filename(filename)
{
    initBuiltinTypes();
}

void Parser::initBuiltinTypes()
{
    // All built-in type names that can be used as type specifiers
    typeNames = {
        "void", "bool", "int", "uint", "float", "half", "fixed",
        "char", "uchar", "short", "ushort",
        "bool2", "bool3", "bool4",
        "int2", "int3", "int4",
        "uint2", "uint3", "uint4",
        "float2", "float3", "float4",
        "half2", "half3", "half4",
        "fixed2", "fixed3", "fixed4",
        "char2", "char3", "char4",
        "uchar2", "uchar3", "uchar4",
        "short2", "short3", "short4",
        "ushort2", "ushort3", "ushort4",
        "float2x2", "float3x3", "float4x4",
        "float2x3", "float2x4", "float3x2", "float3x4", "float4x2", "float4x3",
        "half2x2", "half3x3", "half4x4",
        "sampler", "sampler1D", "sampler2D", "sampler3D", "samplerCUBE", "samplerRECT",
        "isampler1D", "isampler2D", "isampler3D", "isamplerCUBE", "isamplerRECT",
        "usampler1D", "usampler2D", "usampler3D", "usamplerCUBE", "usamplerRECT"
    };
}

// ============================================================================
// Main parsing entry point
// ============================================================================

std::unique_ptr<TranslationUnit> Parser::parse()
{
    auto unit = std::make_unique<TranslationUnit>();
    unit->filename = filename;

    while (!isAtEnd())
    {
        try
        {
            auto decl = parseTopLevelDeclaration();
            if (decl)
            {
                // Track struct/typedef names for type resolution
                if (decl->kind == DeclKind::Struct)
                {
                    typeNames.insert(decl->name);
                }
                else if (decl->kind == DeclKind::Typedef)
                {
                    typeNames.insert(decl->name);
                }

                unit->declarations.push_back(std::move(decl));
            }
        }
        catch (...)
        {
            synchronize();
            if (static_cast<int>(errors.size()) >= config.maxErrors)
            {
                error("Too many errors, stopping parse");
                break;
            }
        }
    }

    // Find entry point (default "main")
    for (auto& decl : unit->declarations)
    {
        if (decl->kind == DeclKind::Function)
        {
            auto* func = static_cast<FunctionDecl*>(decl.get());
            if (func->name == "main" && !func->isPrototype())
            {
                unit->entryPoint = func;
                break;
            }
        }
    }

    return unit;
}

// ============================================================================
// Token utilities
// ============================================================================

bool Parser::isAtEnd() const
{
    return current >= tokens.size() || peek().type == TokenType::END_OF_FILE;
}

const Token& Parser::peek(int offset) const
{
    size_t idx = current + offset;
    if (idx >= tokens.size())
    {
        static Token eof{TokenType::END_OF_FILE, "", 0, 0, ""};
        return eof;
    }
    return tokens[idx];
}

const Token& Parser::previous() const
{
    if (current == 0)
        return tokens[0];
    return tokens[current - 1];
}

const Token& Parser::advance()
{
    if (!isAtEnd())
        current++;
    return previous();
}

bool Parser::check(TokenType type) const
{
    if (isAtEnd())
        return false;
    return peek().type == type;
}

bool Parser::match(TokenType type)
{
    if (check(type))
    {
        advance();
        return true;
    }
    return false;
}

bool Parser::match(std::initializer_list<TokenType> types)
{
    for (auto type : types)
    {
        if (check(type))
        {
            advance();
            return true;
        }
    }
    return false;
}

Token Parser::consume(TokenType type, const std::string& message)
{
    if (check(type))
        return advance();

    error(message + " (got '" + peek().lexeme + "')");
    throw std::runtime_error(message);
}

SourceLocation Parser::currentLocation() const
{
    const Token& tok = peek();
    return {tok.filename.empty() ? filename : tok.filename, tok.line, tok.column};
}

// ============================================================================
// Error handling
// ============================================================================

void Parser::error(const std::string& message)
{
    error(currentLocation(), message);
}

void Parser::error(const SourceLocation& loc, const std::string& message)
{
    errors.push_back({loc, message, false});
    panicMode = true;
}

void Parser::warning(const std::string& message)
{
    errors.push_back({currentLocation(), message, true});
}

void Parser::printErrors() const
{
    for (const auto& err : errors)
    {
        std::cerr << err.loc.toString() << ": "
                  << (err.isWarning ? "warning: " : "error: ")
                  << err.message << std::endl;
    }
}

void Parser::synchronize()
{
    panicMode = false;
    advance();

    while (!isAtEnd())
    {
        // Synchronize after semicolon
        if (previous().type == TokenType::SEMICOLON)
            return;

        // Synchronize before these keywords (start of new declaration/statement)
        switch (peek().type)
        {
        case TokenType::KW_STRUCT:
        case TokenType::KW_TYPEDEF:
        case TokenType::KW_UNIFORM:
        case TokenType::KW_IN:
        case TokenType::KW_OUT:
        case TokenType::KW_INOUT:
        case TokenType::KW_CONST:
        case TokenType::KW_STATIC:
        case TokenType::KW_IF:
        case TokenType::KW_FOR:
        case TokenType::KW_WHILE:
        case TokenType::KW_DO:
        case TokenType::KW_RETURN:
        case TokenType::KW_VOID:
        case TokenType::KW_FLOAT:
        case TokenType::KW_INT:
        case TokenType::KW_HALF:
        case TokenType::RBRACE:
            return;
        default:
            break;
        }

        advance();
    }
}

// ============================================================================
// Type parsing
// ============================================================================

bool Parser::isTypeName() const
{
    if (isAtEnd())
        return false;

    const Token& tok = peek();

    // Check if it's a type keyword
    switch (tok.type)
    {
    case TokenType::KW_VOID:
    case TokenType::KW_BOOL:
    case TokenType::KW_INT:
    case TokenType::KW_UINT:
    case TokenType::KW_FLOAT:
    case TokenType::KW_HALF:
    case TokenType::KW_FIXED:
    case TokenType::KW_CHAR:
    case TokenType::KW_UCHAR:
    case TokenType::KW_SHORT:
    case TokenType::KW_USHORT:
    case TokenType::KW_BOOL2:
    case TokenType::KW_BOOL3:
    case TokenType::KW_BOOL4:
    case TokenType::KW_INT2:
    case TokenType::KW_INT3:
    case TokenType::KW_INT4:
    case TokenType::KW_UINT2:
    case TokenType::KW_UINT3:
    case TokenType::KW_UINT4:
    case TokenType::KW_FLOAT2:
    case TokenType::KW_FLOAT3:
    case TokenType::KW_FLOAT4:
    case TokenType::KW_FLOAT2X2:
    case TokenType::KW_FLOAT3X3:
    case TokenType::KW_FLOAT4X4:
    case TokenType::KW_HALF2:
    case TokenType::KW_HALF3:
    case TokenType::KW_HALF4:
    case TokenType::KW_HALF2X2:
    case TokenType::KW_HALF3X3:
    case TokenType::KW_HALF4X4:
    case TokenType::KW_FIXED2:
    case TokenType::KW_FIXED3:
    case TokenType::KW_FIXED4:
    case TokenType::KW_CHAR2:
    case TokenType::KW_CHAR3:
    case TokenType::KW_CHAR4:
    case TokenType::KW_UCHAR2:
    case TokenType::KW_UCHAR3:
    case TokenType::KW_UCHAR4:
    case TokenType::KW_SHORT2:
    case TokenType::KW_SHORT3:
    case TokenType::KW_SHORT4:
    case TokenType::KW_USHORT2:
    case TokenType::KW_USHORT3:
    case TokenType::KW_USHORT4:
    case TokenType::KW_SAMPLER1D:
    case TokenType::KW_SAMPLER2D:
    case TokenType::KW_SAMPLER3D:
    case TokenType::KW_SAMPLERCUBE:
    case TokenType::KW_SAMPLERRECT:
    case TokenType::KW_ISAMPLER1D:
    case TokenType::KW_ISAMPLER2D:
    case TokenType::KW_ISAMPLER3D:
    case TokenType::KW_ISAMPLERCUBE:
    case TokenType::KW_ISAMPLERRECT:
    case TokenType::KW_USAMPLER1D:
    case TokenType::KW_USAMPLER2D:
    case TokenType::KW_USAMPLER3D:
    case TokenType::KW_USAMPLERCUBE:
    case TokenType::KW_USAMPLERRECT:
        return true;
    default:
        break;
    }

    // Check if it's a known type name (struct/typedef)
    if (tok.type == TokenType::IDENTIFIER)
    {
        return typeNames.count(tok.lexeme) > 0;
    }

    return false;
}

bool Parser::isTypeStart() const
{
    // Check for storage qualifiers that can precede a type
    switch (peek().type)
    {
    case TokenType::KW_UNIFORM:
    case TokenType::KW_IN:
    case TokenType::KW_OUT:
    case TokenType::KW_INOUT:
    case TokenType::KW_CONST:
    case TokenType::KW_STATIC:
    case TokenType::KW_EXTERN:
    case TokenType::KW_PACKED:
    case TokenType::KW_ROW_MAJOR:
    case TokenType::KW_COLUMN_MAJOR:
        return true;
    default:
        break;
    }

    return isTypeName();
}

std::shared_ptr<TypeNode> Parser::parseType()
{
    auto type = parseBaseType();
    if (!type)
        return nullptr;

    // Check for array brackets
    while (check(TokenType::LBRACKET))
    {
        type = parseArrayType(type);
    }

    return type;
}

std::shared_ptr<TypeNode> Parser::parseBaseType()
{
    auto type = std::make_shared<TypeNode>();

    // Handle matrix layout qualifiers
    if (match(TokenType::KW_ROW_MAJOR))
    {
        type->isRowMajor = true;
    }
    else if (match(TokenType::KW_COLUMN_MAJOR))
    {
        type->isRowMajor = false;
    }

    // Handle packed qualifier
    if (match(TokenType::KW_PACKED))
    {
        type->isPacked = true;
    }

    const Token& tok = peek();

    // Check for struct type reference (user-defined type name)
    if (tok.type == TokenType::IDENTIFIER && typeNames.count(tok.lexeme) > 0)
    {
        advance();
        type->baseType = BaseType::Struct;
        type->structName = tok.lexeme;
        return type;
    }

    // Handle "unsigned" as a type modifier (lexer maps "unsigned" to KW_UINT)
    // When we see "unsigned char", "unsigned short", "unsigned int", etc.
    bool isUnsigned = false;
    if (tok.type == TokenType::KW_UINT && tok.lexeme == "unsigned")
    {
        // Check if next token is a type that can be modified by "unsigned"
        if (current + 1 < tokens.size())
        {
            TokenType nextType = tokens[current + 1].type;
            switch (nextType)
            {
            case TokenType::KW_CHAR:
            case TokenType::KW_CHAR2:
            case TokenType::KW_CHAR3:
            case TokenType::KW_CHAR4:
            case TokenType::KW_SHORT:
            case TokenType::KW_SHORT2:
            case TokenType::KW_SHORT3:
            case TokenType::KW_SHORT4:
            case TokenType::KW_INT:
            case TokenType::KW_INT2:
            case TokenType::KW_INT3:
            case TokenType::KW_INT4:
                // Consume "unsigned" and proceed to parse the actual type as unsigned
                advance();
                isUnsigned = true;
                break;
            default:
                // Just "unsigned" alone means "unsigned int"
                break;
            }
        }
    }

    // Handle "signed" as a type modifier (lexer maps "signed" to KW_INT)
    // When we see "signed char", "signed short", "signed int", etc.
    // signed types are the default, so we just skip the "signed" keyword
    if (tok.type == TokenType::KW_INT && tok.lexeme == "signed")
    {
        // Check if next token is a type that can be modified by "signed"
        if (current + 1 < tokens.size())
        {
            TokenType nextType = tokens[current + 1].type;
            switch (nextType)
            {
            case TokenType::KW_CHAR:
            case TokenType::KW_CHAR2:
            case TokenType::KW_CHAR3:
            case TokenType::KW_CHAR4:
            case TokenType::KW_SHORT:
            case TokenType::KW_SHORT2:
            case TokenType::KW_SHORT3:
            case TokenType::KW_SHORT4:
            case TokenType::KW_INT:
            case TokenType::KW_INT2:
            case TokenType::KW_INT3:
            case TokenType::KW_INT4:
                // Consume "signed" and proceed to parse the actual type
                // (signed is the default, so no additional flag needed)
                advance();
                break;
            default:
                // Just "signed" alone means "signed int" = "int"
                break;
            }
        }
    }

    // Get the (possibly updated) current token
    const Token& typeTok = peek();

    // Parse built-in type
    type->baseType = tokenToBaseType(typeTok.type);
    if (type->baseType == BaseType::Void && typeTok.type != TokenType::KW_VOID)
    {
        error("Expected type name");
        return nullptr;
    }

    // Apply unsigned modifier if needed
    if (isUnsigned)
    {
        switch (type->baseType)
        {
        case BaseType::Char:
            type->baseType = BaseType::UChar;
            break;
        case BaseType::Short:
            type->baseType = BaseType::UShort;
            break;
        case BaseType::Int:
            type->baseType = BaseType::UInt;
            break;
        default:
            // Already unsigned or not applicable
            break;
        }
    }

    advance();

    // Set vector/matrix dimensions based on type
    switch (typeTok.type)
    {
    case TokenType::KW_FLOAT2:
    case TokenType::KW_HALF2:
    case TokenType::KW_FIXED2:
    case TokenType::KW_INT2:
    case TokenType::KW_UINT2:
    case TokenType::KW_BOOL2:
    case TokenType::KW_CHAR2:
    case TokenType::KW_UCHAR2:
    case TokenType::KW_SHORT2:
    case TokenType::KW_USHORT2:
        type->vectorSize = 2;
        break;
    case TokenType::KW_FLOAT3:
    case TokenType::KW_HALF3:
    case TokenType::KW_FIXED3:
    case TokenType::KW_INT3:
    case TokenType::KW_UINT3:
    case TokenType::KW_BOOL3:
    case TokenType::KW_CHAR3:
    case TokenType::KW_UCHAR3:
    case TokenType::KW_SHORT3:
    case TokenType::KW_USHORT3:
        type->vectorSize = 3;
        break;
    case TokenType::KW_FLOAT4:
    case TokenType::KW_HALF4:
    case TokenType::KW_FIXED4:
    case TokenType::KW_INT4:
    case TokenType::KW_UINT4:
    case TokenType::KW_BOOL4:
    case TokenType::KW_CHAR4:
    case TokenType::KW_UCHAR4:
    case TokenType::KW_SHORT4:
    case TokenType::KW_USHORT4:
        type->vectorSize = 4;
        break;
    case TokenType::KW_FLOAT2X2:
        type->matrixRows = 2;
        type->matrixCols = 2;
        break;
    case TokenType::KW_FLOAT3X3:
        type->matrixRows = 3;
        type->matrixCols = 3;
        break;
    case TokenType::KW_FLOAT4X4:
        type->matrixRows = 4;
        type->matrixCols = 4;
        break;
    case TokenType::KW_HALF2X2:
        type->matrixRows = 2;
        type->matrixCols = 2;
        break;
    case TokenType::KW_HALF3X3:
        type->matrixRows = 3;
        type->matrixCols = 3;
        break;
    case TokenType::KW_HALF4X4:
        type->matrixRows = 4;
        type->matrixCols = 4;
        break;
    default:
        type->vectorSize = 1;
        break;
    }

    return type;
}

std::shared_ptr<TypeNode> Parser::parseArrayType(std::shared_ptr<TypeNode> elementType)
{
    consume(TokenType::LBRACKET, "Expected '['");

    auto arrayType = std::make_shared<TypeNode>();
    arrayType->baseType = BaseType::Array;
    arrayType->elementType = elementType;

    // Parse array size (must be constant expression, but for now just integer literal)
    if (!check(TokenType::RBRACKET))
    {
        if (check(TokenType::NUMBER))
        {
            const Token& sizeTok = advance();
            arrayType->arraySize = std::stoi(sizeTok.lexeme);
        }
        else
        {
            error("Expected array size");
        }
    }
    else
    {
        // Unsized array (only valid in certain contexts)
        arrayType->arraySize = 0;
    }

    consume(TokenType::RBRACKET, "Expected ']'");
    return arrayType;
}

BaseType Parser::tokenToBaseType(TokenType type) const
{
    switch (type)
    {
    case TokenType::KW_VOID:
        return BaseType::Void;
    case TokenType::KW_BOOL:
    case TokenType::KW_BOOL2:
    case TokenType::KW_BOOL3:
    case TokenType::KW_BOOL4:
        return BaseType::Bool;
    case TokenType::KW_INT:
    case TokenType::KW_INT2:
    case TokenType::KW_INT3:
    case TokenType::KW_INT4:
        return BaseType::Int;
    case TokenType::KW_UINT:
    case TokenType::KW_UINT2:
    case TokenType::KW_UINT3:
    case TokenType::KW_UINT4:
        return BaseType::UInt;
    case TokenType::KW_FLOAT:
    case TokenType::KW_FLOAT2:
    case TokenType::KW_FLOAT3:
    case TokenType::KW_FLOAT4:
    case TokenType::KW_FLOAT2X2:
    case TokenType::KW_FLOAT3X3:
    case TokenType::KW_FLOAT4X4:
        return BaseType::Float;
    case TokenType::KW_HALF:
    case TokenType::KW_HALF2:
    case TokenType::KW_HALF3:
    case TokenType::KW_HALF4:
    case TokenType::KW_HALF2X2:
    case TokenType::KW_HALF3X3:
    case TokenType::KW_HALF4X4:
        return BaseType::Half;
    case TokenType::KW_FIXED:
    case TokenType::KW_FIXED2:
    case TokenType::KW_FIXED3:
    case TokenType::KW_FIXED4:
        return BaseType::Fixed;
    case TokenType::KW_CHAR:
    case TokenType::KW_CHAR2:
    case TokenType::KW_CHAR3:
    case TokenType::KW_CHAR4:
        return BaseType::Char;
    case TokenType::KW_UCHAR:
    case TokenType::KW_UCHAR2:
    case TokenType::KW_UCHAR3:
    case TokenType::KW_UCHAR4:
        return BaseType::UChar;
    case TokenType::KW_SHORT:
    case TokenType::KW_SHORT2:
    case TokenType::KW_SHORT3:
    case TokenType::KW_SHORT4:
        return BaseType::Short;
    case TokenType::KW_USHORT:
    case TokenType::KW_USHORT2:
    case TokenType::KW_USHORT3:
    case TokenType::KW_USHORT4:
        return BaseType::UShort;
    case TokenType::KW_SAMPLER1D:
        return BaseType::Sampler1D;
    case TokenType::KW_SAMPLER2D:
        return BaseType::Sampler2D;
    case TokenType::KW_SAMPLER3D:
        return BaseType::Sampler3D;
    case TokenType::KW_SAMPLERCUBE:
        return BaseType::SamplerCube;
    case TokenType::KW_SAMPLERRECT:
        return BaseType::SamplerRect;
    case TokenType::KW_ISAMPLER1D:
        return BaseType::ISampler1D;
    case TokenType::KW_ISAMPLER2D:
        return BaseType::ISampler2D;
    case TokenType::KW_ISAMPLER3D:
        return BaseType::ISampler3D;
    case TokenType::KW_ISAMPLERCUBE:
        return BaseType::ISamplerCube;
    case TokenType::KW_ISAMPLERRECT:
        return BaseType::ISamplerRect;
    case TokenType::KW_USAMPLER1D:
        return BaseType::USampler1D;
    case TokenType::KW_USAMPLER2D:
        return BaseType::USampler2D;
    case TokenType::KW_USAMPLER3D:
        return BaseType::USampler3D;
    case TokenType::KW_USAMPLERCUBE:
        return BaseType::USamplerCube;
    case TokenType::KW_USAMPLERRECT:
        return BaseType::USamplerRect;
    default:
        return BaseType::Void;
    }
}

// ============================================================================
// Declaration parsing
// ============================================================================

std::unique_ptr<DeclNode> Parser::parseTopLevelDeclaration()
{
    // Skip any stray semicolons
    while (match(TokenType::SEMICOLON))
    {
    }

    if (isAtEnd())
        return nullptr;

    // Check for struct
    if (check(TokenType::KW_STRUCT))
    {
        return parseStructDeclaration();
    }

    // Check for typedef
    if (check(TokenType::KW_TYPEDEF))
    {
        return parseTypedefDeclaration();
    }

    // Otherwise it's a variable or function declaration
    return parseVariableOrFunctionDeclaration();
}

std::unique_ptr<StructDecl> Parser::parseStructDeclaration()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_STRUCT, "Expected 'struct'");

    std::string name;
    if (check(TokenType::IDENTIFIER))
    {
        name = advance().lexeme;
    }

    auto structDecl = std::make_unique<StructDecl>(loc, name);

    consume(TokenType::LBRACE, "Expected '{' after struct name");

    while (!check(TokenType::RBRACE) && !isAtEnd())
    {
        // Parse struct member
        auto type = parseType();
        if (!type)
        {
            synchronize();
            continue;
        }

        if (!check(TokenType::IDENTIFIER))
        {
            error("Expected member name");
            synchronize();
            continue;
        }

        StructField field;
        field.name = advance().lexeme;
        field.type = type;

        // Check for array brackets after field name (e.g., float4 positions[8])
        while (check(TokenType::LBRACKET))
        {
            field.type = parseArrayType(field.type);
        }

        // Check for semantic
        if (match(TokenType::COLON))
        {
            field.semantic = parseSemantic();
        }

        structDecl->fields.push_back(field);

        consume(TokenType::SEMICOLON, "Expected ';' after struct member");
    }

    consume(TokenType::RBRACE, "Expected '}' after struct body");

    // Struct declarations are often followed by a semicolon or variable name
    if (check(TokenType::IDENTIFIER))
    {
        // This is a variable declaration using the struct type
        // For now, we just register the struct and skip this case
        // Full handling would create a VarDecl
        warning("Variable declaration after struct not fully supported yet");
    }

    consume(TokenType::SEMICOLON, "Expected ';' after struct declaration");

    return structDecl;
}

std::unique_ptr<TypedefDecl> Parser::parseTypedefDeclaration()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_TYPEDEF, "Expected 'typedef'");

    auto type = parseType();
    if (!type)
    {
        error("Expected type in typedef");
        return nullptr;
    }

    if (!check(TokenType::IDENTIFIER))
    {
        error("Expected name for typedef");
        return nullptr;
    }

    std::string name = advance().lexeme;

    consume(TokenType::SEMICOLON, "Expected ';' after typedef");

    return std::make_unique<TypedefDecl>(loc, name, type);
}

std::unique_ptr<DeclNode> Parser::parseVariableOrFunctionDeclaration()
{
    SourceLocation loc = currentLocation();

    // Parse storage qualifiers
    StorageQualifier storage = parseStorageQualifier();

    // Parse Vita attributes (can appear before type)
    VitaAttributes vitaAttrs = parseVitaAttributes();

    // Parse type
    auto type = parseType();
    if (!type)
    {
        error("Expected type");
        return nullptr;
    }

    // Parse name
    if (!check(TokenType::IDENTIFIER))
    {
        error("Expected identifier after type");
        return nullptr;
    }

    std::string name = advance().lexeme;

    // Check if this is a function (has '(') or variable
    if (check(TokenType::LPAREN))
    {
        auto funcDecl = parseFunctionDeclaration(loc, type, name, storage);
        return funcDecl;
    }
    else
    {
        // Could be multiple variable declarations: int a, b, c;
        auto vars = parseMultipleVariableDeclarations(loc, type, name, storage);
        if (vars.size() == 1)
        {
            vars[0]->vitaAttrs = vitaAttrs;
            return std::move(vars[0]);
        }
        else
        {
            // For multiple declarations, return the first and warn
            // (proper handling would need a declaration list node)
            if (!vars.empty())
            {
                vars[0]->vitaAttrs = vitaAttrs;
                return std::move(vars[0]);
            }
        }
    }

    return nullptr;
}

std::unique_ptr<FunctionDecl> Parser::parseFunctionDeclaration(
    SourceLocation loc,
    std::shared_ptr<TypeNode> returnType,
    const std::string& name,
    StorageQualifier storage)
{
    auto func = std::make_unique<FunctionDecl>(loc, name, returnType);

    // Parse parameters
    func->parameters = parseParameterList();

    // Check for return semantic
    if (match(TokenType::COLON))
    {
        func->returnSemantic = parseSemantic();
    }

    // Skip GCC-style __attribute__((...)) if present
    skipGccAttributes();

    // Function body or prototype
    if (check(TokenType::LBRACE))
    {
        func->body = parseBlock();
    }
    else
    {
        consume(TokenType::SEMICOLON, "Expected ';' after function prototype");
    }

    return func;
}

std::unique_ptr<VarDecl> Parser::parseVariableDeclaration(
    SourceLocation loc,
    std::shared_ptr<TypeNode> type,
    const std::string& name,
    StorageQualifier storage)
{
    auto var = std::make_unique<VarDecl>(loc, name, type);
    var->storage = storage;

    // Check for array brackets after name
    while (check(TokenType::LBRACKET))
    {
        var->type = parseArrayType(var->type);
    }

    // Check for semantic annotation
    if (match(TokenType::COLON))
    {
        var->semantic = parseSemantic();
    }

    // Check for initializer
    if (match(TokenType::OP_ASSIGN))
    {
        var->initializer = parseAssignmentExpression();
    }

    return var;
}

std::vector<std::unique_ptr<VarDecl>> Parser::parseMultipleVariableDeclarations(
    SourceLocation loc,
    std::shared_ptr<TypeNode> type,
    const std::string& firstName,
    StorageQualifier storage)
{
    std::vector<std::unique_ptr<VarDecl>> vars;

    // First variable
    vars.push_back(parseVariableDeclaration(loc, type, firstName, storage));

    // Additional variables (comma-separated)
    while (match(TokenType::COMMA))
    {
        if (!check(TokenType::IDENTIFIER))
        {
            error("Expected variable name after ','");
            break;
        }

        SourceLocation varLoc = currentLocation();
        std::string varName = advance().lexeme;
        vars.push_back(parseVariableDeclaration(varLoc, type, varName, storage));
    }

    consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
    return vars;
}

std::unique_ptr<ParamDecl> Parser::parseParameter()
{
    SourceLocation loc = currentLocation();

    // Parse storage qualifier (in/out/inout/uniform)
    StorageQualifier storage = StorageQualifier::In;  // default
    if (match(TokenType::KW_IN))
        storage = StorageQualifier::In;
    else if (match(TokenType::KW_OUT))
        storage = StorageQualifier::Out;
    else if (match(TokenType::KW_INOUT))
        storage = StorageQualifier::InOut;
    else if (match(TokenType::KW_UNIFORM))
        storage = StorageQualifier::Uniform;
    else if (match(TokenType::KW_CONST))
        storage = StorageQualifier::Const;

    // Parse type
    auto type = parseType();
    if (!type)
    {
        error("Expected parameter type");
        return nullptr;
    }

    // Parse name (optional for prototypes)
    std::string name;
    if (check(TokenType::IDENTIFIER))
    {
        name = advance().lexeme;
    }

    auto param = std::make_unique<ParamDecl>(loc, name, type);
    param->storage = storage;

    // Check for array brackets
    while (check(TokenType::LBRACKET))
    {
        param->type = parseArrayType(param->type);
    }

    // Check for semantic
    if (match(TokenType::COLON))
    {
        param->semantic = parseSemantic();
    }

    // Check for default value
    if (match(TokenType::OP_ASSIGN))
    {
        param->defaultValue = parseAssignmentExpression();
    }

    return param;
}

std::vector<std::unique_ptr<ParamDecl>> Parser::parseParameterList()
{
    std::vector<std::unique_ptr<ParamDecl>> params;

    consume(TokenType::LPAREN, "Expected '(' for parameter list");

    if (!check(TokenType::RPAREN))
    {
        // Check for void parameter (means no parameters)
        if (check(TokenType::KW_VOID) && peek(1).type == TokenType::RPAREN)
        {
            advance();  // consume 'void'
        }
        else
        {
            do
            {
                auto param = parseParameter();
                if (param)
                {
                    params.push_back(std::move(param));
                }
            } while (match(TokenType::COMMA));
        }
    }

    consume(TokenType::RPAREN, "Expected ')' after parameters");
    return params;
}

StorageQualifier Parser::parseStorageQualifier()
{
    if (match(TokenType::KW_UNIFORM))
        return StorageQualifier::Uniform;
    if (match(TokenType::KW_IN))
        return StorageQualifier::In;
    if (match(TokenType::KW_OUT))
        return StorageQualifier::Out;
    if (match(TokenType::KW_INOUT))
        return StorageQualifier::InOut;
    if (match(TokenType::KW_CONST))
    {
        // Handle "const static" combination
        if (match(TokenType::KW_STATIC))
            return StorageQualifier::Static;  // static const -> treat as static
        return StorageQualifier::Const;
    }
    if (match(TokenType::KW_STATIC))
    {
        // Handle "static const" combination
        match(TokenType::KW_CONST);  // consume const if present
        return StorageQualifier::Static;
    }
    if (match(TokenType::KW_EXTERN))
        return StorageQualifier::Extern;

    return StorageQualifier::None;
}

Semantic Parser::parseSemantic()
{
    Semantic sem;

    if (!check(TokenType::IDENTIFIER))
    {
        error("Expected semantic name");
        return sem;
    }

    std::string semName = advance().lexeme;
    sem.rawName = semName;  // preserve source spelling for byte-exact container emit

    // Parse semantic name and optional index (e.g., TEXCOORD0)
    // Try to extract trailing digit
    size_t digitStart = semName.length();
    while (digitStart > 0 && std::isdigit(semName[digitStart - 1]))
    {
        digitStart--;
    }

    if (digitStart < semName.length())
    {
        sem.name = semName.substr(0, digitStart);
        sem.index = std::stoi(semName.substr(digitStart));
    }
    else
    {
        sem.name = semName;
        sem.index = 0;
    }

    // Handle bracket syntax for index (e.g., BUFFER[0])
    if (match(TokenType::LBRACKET))
    {
        if (check(TokenType::NUMBER))
        {
            sem.index = std::stoi(advance().lexeme);
        }
        else
        {
            error("Expected integer index in semantic brackets");
        }
        consume(TokenType::RBRACKET, "Expected ']' after semantic index");
    }

    return sem;
}

VitaAttributes Parser::parseVitaAttributes()
{
    VitaAttributes attrs;

    while (true)
    {
        if (match(TokenType::KW_NOSTRIP))
        {
            attrs.noStrip = true;
        }
        else if (match(TokenType::KW_NATIVECOLOR))
        {
            attrs.nativeColor = true;
        }
        else if (match(TokenType::KW_MSAA))
        {
            attrs.msaaLevel = 1;  // generic MSAA
        }
        else if (match(TokenType::KW_MSAA2X))
        {
            attrs.msaaLevel = 2;
        }
        else if (match(TokenType::KW_MSAA4X))
        {
            attrs.msaaLevel = 4;
        }
        else if (match(TokenType::KW_REGFORMAT))
        {
            // __regformat(format)
            if (match(TokenType::LPAREN))
            {
                if (check(TokenType::IDENTIFIER) || check(TokenType::NUMBER))
                {
                    attrs.regFormat = advance().lexeme;
                }
                consume(TokenType::RPAREN, "Expected ')' after __regformat");
            }
        }
        else
        {
            break;
        }
    }

    return attrs;
}

void Parser::skipGccAttributes()
{
    // Skip any number of __attribute__((...)) clauses
    while (match(TokenType::KW_ATTRIBUTE))
    {
        // Expect ((
        if (!match(TokenType::LPAREN))
        {
            error("Expected '(' after __attribute__");
            return;
        }
        if (!match(TokenType::LPAREN))
        {
            error("Expected '((' after __attribute__");
            return;
        }

        // Skip everything until we find matching ))
        // We need to track parenthesis depth
        int parenDepth = 2;  // We've consumed ((
        while (!isAtEnd() && parenDepth > 0)
        {
            if (check(TokenType::LPAREN))
            {
                parenDepth++;
            }
            else if (check(TokenType::RPAREN))
            {
                parenDepth--;
            }
            advance();
        }

        // After the loop, we've consumed the final )
        // Check if we need to consume another ) for the outer paren
        // Actually the loop handles this - when parenDepth reaches 0, we've consumed ))
    }
}

// ============================================================================
// Statement parsing
// ============================================================================

std::unique_ptr<StmtNode> Parser::parseStatement()
{
    // Empty statement
    if (match(TokenType::SEMICOLON))
    {
        return std::make_unique<EmptyStmt>(currentLocation());
    }

    // Block
    if (check(TokenType::LBRACE))
    {
        return parseBlock();
    }

    // Control flow
    if (check(TokenType::KW_IF))
        return parseIfStatement();
    if (check(TokenType::KW_FOR))
        return parseForStatement();
    if (check(TokenType::KW_WHILE))
        return parseWhileStatement();
    if (check(TokenType::KW_DO))
        return parseDoWhileStatement();
    if (check(TokenType::KW_SWITCH))
        return parseSwitchStatement();
    if (check(TokenType::KW_RETURN))
        return parseReturnStatement();
    if (check(TokenType::KW_BREAK))
        return parseBreakStatement();
    if (check(TokenType::KW_CONTINUE))
        return parseContinueStatement();
    if (check(TokenType::KW_DISCARD))
        return parseDiscardStatement();

    // Case/default labels
    if (check(TokenType::KW_CASE))
    {
        SourceLocation loc = currentLocation();
        advance();
        auto value = parseExpression();
        consume(TokenType::COLON, "Expected ':' after case value");
        return std::make_unique<CaseStmt>(loc, std::move(value));
    }
    if (check(TokenType::KW_DEFAULT))
    {
        SourceLocation loc = currentLocation();
        advance();
        consume(TokenType::COLON, "Expected ':' after 'default'");
        return std::make_unique<DefaultStmt>(loc);
    }

    // Expression or declaration statement
    return parseExpressionOrDeclStatement();
}

std::unique_ptr<BlockStmt> Parser::parseBlock()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::LBRACE, "Expected '{'");

    auto block = std::make_unique<BlockStmt>(loc);

    while (!check(TokenType::RBRACE) && !isAtEnd())
    {
        auto stmt = parseStatement();
        if (stmt)
        {
            block->statements.push_back(std::move(stmt));
        }
    }

    consume(TokenType::RBRACE, "Expected '}'");
    return block;
}

std::unique_ptr<StmtNode> Parser::parseIfStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_IF, "Expected 'if'");
    consume(TokenType::LPAREN, "Expected '(' after 'if'");

    auto condition = parseExpression();

    consume(TokenType::RPAREN, "Expected ')' after if condition");

    auto thenBranch = parseStatement();

    std::unique_ptr<StmtNode> elseBranch;
    if (match(TokenType::KW_ELSE))
    {
        elseBranch = parseStatement();
    }

    return std::make_unique<IfStmt>(loc, std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<StmtNode> Parser::parseForStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_FOR, "Expected 'for'");
    consume(TokenType::LPAREN, "Expected '(' after 'for'");

    auto forStmt = std::make_unique<ForStmt>(loc);

    // Init
    if (!check(TokenType::SEMICOLON))
    {
        forStmt->init = parseExpressionOrDeclStatement();
    }
    else
    {
        advance();  // consume semicolon
    }

    // Condition
    if (!check(TokenType::SEMICOLON))
    {
        forStmt->condition = parseExpression();
    }
    consume(TokenType::SEMICOLON, "Expected ';' after for condition");

    // Increment
    if (!check(TokenType::RPAREN))
    {
        forStmt->increment = parseExpression();
    }

    consume(TokenType::RPAREN, "Expected ')' after for clauses");

    forStmt->body = parseStatement();

    return forStmt;
}

std::unique_ptr<StmtNode> Parser::parseWhileStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_WHILE, "Expected 'while'");
    consume(TokenType::LPAREN, "Expected '(' after 'while'");

    auto condition = parseExpression();

    consume(TokenType::RPAREN, "Expected ')' after while condition");

    auto body = parseStatement();

    return std::make_unique<WhileStmt>(loc, std::move(condition), std::move(body));
}

std::unique_ptr<StmtNode> Parser::parseDoWhileStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_DO, "Expected 'do'");

    auto body = parseStatement();

    consume(TokenType::KW_WHILE, "Expected 'while' after do body");
    consume(TokenType::LPAREN, "Expected '(' after 'while'");

    auto condition = parseExpression();

    consume(TokenType::RPAREN, "Expected ')' after while condition");
    consume(TokenType::SEMICOLON, "Expected ';' after do-while");

    return std::make_unique<DoWhileStmt>(loc, std::move(body), std::move(condition));
}

std::unique_ptr<StmtNode> Parser::parseSwitchStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_SWITCH, "Expected 'switch'");
    consume(TokenType::LPAREN, "Expected '(' after 'switch'");

    auto expr = parseExpression();

    consume(TokenType::RPAREN, "Expected ')' after switch expression");

    auto switchStmt = std::make_unique<SwitchStmt>(loc, std::move(expr));
    switchStmt->body = parseBlock();

    return switchStmt;
}

std::unique_ptr<StmtNode> Parser::parseReturnStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_RETURN, "Expected 'return'");

    std::unique_ptr<ExprNode> value;
    if (!check(TokenType::SEMICOLON))
    {
        value = parseExpression();
    }

    consume(TokenType::SEMICOLON, "Expected ';' after return");

    return std::make_unique<ReturnStmt>(loc, std::move(value));
}

std::unique_ptr<StmtNode> Parser::parseBreakStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_BREAK, "Expected 'break'");
    consume(TokenType::SEMICOLON, "Expected ';' after 'break'");
    return std::make_unique<BreakStmt>(loc);
}

std::unique_ptr<StmtNode> Parser::parseContinueStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_CONTINUE, "Expected 'continue'");
    consume(TokenType::SEMICOLON, "Expected ';' after 'continue'");
    return std::make_unique<ContinueStmt>(loc);
}

std::unique_ptr<StmtNode> Parser::parseDiscardStatement()
{
    SourceLocation loc = currentLocation();
    consume(TokenType::KW_DISCARD, "Expected 'discard'");
    consume(TokenType::SEMICOLON, "Expected ';' after 'discard'");
    return std::make_unique<DiscardStmt>(loc);
}

std::unique_ptr<StmtNode> Parser::parseExpressionOrDeclStatement()
{
    SourceLocation loc = currentLocation();

    // Check if this looks like a declaration (type followed by identifier)
    // This is tricky because "a * b" could be multiplication or pointer declaration
    // Cg doesn't have pointers, so we check for type names

    // First check storage qualifiers
    bool hasStorageQualifier = false;
    size_t savedPos = current;

    StorageQualifier storage = parseStorageQualifier();
    if (storage != StorageQualifier::None)
    {
        hasStorageQualifier = true;
    }

    // Check for Vita attributes
    VitaAttributes vitaAttrs = parseVitaAttributes();

    // Check for matrix layout qualifiers (row_major, column_major) - these indicate a type
    bool hasMatrixLayoutQualifier = check(TokenType::KW_ROW_MAJOR) || check(TokenType::KW_COLUMN_MAJOR);

    // Now check if this is a type name
    if (isTypeName() || hasMatrixLayoutQualifier)
    {
        // This looks like a declaration
        auto type = parseType();
        if (type && check(TokenType::IDENTIFIER))
        {
            std::string name = advance().lexeme;
            auto vars = parseMultipleVariableDeclarations(loc, type, name, storage);

            if (!vars.empty())
            {
                vars[0]->vitaAttrs = vitaAttrs;
                return std::make_unique<DeclStmt>(loc, std::move(vars[0]));
            }
        }
        else
        {
            // Type constructor expression? e.g., float4(1,2,3,4);
            // Reset and parse as expression
            current = savedPos;
        }
    }
    else if (hasStorageQualifier)
    {
        error("Storage qualifier must be followed by a type");
        return nullptr;
    }
    else
    {
        // Reset position if we consumed Vita attributes but it's not a declaration
        current = savedPos;
    }

    // Parse as expression statement
    auto expr = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after expression");

    return std::make_unique<ExprStmt>(loc, std::move(expr));
}

// ============================================================================
// Expression parsing (precedence climbing)
// ============================================================================

std::unique_ptr<ExprNode> Parser::parseExpression()
{
    return parseAssignmentExpression();
}

std::unique_ptr<ExprNode> Parser::parseAssignmentExpression()
{
    auto left = parseTernaryExpression();

    if (match({TokenType::OP_ASSIGN, TokenType::OP_PLUS_ASSIGN, TokenType::OP_MINUS_ASSIGN,
               TokenType::OP_MULTIPLY_ASSIGN, TokenType::OP_DIVIDE_ASSIGN, TokenType::OP_MODULO_ASSIGN}))
    {
        Token op = previous();
        SourceLocation loc = {op.filename, op.line, op.column};

        BinaryOp binOp;
        switch (op.type)
        {
        case TokenType::OP_ASSIGN:
            binOp = BinaryOp::Assign;
            break;
        case TokenType::OP_PLUS_ASSIGN:
            binOp = BinaryOp::AddAssign;
            break;
        case TokenType::OP_MINUS_ASSIGN:
            binOp = BinaryOp::SubAssign;
            break;
        case TokenType::OP_MULTIPLY_ASSIGN:
            binOp = BinaryOp::MulAssign;
            break;
        case TokenType::OP_DIVIDE_ASSIGN:
            binOp = BinaryOp::DivAssign;
            break;
        case TokenType::OP_MODULO_ASSIGN:
            binOp = BinaryOp::ModAssign;
            break;
        default:
            binOp = BinaryOp::Assign;
            break;
        }

        auto right = parseAssignmentExpression();  // Right associative
        return std::make_unique<BinaryExpr>(loc, binOp, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseTernaryExpression()
{
    auto condition = parseLogicalOrExpression();

    if (match(TokenType::QUESTION))
    {
        SourceLocation loc = currentLocation();
        auto thenExpr = parseExpression();
        consume(TokenType::COLON, "Expected ':' in ternary expression");
        auto elseExpr = parseTernaryExpression();
        return std::make_unique<TernaryExpr>(loc, std::move(condition), std::move(thenExpr), std::move(elseExpr));
    }

    return condition;
}

std::unique_ptr<ExprNode> Parser::parseLogicalOrExpression()
{
    auto left = parseLogicalAndExpression();

    while (match(TokenType::OP_LOGICAL_OR))
    {
        SourceLocation loc = {previous().filename, previous().line, previous().column};
        auto right = parseLogicalAndExpression();
        left = std::make_unique<BinaryExpr>(loc, BinaryOp::LogicalOr, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseLogicalAndExpression()
{
    auto left = parseBitwiseOrExpression();

    while (match(TokenType::OP_LOGICAL_AND))
    {
        SourceLocation loc = {previous().filename, previous().line, previous().column};
        auto right = parseBitwiseOrExpression();
        left = std::make_unique<BinaryExpr>(loc, BinaryOp::LogicalAnd, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseBitwiseOrExpression()
{
    auto left = parseBitwiseXorExpression();

    while (match(TokenType::OP_BITWISE_OR))
    {
        SourceLocation loc = {previous().filename, previous().line, previous().column};
        auto right = parseBitwiseXorExpression();
        left = std::make_unique<BinaryExpr>(loc, BinaryOp::BitwiseOr, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseBitwiseXorExpression()
{
    auto left = parseBitwiseAndExpression();

    while (match(TokenType::OP_BITWISE_XOR))
    {
        SourceLocation loc = {previous().filename, previous().line, previous().column};
        auto right = parseBitwiseAndExpression();
        left = std::make_unique<BinaryExpr>(loc, BinaryOp::BitwiseXor, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseBitwiseAndExpression()
{
    auto left = parseEqualityExpression();

    while (match(TokenType::OP_BITWISE_AND))
    {
        SourceLocation loc = {previous().filename, previous().line, previous().column};
        auto right = parseEqualityExpression();
        left = std::make_unique<BinaryExpr>(loc, BinaryOp::BitwiseAnd, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseEqualityExpression()
{
    auto left = parseRelationalExpression();

    while (match({TokenType::OP_EQUAL, TokenType::OP_NOT_EQUAL}))
    {
        Token op = previous();
        SourceLocation loc = {op.filename, op.line, op.column};
        BinaryOp binOp = (op.type == TokenType::OP_EQUAL) ? BinaryOp::Equal : BinaryOp::NotEqual;
        auto right = parseRelationalExpression();
        left = std::make_unique<BinaryExpr>(loc, binOp, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseRelationalExpression()
{
    auto left = parseShiftExpression();

    while (match({TokenType::OP_LESS, TokenType::OP_LESS_EQUAL, TokenType::OP_GREATER, TokenType::OP_GREATER_EQUAL}))
    {
        Token op = previous();
        SourceLocation loc = {op.filename, op.line, op.column};

        BinaryOp binOp;
        switch (op.type)
        {
        case TokenType::OP_LESS:
            binOp = BinaryOp::Less;
            break;
        case TokenType::OP_LESS_EQUAL:
            binOp = BinaryOp::LessEqual;
            break;
        case TokenType::OP_GREATER:
            binOp = BinaryOp::Greater;
            break;
        case TokenType::OP_GREATER_EQUAL:
            binOp = BinaryOp::GreaterEqual;
            break;
        default:
            binOp = BinaryOp::Less;
            break;
        }

        auto right = parseShiftExpression();
        left = std::make_unique<BinaryExpr>(loc, binOp, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseShiftExpression()
{
    auto left = parseAdditiveExpression();

    while (match({TokenType::OP_SHIFT_LEFT, TokenType::OP_SHIFT_RIGHT}))
    {
        Token op = previous();
        SourceLocation loc = {op.filename, op.line, op.column};
        BinaryOp binOp = (op.type == TokenType::OP_SHIFT_LEFT) ? BinaryOp::ShiftLeft : BinaryOp::ShiftRight;
        auto right = parseAdditiveExpression();
        left = std::make_unique<BinaryExpr>(loc, binOp, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseAdditiveExpression()
{
    auto left = parseMultiplicativeExpression();

    while (match({TokenType::OP_PLUS, TokenType::OP_MINUS}))
    {
        Token op = previous();
        SourceLocation loc = {op.filename, op.line, op.column};
        BinaryOp binOp = (op.type == TokenType::OP_PLUS) ? BinaryOp::Add : BinaryOp::Sub;
        auto right = parseMultiplicativeExpression();
        left = std::make_unique<BinaryExpr>(loc, binOp, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseMultiplicativeExpression()
{
    auto left = parseUnaryExpression();

    while (match({TokenType::OP_MULTIPLY, TokenType::OP_DIVIDE, TokenType::OP_MODULO}))
    {
        Token op = previous();
        SourceLocation loc = {op.filename, op.line, op.column};

        BinaryOp binOp;
        switch (op.type)
        {
        case TokenType::OP_MULTIPLY:
            binOp = BinaryOp::Mul;
            break;
        case TokenType::OP_DIVIDE:
            binOp = BinaryOp::Div;
            break;
        case TokenType::OP_MODULO:
            binOp = BinaryOp::Mod;
            break;
        default:
            binOp = BinaryOp::Mul;
            break;
        }

        auto right = parseUnaryExpression();
        left = std::make_unique<BinaryExpr>(loc, binOp, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<ExprNode> Parser::parseUnaryExpression()
{
    // Prefix operators
    if (match({TokenType::OP_MINUS, TokenType::OP_LOGICAL_NOT, TokenType::OP_BITWISE_NOT,
               TokenType::OP_INCREMENT, TokenType::OP_DECREMENT}))
    {
        Token op = previous();
        SourceLocation loc = {op.filename, op.line, op.column};

        UnaryOp unOp;
        switch (op.type)
        {
        case TokenType::OP_MINUS:
            unOp = UnaryOp::Negate;
            break;
        case TokenType::OP_LOGICAL_NOT:
            unOp = UnaryOp::LogicalNot;
            break;
        case TokenType::OP_BITWISE_NOT:
            unOp = UnaryOp::BitwiseNot;
            break;
        case TokenType::OP_INCREMENT:
            unOp = UnaryOp::PreIncrement;
            break;
        case TokenType::OP_DECREMENT:
            unOp = UnaryOp::PreDecrement;
            break;
        default:
            unOp = UnaryOp::Negate;
            break;
        }

        auto operand = parseUnaryExpression();
        return std::make_unique<UnaryExpr>(loc, unOp, std::move(operand));
    }

    // sizeof
    if (match(TokenType::KW_SIZEOF))
    {
        SourceLocation loc = currentLocation();
        consume(TokenType::LPAREN, "Expected '(' after 'sizeof'");

        // Could be sizeof(type) or sizeof(expr)
        if (isTypeName())
        {
            auto type = parseType();
            consume(TokenType::RPAREN, "Expected ')' after sizeof type");
            return std::make_unique<SizeofExpr>(loc, type);
        }
        else
        {
            auto expr = parseExpression();
            consume(TokenType::RPAREN, "Expected ')' after sizeof expression");
            return std::make_unique<SizeofExpr>(loc, std::move(expr));
        }
    }

    // Cast expression: (type)expr
    if (check(TokenType::LPAREN))
    {
        // Look ahead to see if this is a cast
        size_t savedPos = current;
        advance();  // consume '('

        if (isTypeName())
        {
            auto type = parseType();
            if (check(TokenType::RPAREN))
            {
                SourceLocation loc = currentLocation();
                advance();  // consume ')'
                auto operand = parseUnaryExpression();
                return std::make_unique<CastExpr>(loc, type, std::move(operand));
            }
        }

        // Not a cast, restore position
        current = savedPos;
    }

    return parsePostfixExpression();
}

std::unique_ptr<ExprNode> Parser::parsePostfixExpression()
{
    auto expr = parsePrimaryExpression();

    while (true)
    {
        if (match(TokenType::LBRACKET))
        {
            // Array indexing
            SourceLocation loc = currentLocation();
            auto index = parseExpression();
            consume(TokenType::RBRACKET, "Expected ']' after array index");
            expr = std::make_unique<IndexExpr>(loc, std::move(expr), std::move(index));
        }
        else if (match(TokenType::DOT))
        {
            // Member access / swizzle
            SourceLocation loc = currentLocation();
            if (!check(TokenType::IDENTIFIER))
            {
                error("Expected member name after '.'");
                break;
            }
            std::string member = advance().lexeme;
            expr = std::make_unique<MemberAccessExpr>(loc, std::move(expr), member);
        }
        else if (match(TokenType::OP_INCREMENT))
        {
            // Post-increment
            SourceLocation loc = {previous().filename, previous().line, previous().column};
            expr = std::make_unique<UnaryExpr>(loc, UnaryOp::PostIncrement, std::move(expr));
        }
        else if (match(TokenType::OP_DECREMENT))
        {
            // Post-decrement
            SourceLocation loc = {previous().filename, previous().line, previous().column};
            expr = std::make_unique<UnaryExpr>(loc, UnaryOp::PostDecrement, std::move(expr));
        }
        else
        {
            break;
        }
    }

    return expr;
}

std::unique_ptr<ExprNode> Parser::parsePrimaryExpression()
{
    SourceLocation loc = currentLocation();

    // Number literal
    if (check(TokenType::NUMBER))
    {
        return parseNumberLiteral();
    }

    // String literal (rarely used in shaders, but supported)
    if (check(TokenType::STRING))
    {
        // String literals not typically used in shader code
        error("String literals not supported in shader code");
        advance();
        return std::make_unique<LiteralExpr>(loc, static_cast<int64_t>(0));
    }

    // Boolean literals (true/false are identifiers in lexer, handle specially)
    if (check(TokenType::IDENTIFIER))
    {
        const std::string& lexeme = peek().lexeme;
        if (lexeme == "true")
        {
            advance();
            return std::make_unique<LiteralExpr>(loc, true);
        }
        if (lexeme == "false")
        {
            advance();
            return std::make_unique<LiteralExpr>(loc, false);
        }
    }

    // Type constructor: float4(1, 2, 3, 4)
    if (isTypeName())
    {
        auto type = parseType();
        if (check(TokenType::LPAREN))
        {
            return parseConstructorExpression(type);
        }
        // Not followed by '(', this shouldn't happen in expression context
        error("Expected '(' after type in expression");
        return nullptr;
    }

    // Identifier or function call
    if (check(TokenType::IDENTIFIER))
    {
        std::string name = advance().lexeme;

        if (check(TokenType::LPAREN))
        {
            // Function call
            return parseCallExpression(loc, name);
        }
        else
        {
            // Variable reference
            return std::make_unique<IdentifierExpr>(loc, name);
        }
    }

    // Parenthesized expression
    if (match(TokenType::LPAREN))
    {
        auto expr = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after expression");
        return expr;
    }

    error("Expected expression");
    return nullptr;
}

std::unique_ptr<ExprNode> Parser::parseCallExpression(SourceLocation loc, const std::string& name)
{
    auto call = std::make_unique<CallExpr>(loc, name);
    call->arguments = parseArgumentList();
    return call;
}

std::unique_ptr<ExprNode> Parser::parseConstructorExpression(std::shared_ptr<TypeNode> type)
{
    SourceLocation loc = currentLocation();
    auto ctor = std::make_unique<ConstructorExpr>(loc, type);

    consume(TokenType::LPAREN, "Expected '(' in type constructor");

    if (!check(TokenType::RPAREN))
    {
        do
        {
            ctor->arguments.push_back(parseAssignmentExpression());
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::RPAREN, "Expected ')' after constructor arguments");

    return ctor;
}

std::vector<std::unique_ptr<ExprNode>> Parser::parseArgumentList()
{
    std::vector<std::unique_ptr<ExprNode>> args;

    consume(TokenType::LPAREN, "Expected '(' for argument list");

    if (!check(TokenType::RPAREN))
    {
        do
        {
            args.push_back(parseAssignmentExpression());
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::RPAREN, "Expected ')' after arguments");

    return args;
}

// ============================================================================
// Literal parsing
// ============================================================================

std::unique_ptr<LiteralExpr> Parser::parseNumberLiteral()
{
    SourceLocation loc = currentLocation();
    Token tok = advance();

    std::string lexeme = tok.lexeme;
    std::string suffix;

    // Extract suffix (f, h, etc.)
    size_t suffixStart = lexeme.length();
    while (suffixStart > 0 && std::isalpha(lexeme[suffixStart - 1]))
    {
        suffixStart--;
    }
    if (suffixStart < lexeme.length())
    {
        suffix = lexeme.substr(suffixStart);
        lexeme = lexeme.substr(0, suffixStart);
    }

    // Check for float (has decimal point or exponent)
    bool isFloat = (lexeme.find('.') != std::string::npos) ||
                   (lexeme.find('e') != std::string::npos) ||
                   (lexeme.find('E') != std::string::npos) ||
                   (suffix == "f" || suffix == "F" || suffix == "h" || suffix == "H");

    if (isFloat)
    {
        double value = std::stod(lexeme);
        return std::make_unique<LiteralExpr>(loc, value, suffix);
    }
    else
    {
        // Integer - handle hex (0x), octal (0), decimal
        int64_t value = 0;
        if (lexeme.length() > 2 && lexeme[0] == '0' && (lexeme[1] == 'x' || lexeme[1] == 'X'))
        {
            value = std::stoll(lexeme, nullptr, 16);
        }
        else if (lexeme.length() > 1 && lexeme[0] == '0')
        {
            value = std::stoll(lexeme, nullptr, 8);
        }
        else
        {
            value = std::stoll(lexeme);
        }
        return std::make_unique<LiteralExpr>(loc, value);
    }
}

// ============================================================================
// Convenience function
// ============================================================================

std::unique_ptr<TranslationUnit> parseShaderSource(
    const std::string& source,
    const std::string& filename,
    std::vector<ParseError>* outErrors)
{
    Lexer lexer(source, filename);
    auto tokens = lexer.tokenize();

    Parser parser(tokens, filename);
    auto unit = parser.parse();

    if (outErrors)
    {
        *outErrors = parser.getErrors();
    }

    return unit;
}

// ============================================================================
// Type utility implementations
// ============================================================================

bool TypeNode::isSampler() const
{
    switch (baseType)
    {
    case BaseType::Sampler1D:
    case BaseType::Sampler2D:
    case BaseType::Sampler3D:
    case BaseType::SamplerCube:
    case BaseType::SamplerRect:
    case BaseType::ISampler1D:
    case BaseType::ISampler2D:
    case BaseType::ISampler3D:
    case BaseType::ISamplerCube:
    case BaseType::ISamplerRect:
    case BaseType::USampler1D:
    case BaseType::USampler2D:
    case BaseType::USampler3D:
    case BaseType::USamplerCube:
    case BaseType::USamplerRect:
        return true;
    default:
        return false;
    }
}

bool TypeNode::isNumeric() const
{
    if (isSampler() || baseType == BaseType::Void || baseType == BaseType::Struct)
        return false;
    return true;
}

bool TypeNode::isIntegral() const
{
    switch (baseType)
    {
    case BaseType::Bool:
    case BaseType::Int:
    case BaseType::UInt:
    case BaseType::Char:
    case BaseType::UChar:
    case BaseType::Short:
    case BaseType::UShort:
        return true;
    default:
        return false;
    }
}

bool TypeNode::isFloatingPoint() const
{
    switch (baseType)
    {
    case BaseType::Float:
    case BaseType::Half:
    case BaseType::Fixed:
        return true;
    default:
        return false;
    }
}

int TypeNode::componentCount() const
{
    if (matrixRows > 0)
        return matrixRows * matrixCols;
    return vectorSize;
}

std::string TypeNode::toString() const
{
    std::string result = baseTypeToString(baseType);

    if (vectorSize > 1 && matrixRows == 0)
    {
        result += std::to_string(vectorSize);
    }
    else if (matrixRows > 0)
    {
        result += std::to_string(matrixRows) + "x" + std::to_string(matrixCols);
    }

    if (arraySize > 0)
    {
        result += "[" + std::to_string(arraySize) + "]";
    }

    return result;
}

// Factory methods
std::shared_ptr<TypeNode> TypeNode::makeVoid()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Void;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeBool()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Bool;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeInt()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Int;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeUInt()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::UInt;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeFloat()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Float;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeFloat2()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Float;
    t->vectorSize = 2;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeFloat3()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Float;
    t->vectorSize = 3;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeFloat4()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Float;
    t->vectorSize = 4;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeFloat4x4()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Float;
    t->matrixRows = 4;
    t->matrixCols = 4;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeHalf()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Half;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeHalf4()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Half;
    t->vectorSize = 4;
    return t;
}

std::shared_ptr<TypeNode> TypeNode::makeSampler2D()
{
    auto t = std::make_shared<TypeNode>();
    t->baseType = BaseType::Sampler2D;
    return t;
}

// ============================================================================
// Helper function implementations
// ============================================================================

std::string binaryOpToString(BinaryOp op)
{
    switch (op)
    {
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Sub:
        return "-";
    case BinaryOp::Mul:
        return "*";
    case BinaryOp::Div:
        return "/";
    case BinaryOp::Mod:
        return "%";
    case BinaryOp::Equal:
        return "==";
    case BinaryOp::NotEqual:
        return "!=";
    case BinaryOp::Less:
        return "<";
    case BinaryOp::LessEqual:
        return "<=";
    case BinaryOp::Greater:
        return ">";
    case BinaryOp::GreaterEqual:
        return ">=";
    case BinaryOp::LogicalAnd:
        return "&&";
    case BinaryOp::LogicalOr:
        return "||";
    case BinaryOp::BitwiseAnd:
        return "&";
    case BinaryOp::BitwiseOr:
        return "|";
    case BinaryOp::BitwiseXor:
        return "^";
    case BinaryOp::ShiftLeft:
        return "<<";
    case BinaryOp::ShiftRight:
        return ">>";
    case BinaryOp::Assign:
        return "=";
    case BinaryOp::AddAssign:
        return "+=";
    case BinaryOp::SubAssign:
        return "-=";
    case BinaryOp::MulAssign:
        return "*=";
    case BinaryOp::DivAssign:
        return "/=";
    case BinaryOp::ModAssign:
        return "%=";
    case BinaryOp::Comma:
        return ",";
    default:
        return "?";
    }
}

std::string unaryOpToString(UnaryOp op)
{
    switch (op)
    {
    case UnaryOp::Negate:
        return "-";
    case UnaryOp::LogicalNot:
        return "!";
    case UnaryOp::BitwiseNot:
        return "~";
    case UnaryOp::PreIncrement:
        return "++";
    case UnaryOp::PreDecrement:
        return "--";
    case UnaryOp::PostIncrement:
        return "++";
    case UnaryOp::PostDecrement:
        return "--";
    default:
        return "?";
    }
}

std::string storageQualifierToString(StorageQualifier sq)
{
    switch (sq)
    {
    case StorageQualifier::None:
        return "";
    case StorageQualifier::Uniform:
        return "uniform";
    case StorageQualifier::In:
        return "in";
    case StorageQualifier::Out:
        return "out";
    case StorageQualifier::InOut:
        return "inout";
    case StorageQualifier::Const:
        return "const";
    case StorageQualifier::Static:
        return "static";
    case StorageQualifier::Extern:
        return "extern";
    default:
        return "?";
    }
}

std::string baseTypeToString(BaseType bt)
{
    switch (bt)
    {
    case BaseType::Void:
        return "void";
    case BaseType::Bool:
        return "bool";
    case BaseType::Int:
        return "int";
    case BaseType::UInt:
        return "uint";
    case BaseType::Float:
        return "float";
    case BaseType::Half:
        return "half";
    case BaseType::Fixed:
        return "fixed";
    case BaseType::Char:
        return "char";
    case BaseType::UChar:
        return "uchar";
    case BaseType::Short:
        return "short";
    case BaseType::UShort:
        return "ushort";
    case BaseType::Sampler1D:
        return "sampler1D";
    case BaseType::Sampler2D:
        return "sampler2D";
    case BaseType::Sampler3D:
        return "sampler3D";
    case BaseType::SamplerCube:
        return "samplerCUBE";
    case BaseType::SamplerRect:
        return "samplerRECT";
    case BaseType::ISampler1D:
        return "isampler1D";
    case BaseType::ISampler2D:
        return "isampler2D";
    case BaseType::ISampler3D:
        return "isampler3D";
    case BaseType::ISamplerCube:
        return "isamplerCUBE";
    case BaseType::ISamplerRect:
        return "isamplerRECT";
    case BaseType::USampler1D:
        return "usampler1D";
    case BaseType::USampler2D:
        return "usampler2D";
    case BaseType::USampler3D:
        return "usampler3D";
    case BaseType::USamplerCube:
        return "usamplerCUBE";
    case BaseType::USamplerRect:
        return "usamplerRECT";
    case BaseType::Struct:
        return "struct";
    case BaseType::Array:
        return "array";
    default:
        return "?";
    }
}

// ============================================================================
// AST Printer implementation
// ============================================================================

void AstPrinter::indent(int level)
{
    for (int i = 0; i < level; i++)
        std::cout << "  ";
}

void AstPrinter::print(const TranslationUnit& unit)
{
    std::cout << "TranslationUnit: " << unit.filename << "\n";
    for (const auto& decl : unit.declarations)
    {
        print(decl.get(), 1);
    }
    if (unit.entryPoint)
    {
        std::cout << "Entry point: " << unit.entryPoint->name << "\n";
    }
}

void AstPrinter::print(const DeclNode* decl, int ind)
{
    if (!decl)
        return;

    indent(ind);

    switch (decl->kind)
    {
    case DeclKind::Variable:
    {
        auto* var = static_cast<const VarDecl*>(decl);
        std::cout << "VarDecl: " << var->name;
        if (var->type)
            std::cout << " : " << var->type->toString();
        if (var->storage != StorageQualifier::None)
            std::cout << " [" << storageQualifierToString(var->storage) << "]";
        if (!var->semantic.isEmpty())
            std::cout << " : " << var->semantic.name << var->semantic.index;
        std::cout << "\n";
        if (var->initializer)
        {
            indent(ind + 1);
            std::cout << "= ";
            print(var->initializer.get(), 0);
            std::cout << "\n";
        }
        break;
    }
    case DeclKind::Function:
    {
        auto* func = static_cast<const FunctionDecl*>(decl);
        std::cout << "FunctionDecl: " << func->name;
        if (func->returnType)
            std::cout << " -> " << func->returnType->toString();
        if (!func->returnSemantic.isEmpty())
            std::cout << " : " << func->returnSemantic.name;
        std::cout << "\n";

        for (const auto& param : func->parameters)
        {
            indent(ind + 1);
            std::cout << "Param: " << param->name;
            if (param->type)
                std::cout << " : " << param->type->toString();
            if (!param->semantic.isEmpty())
                std::cout << " : " << param->semantic.name << param->semantic.index;
            std::cout << "\n";
        }

        if (func->body)
        {
            print(func->body.get(), ind + 1);
        }
        break;
    }
    case DeclKind::Struct:
    {
        auto* str = static_cast<const StructDecl*>(decl);
        std::cout << "StructDecl: " << str->name << "\n";
        for (const auto& field : str->fields)
        {
            indent(ind + 1);
            std::cout << "Field: " << field.name;
            if (field.type)
                std::cout << " : " << field.type->toString();
            std::cout << "\n";
        }
        break;
    }
    case DeclKind::Typedef:
    {
        auto* td = static_cast<const TypedefDecl*>(decl);
        std::cout << "TypedefDecl: " << td->name;
        if (td->aliasedType)
            std::cout << " = " << td->aliasedType->toString();
        std::cout << "\n";
        break;
    }
    default:
        std::cout << "UnknownDecl\n";
        break;
    }
}

void AstPrinter::print(const StmtNode* stmt, int ind)
{
    if (!stmt)
        return;

    indent(ind);

    switch (stmt->kind)
    {
    case StmtKind::Block:
    {
        auto* block = static_cast<const BlockStmt*>(stmt);
        std::cout << "Block\n";
        for (const auto& s : block->statements)
        {
            print(s.get(), ind + 1);
        }
        break;
    }
    case StmtKind::Expr:
    {
        auto* es = static_cast<const ExprStmt*>(stmt);
        std::cout << "ExprStmt: ";
        print(es->expr.get(), 0);
        std::cout << "\n";
        break;
    }
    case StmtKind::Return:
    {
        auto* ret = static_cast<const ReturnStmt*>(stmt);
        std::cout << "Return";
        if (ret->value)
        {
            std::cout << " ";
            print(ret->value.get(), 0);
        }
        std::cout << "\n";
        break;
    }
    case StmtKind::If:
    {
        auto* ifs = static_cast<const IfStmt*>(stmt);
        std::cout << "If\n";
        indent(ind + 1);
        std::cout << "Cond: ";
        print(ifs->condition.get(), 0);
        std::cout << "\n";
        indent(ind + 1);
        std::cout << "Then:\n";
        print(ifs->thenBranch.get(), ind + 2);
        if (ifs->elseBranch)
        {
            indent(ind + 1);
            std::cout << "Else:\n";
            print(ifs->elseBranch.get(), ind + 2);
        }
        break;
    }
    case StmtKind::For:
    {
        auto* fs = static_cast<const ForStmt*>(stmt);
        std::cout << "For\n";
        if (fs->init)
        {
            indent(ind + 1);
            std::cout << "Init:\n";
            print(fs->init.get(), ind + 2);
        }
        if (fs->condition)
        {
            indent(ind + 1);
            std::cout << "Cond: ";
            print(fs->condition.get(), 0);
            std::cout << "\n";
        }
        if (fs->increment)
        {
            indent(ind + 1);
            std::cout << "Inc: ";
            print(fs->increment.get(), 0);
            std::cout << "\n";
        }
        indent(ind + 1);
        std::cout << "Body:\n";
        print(fs->body.get(), ind + 2);
        break;
    }
    case StmtKind::Discard:
        std::cout << "Discard\n";
        break;
    case StmtKind::Break:
        std::cout << "Break\n";
        break;
    case StmtKind::Continue:
        std::cout << "Continue\n";
        break;
    default:
        std::cout << "Stmt\n";
        break;
    }
}

void AstPrinter::print(const ExprNode* expr, int ind)
{
    if (!expr)
    {
        std::cout << "<null>";
        return;
    }

    switch (expr->kind)
    {
    case ExprKind::Literal:
    {
        auto* lit = static_cast<const LiteralExpr*>(expr);
        switch (lit->literalKind)
        {
        case LiteralExpr::LiteralKind::Int:
            std::cout << std::get<int64_t>(lit->value);
            break;
        case LiteralExpr::LiteralKind::Float:
            std::cout << std::get<double>(lit->value);
            break;
        case LiteralExpr::LiteralKind::Bool:
            std::cout << (std::get<bool>(lit->value) ? "true" : "false");
            break;
        }
        break;
    }
    case ExprKind::Identifier:
    {
        auto* id = static_cast<const IdentifierExpr*>(expr);
        std::cout << id->name;
        break;
    }
    case ExprKind::Binary:
    {
        auto* bin = static_cast<const BinaryExpr*>(expr);
        std::cout << "(";
        print(bin->left.get(), 0);
        std::cout << " " << binaryOpToString(bin->op) << " ";
        print(bin->right.get(), 0);
        std::cout << ")";
        break;
    }
    case ExprKind::Unary:
    {
        auto* un = static_cast<const UnaryExpr*>(expr);
        if (un->op == UnaryOp::PostIncrement || un->op == UnaryOp::PostDecrement)
        {
            print(un->operand.get(), 0);
            std::cout << unaryOpToString(un->op);
        }
        else
        {
            std::cout << unaryOpToString(un->op);
            print(un->operand.get(), 0);
        }
        break;
    }
    case ExprKind::Call:
    {
        auto* call = static_cast<const CallExpr*>(expr);
        std::cout << call->functionName << "(";
        for (size_t i = 0; i < call->arguments.size(); i++)
        {
            if (i > 0)
                std::cout << ", ";
            print(call->arguments[i].get(), 0);
        }
        std::cout << ")";
        break;
    }
    case ExprKind::MemberAccess:
    {
        auto* mem = static_cast<const MemberAccessExpr*>(expr);
        print(mem->object.get(), 0);
        std::cout << "." << mem->member;
        break;
    }
    case ExprKind::Index:
    {
        auto* idx = static_cast<const IndexExpr*>(expr);
        print(idx->array.get(), 0);
        std::cout << "[";
        print(idx->index.get(), 0);
        std::cout << "]";
        break;
    }
    case ExprKind::Ternary:
    {
        auto* ter = static_cast<const TernaryExpr*>(expr);
        std::cout << "(";
        print(ter->condition.get(), 0);
        std::cout << " ? ";
        print(ter->thenExpr.get(), 0);
        std::cout << " : ";
        print(ter->elseExpr.get(), 0);
        std::cout << ")";
        break;
    }
    case ExprKind::Constructor:
    {
        auto* ctor = static_cast<const ConstructorExpr*>(expr);
        if (ctor->constructedType)
            std::cout << ctor->constructedType->toString();
        std::cout << "(";
        for (size_t i = 0; i < ctor->arguments.size(); i++)
        {
            if (i > 0)
                std::cout << ", ";
            print(ctor->arguments[i].get(), 0);
        }
        std::cout << ")";
        break;
    }
    case ExprKind::Cast:
    {
        auto* cast = static_cast<const CastExpr*>(expr);
        std::cout << "(";
        if (cast->targetType)
            std::cout << cast->targetType->toString();
        std::cout << ")";
        print(cast->operand.get(), 0);
        break;
    }
    default:
        std::cout << "<expr>";
        break;
    }
}

void AstPrinter::print(const TypeNode* type)
{
    if (type)
        std::cout << type->toString();
    else
        std::cout << "<null>";
}
