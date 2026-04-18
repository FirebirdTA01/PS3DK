#pragma once

#pragma once

#include <string>
#include <vector>
#include <unordered_map>

enum class TokenType
{
	// Literals
	IDENTIFIER,
	NUMBER,
	STRING,

	// Keywords

	// Cg Types
	KW_VOID,
	KW_FLOAT, KW_FLOAT2, KW_FLOAT3, KW_FLOAT4,
	KW_FLOAT2X2, KW_FLOAT3X3, KW_FLOAT4X4,
	KW_HALF, KW_HALF2, KW_HALF3, KW_HALF4,
	KW_HALF2X2, KW_HALF3X3, KW_HALF4X4,
	KW_FIXED, KW_FIXED2, KW_FIXED3, KW_FIXED4,
	KW_INT, KW_INT2, KW_INT3, KW_INT4,
	KW_UINT, KW_UINT2, KW_UINT3, KW_UINT4,
	KW_BOOL, KW_BOOL2, KW_BOOL3, KW_BOOL4,
	KW_CHAR, KW_CHAR2, KW_CHAR3, KW_CHAR4,
	KW_UCHAR, KW_UCHAR2, KW_UCHAR3, KW_UCHAR4,
	KW_SHORT, KW_SHORT2, KW_SHORT3, KW_SHORT4,
	KW_USHORT, KW_USHORT2, KW_USHORT3, KW_USHORT4,

	// Sampler types
	KW_SAMPLER1D, KW_SAMPLER2D, KW_SAMPLER3D, KW_SAMPLERCUBE, KW_SAMPLERRECT,
	KW_ISAMPLER1D, KW_ISAMPLER2D, KW_ISAMPLER3D, KW_ISAMPLERCUBE, KW_ISAMPLERRECT,
	KW_USAMPLER1D, KW_USAMPLER2D, KW_USAMPLER3D, KW_USAMPLERCUBE, KW_USAMPLERRECT,

	// Storage qualifiers
	KW_UNIFORM, KW_IN, KW_OUT, KW_INOUT,
	KW_CONST, KW_STATIC, KW_EXTERN,
	KW_PACKED, // Cg packed arrays
	KW_ROW_MAJOR, KW_COLUMN_MAJOR, // Matrix layout

	// Semantics
	KW_POSITION, KW_COLOR, KW_TEXCOORD, KW_NORMAL,

	// PSVita specific extensions
	KW_REGFORMAT,	// __regformat
	KW_NATIVECOLOR, // __nativecolor
	KW_NOSTRIP,		// __nostrip
	KW_MSAA,		// __msaa
	KW_MSAA2X,		// __msaa2
	KW_MSAA4X,		// __msaa4

	// GCC-style attributes
	KW_ATTRIBUTE,	// __attribute__

	// Control flow
	KW_IF, KW_ELSE, KW_FOR, KW_WHILE, KW_DO,
	KW_BREAK, KW_CONTINUE, KW_RETURN,
	KW_SWITCH, KW_CASE, KW_DEFAULT,
	KW_DISCARD,

	// Other keywords
	KW_STRUCT,
	KW_TYPEDEF,
	KW_SIZEOF,

	// Operators
	OP_PLUS, OP_MINUS, OP_MULTIPLY, OP_DIVIDE, OP_MODULO,
	OP_ASSIGN, OP_PLUS_ASSIGN, OP_MINUS_ASSIGN,
	OP_MULTIPLY_ASSIGN, OP_DIVIDE_ASSIGN, OP_MODULO_ASSIGN,
	OP_EQUAL, OP_NOT_EQUAL,
	OP_LESS, OP_LESS_EQUAL, OP_GREATER, OP_GREATER_EQUAL,
	OP_LOGICAL_AND, OP_LOGICAL_OR, OP_LOGICAL_NOT,
	OP_BITWISE_AND, OP_BITWISE_OR, OP_BITWISE_XOR, OP_BITWISE_NOT,
	OP_SHIFT_LEFT, OP_SHIFT_RIGHT,
	OP_INCREMENT, OP_DECREMENT,
	OP_TERNARY,

	// Delimiters
	SEMICOLON, COMMA, DOT,
	LPAREN, RPAREN,
	LBRACE, RBRACE,
	LBRACKET, RBRACKET,
	COLON, QUESTION,

	// Preprocessor
	PP_DIRECTIVE, // #include, #define, etc. (any of them)
	OP_HASH,      // # (stringize operator)
	OP_HASH_HASH, // ## (token pasting operator)

	// Special
	NEWLINE,
	END_OF_FILE,
	UNKNOWN
};

struct Token
{
	TokenType type;
	std::string lexeme;
	int line;
	int column;
	std::string filename; // For tracking includes
};


class Lexer
{
public:
	explicit Lexer(const std::string& source, const std::string& filename = "<input>");

	std::vector<Token> tokenize();
	std::vector<Token> tokenizeWithPreprocessor(); // Keeps preprocessor directives

private:
	std::string source;
	std::string filename;
	size_t currentPos;
	int line;
	int column;
	std::unordered_map<std::string, TokenType> keywords;

	void initKeywords();
	bool isAtEnd() const;
	char peek(size_t offset = 0) const;
	char advance();
	void skipWhitespace();
	void skipWhiteSpaceAndComments();
	Token nextToken(bool keepPreprocessor = false);
	Token scanIdentifierOrKeyword(int startLine, int startColumn);
	Token scanNumber(int startLine, int startColumn);
	Token scanString(int startLine, int startColumn);
	Token scanPreprocessorDirective();
};