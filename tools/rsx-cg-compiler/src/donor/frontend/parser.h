#pragma once

#include "lexer.h"
#include "ast.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_set>
#include <functional>

// Parser error information
struct ParseError
{
    SourceLocation loc;
    std::string message;
    bool isWarning = false;
};

// Parser configuration
struct ParserConfig
{
    bool verboseErrors = true;
    bool continueOnError = true;  // Try to recover and continue parsing
    int maxErrors = 50;
};

// Recursive descent parser for Cg shader language
class Parser
{
public:
    explicit Parser(const std::vector<Token>& tokens, const std::string& filename = "<input>");

    // Main entry point - parse entire translation unit
    std::unique_ptr<TranslationUnit> parse();

    // Configuration
    void setConfig(const ParserConfig& config) { this->config = config; }

    // Error access
    bool hasErrors() const { return !errors.empty(); }
    const std::vector<ParseError>& getErrors() const { return errors; }
    void printErrors() const;

private:
    // Token stream
    std::vector<Token> tokens;
    size_t current = 0;
    std::string filename;

    // Configuration and state
    ParserConfig config;
    std::vector<ParseError> errors;
    bool panicMode = false;

    // Known type names (built-in + user-defined structs/typedefs)
    std::unordered_set<std::string> typeNames;

    // ========================================================================
    // Token utilities
    // ========================================================================

    bool isAtEnd() const;
    const Token& peek(int offset = 0) const;
    const Token& previous() const;
    const Token& advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    bool match(std::initializer_list<TokenType> types);
    Token consume(TokenType type, const std::string& message);
    SourceLocation currentLocation() const;

    // ========================================================================
    // Error handling
    // ========================================================================

    void error(const std::string& message);
    void error(const SourceLocation& loc, const std::string& message);
    void warning(const std::string& message);
    void synchronize();  // Error recovery - skip to next statement/declaration boundary

    // ========================================================================
    // Type parsing
    // ========================================================================

    bool isTypeName() const;
    bool isTypeStart() const;
    std::shared_ptr<TypeNode> parseType();
    std::shared_ptr<TypeNode> parseBaseType();
    std::shared_ptr<TypeNode> parseArrayType(std::shared_ptr<TypeNode> elementType);
    BaseType tokenToBaseType(TokenType type) const;

    // ========================================================================
    // Declaration parsing
    // ========================================================================

    std::unique_ptr<DeclNode> parseDeclaration();
    std::unique_ptr<DeclNode> parseTopLevelDeclaration();
    std::unique_ptr<StructDecl> parseStructDeclaration();
    std::unique_ptr<TypedefDecl> parseTypedefDeclaration();
    std::unique_ptr<DeclNode> parseVariableOrFunctionDeclaration();
    std::unique_ptr<FunctionDecl> parseFunctionDeclaration(
        SourceLocation loc,
        std::shared_ptr<TypeNode> returnType,
        const std::string& name,
        StorageQualifier storage);
    std::unique_ptr<VarDecl> parseVariableDeclaration(
        SourceLocation loc,
        std::shared_ptr<TypeNode> type,
        const std::string& name,
        StorageQualifier storage);
    std::vector<std::unique_ptr<VarDecl>> parseMultipleVariableDeclarations(
        SourceLocation loc,
        std::shared_ptr<TypeNode> type,
        const std::string& firstName,
        StorageQualifier storage);
    std::unique_ptr<ParamDecl> parseParameter();
    std::vector<std::unique_ptr<ParamDecl>> parseParameterList();

    // Storage qualifiers and attributes
    StorageQualifier parseStorageQualifier();
    Semantic parseSemantic();
    VitaAttributes parseVitaAttributes();
    void skipGccAttributes();  // Skip __attribute__((...)) clauses

    // ========================================================================
    // Statement parsing
    // ========================================================================

    std::unique_ptr<StmtNode> parseStatement();
    std::unique_ptr<BlockStmt> parseBlock();
    std::unique_ptr<StmtNode> parseIfStatement();
    std::unique_ptr<StmtNode> parseForStatement();
    std::unique_ptr<StmtNode> parseWhileStatement();
    std::unique_ptr<StmtNode> parseDoWhileStatement();
    std::unique_ptr<StmtNode> parseSwitchStatement();
    std::unique_ptr<StmtNode> parseReturnStatement();
    std::unique_ptr<StmtNode> parseBreakStatement();
    std::unique_ptr<StmtNode> parseContinueStatement();
    std::unique_ptr<StmtNode> parseDiscardStatement();
    std::unique_ptr<StmtNode> parseExpressionOrDeclStatement();

    // ========================================================================
    // Expression parsing (precedence climbing)
    // ========================================================================

    std::unique_ptr<ExprNode> parseExpression();
    std::unique_ptr<ExprNode> parseAssignmentExpression();
    std::unique_ptr<ExprNode> parseTernaryExpression();
    std::unique_ptr<ExprNode> parseLogicalOrExpression();
    std::unique_ptr<ExprNode> parseLogicalAndExpression();
    std::unique_ptr<ExprNode> parseBitwiseOrExpression();
    std::unique_ptr<ExprNode> parseBitwiseXorExpression();
    std::unique_ptr<ExprNode> parseBitwiseAndExpression();
    std::unique_ptr<ExprNode> parseEqualityExpression();
    std::unique_ptr<ExprNode> parseRelationalExpression();
    std::unique_ptr<ExprNode> parseShiftExpression();
    std::unique_ptr<ExprNode> parseAdditiveExpression();
    std::unique_ptr<ExprNode> parseMultiplicativeExpression();
    std::unique_ptr<ExprNode> parseUnaryExpression();
    std::unique_ptr<ExprNode> parsePostfixExpression();
    std::unique_ptr<ExprNode> parsePrimaryExpression();

    // Expression helpers
    std::unique_ptr<ExprNode> parseCallExpression(SourceLocation loc, const std::string& name);
    std::unique_ptr<ExprNode> parseConstructorExpression(std::shared_ptr<TypeNode> type);
    std::vector<std::unique_ptr<ExprNode>> parseArgumentList();

    // ========================================================================
    // Literal parsing
    // ========================================================================

    std::unique_ptr<LiteralExpr> parseNumberLiteral();
    std::unique_ptr<LiteralExpr> parseBoolLiteral(bool value);

    // ========================================================================
    // Initialization
    // ========================================================================

    void initBuiltinTypes();
};

// Convenience function to parse source code directly
std::unique_ptr<TranslationUnit> parseShaderSource(
    const std::string& source,
    const std::string& filename = "<input>",
    std::vector<ParseError>* outErrors = nullptr);
