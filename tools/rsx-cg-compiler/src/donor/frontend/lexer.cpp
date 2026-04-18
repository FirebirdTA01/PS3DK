#include "lexer.h"
#include <cctype>
#include <algorithm>
#include <sstream>

Lexer::Lexer(const std::string& source, const std::string& filename)
	: source(source), filename(filename), currentPos(0), line(1), column(1)
{
	initKeywords();
}

void Lexer::initKeywords() {
    // Cg types
    keywords["void"] = TokenType::KW_VOID;
    keywords["float"] = TokenType::KW_FLOAT;
    keywords["float2"] = TokenType::KW_FLOAT2;
    keywords["float3"] = TokenType::KW_FLOAT3;
    keywords["float4"] = TokenType::KW_FLOAT4;
    keywords["float2x2"] = TokenType::KW_FLOAT2X2;
    keywords["float3x3"] = TokenType::KW_FLOAT3X3;
    keywords["float4x4"] = TokenType::KW_FLOAT4X4;

    keywords["half"] = TokenType::KW_HALF;
    keywords["half2"] = TokenType::KW_HALF2;
    keywords["half3"] = TokenType::KW_HALF3;
    keywords["half4"] = TokenType::KW_HALF4;
    keywords["half2x2"] = TokenType::KW_HALF2X2;
    keywords["half3x3"] = TokenType::KW_HALF3X3;
    keywords["half4x4"] = TokenType::KW_HALF4X4;

    keywords["fixed"] = TokenType::KW_FIXED;
    keywords["fixed2"] = TokenType::KW_FIXED2;
    keywords["fixed3"] = TokenType::KW_FIXED3;
    keywords["fixed4"] = TokenType::KW_FIXED4;

    keywords["int"] = TokenType::KW_INT;
    keywords["int2"] = TokenType::KW_INT2;
    keywords["int3"] = TokenType::KW_INT3;
    keywords["int4"] = TokenType::KW_INT4;

    // Add unsigned types
    keywords["uint"] = TokenType::KW_UINT;
    keywords["unsigned"] = TokenType::KW_UINT;

    // Add signed modifier (signed alone = signed int, which is just int)
    keywords["signed"] = TokenType::KW_INT;

    keywords["bool"] = TokenType::KW_BOOL;
    keywords["bool2"] = TokenType::KW_BOOL2;
    keywords["bool3"] = TokenType::KW_BOOL3;
    keywords["bool4"] = TokenType::KW_BOOL4;
    keywords["char"] = TokenType::KW_CHAR;
    keywords["char2"] = TokenType::KW_CHAR2;
    keywords["char3"] = TokenType::KW_CHAR3;
    keywords["char4"] = TokenType::KW_CHAR4;
    keywords["uchar"] = TokenType::KW_UCHAR;
    keywords["uchar2"] = TokenType::KW_UCHAR2;
    keywords["uchar3"] = TokenType::KW_UCHAR3;
    keywords["uchar4"] = TokenType::KW_UCHAR4;
    keywords["short"] = TokenType::KW_SHORT;
    keywords["short2"] = TokenType::KW_SHORT2;
    keywords["short3"] = TokenType::KW_SHORT3;
    keywords["short4"] = TokenType::KW_SHORT4;
    keywords["ushort"] = TokenType::KW_USHORT;
    keywords["ushort2"] = TokenType::KW_USHORT2;
    keywords["ushort3"] = TokenType::KW_USHORT3;
    keywords["ushort4"] = TokenType::KW_USHORT4;
    keywords["uint2"] = TokenType::KW_UINT2;
    keywords["uint3"] = TokenType::KW_UINT3;
    keywords["uint4"] = TokenType::KW_UINT4;

    // Samplers
    keywords["sampler1D"] = TokenType::KW_SAMPLER1D;
    keywords["sampler2D"] = TokenType::KW_SAMPLER2D;
    keywords["sampler3D"] = TokenType::KW_SAMPLER3D;
    keywords["samplerCUBE"] = TokenType::KW_SAMPLERCUBE;
    keywords["samplerRECT"] = TokenType::KW_SAMPLERRECT;

    keywords["isampler1D"] = TokenType::KW_ISAMPLER1D;
    keywords["isampler2D"] = TokenType::KW_ISAMPLER2D;
    keywords["isampler3D"] = TokenType::KW_ISAMPLER3D;
    keywords["isamplerCUBE"] = TokenType::KW_ISAMPLERCUBE;

    keywords["usampler1D"] = TokenType::KW_USAMPLER1D;
    keywords["usampler2D"] = TokenType::KW_USAMPLER2D;
    keywords["usampler3D"] = TokenType::KW_USAMPLER3D;
    keywords["usamplerCUBE"] = TokenType::KW_USAMPLERCUBE;

    // Storage
    keywords["uniform"] = TokenType::KW_UNIFORM;
    keywords["in"] = TokenType::KW_IN;
    keywords["out"] = TokenType::KW_OUT;
    keywords["inout"] = TokenType::KW_INOUT;
    keywords["const"] = TokenType::KW_CONST;
    keywords["static"] = TokenType::KW_STATIC;
    keywords["extern"] = TokenType::KW_EXTERN;
    keywords["packed"] = TokenType::KW_PACKED;
    keywords["row_major"] = TokenType::KW_ROW_MAJOR;
    keywords["column_major"] = TokenType::KW_COLUMN_MAJOR;

    // PSVita extensions
    keywords["__regformat"] = TokenType::KW_REGFORMAT;
    keywords["__nativecolor"] = TokenType::KW_NATIVECOLOR;
    keywords["__nostrip"] = TokenType::KW_NOSTRIP;
    keywords["__msaa"] = TokenType::KW_MSAA;
    keywords["__msaa2x"] = TokenType::KW_MSAA2X;
    keywords["__msaa4x"] = TokenType::KW_MSAA4X;
    keywords["__attribute__"] = TokenType::KW_ATTRIBUTE;

    // Control flow
    keywords["if"] = TokenType::KW_IF;
    keywords["else"] = TokenType::KW_ELSE;
    keywords["for"] = TokenType::KW_FOR;
    keywords["while"] = TokenType::KW_WHILE;
    keywords["do"] = TokenType::KW_DO;
    keywords["break"] = TokenType::KW_BREAK;
    keywords["continue"] = TokenType::KW_CONTINUE;
    keywords["return"] = TokenType::KW_RETURN;
    keywords["switch"] = TokenType::KW_SWITCH;
    keywords["case"] = TokenType::KW_CASE;
    keywords["default"] = TokenType::KW_DEFAULT;
    keywords["discard"] = TokenType::KW_DISCARD;

    keywords["struct"] = TokenType::KW_STRUCT;
    keywords["typedef"] = TokenType::KW_TYPEDEF;
    keywords["sizeof"] = TokenType::KW_SIZEOF;
}

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;

    while(!isAtEnd())
    {
        skipWhiteSpaceAndComments();
        if (isAtEnd())
            break;

        // Skip preprocessor directives that pass through (leftover from preprocessor output)
        // This includes #line directives and #pragma directives
        // Only skip if # is followed by a directive name (not ## which is token pasting)
        if (peek() == '#' && peek(1) != '#')
        {
            // Check if this looks like a preprocessor directive:
            // - 'l' for "#line"
            // - 'p' for "#pragma"
            // - digit (like "#1 filename")
            // - space/tab followed by line/pragma
            char next = peek(1);
            if (next == 'l' || next == 'p' || std::isdigit(next) || next == ' ' || next == '\t')
            {
                // Skip the entire line (it's a preprocessor directive)
                while (!isAtEnd() && peek() != '\n')
                {
                    advance();
                }
                if (!isAtEnd() && peek() == '\n')
                {
                    advance(); // consume the newline
                }
                continue;
            }
        }

        Token token = nextToken();
        if (token.type != TokenType::UNKNOWN)
        {
            tokens.push_back(token);
        }
	}

    tokens.push_back({TokenType::END_OF_FILE, "", line, column, filename});
	return tokens;
}

std::vector<Token> Lexer::tokenizeWithPreprocessor()
{
    std::vector<Token> tokens;

    while(!isAtEnd()) 
    {
        skipWhitespace();
        if (isAtEnd())
            break;

		// Check for preprocessor directive at start of line
        if (peek() == '#' && column == 1)
        {
            advance(); // Consume '#'

			tokens.push_back(scanPreprocessorDirective());
            if(peek() == '\n') // Handle newline after directive
            {
                advance();
				tokens.push_back({ TokenType::NEWLINE, "\n", line - 1, column, filename });
			}
        }
        else
        {
            Token token = nextToken(false);
            if (token.type != TokenType::UNKNOWN || !token.lexeme.empty())
            {
                tokens.push_back(token);
            }
		}
    }
    tokens.push_back({TokenType::END_OF_FILE, "", line, column, filename});
    return tokens;
}

bool Lexer::isAtEnd() const 
{
    return currentPos >= source.length();
}

char Lexer::peek(size_t offset) const 
{
    if (currentPos + offset >= source.length())
        return '\0';

    return source[currentPos + offset];
}

char Lexer::advance() 
{
    if (isAtEnd())
        return '\0';

    char c = source[currentPos++];
    if (c == '\n') 
    {
        line++;
        column = 1;
    } 
    else 
    {
        column++;
    }
    return c;
}

// Command line parameters have an option to preserve comments when preprocessing so we want seperate functions to skip whitespace and comments

void Lexer::skipWhitespace() 
{
    while (!isAtEnd()) 
    {
        if (std::isspace(peek()))
        {
            advance();
        }
        else
            break;
    }
}

void Lexer::skipWhiteSpaceAndComments() 
{
    while (!isAtEnd()) 
    {
        if (std::isspace(peek())) 
        {
            advance();
        } 
        else if (peek() == '/' && peek(1) == '/') 
        {
            // Single-line comment
            while (peek() != '\n' && !isAtEnd()) 
                advance();
        } 
        else if (peek() == '/' && peek(1) == '*') 
        {
            // Multi-line comment
            advance(); // Consume '/'
            advance(); // Consume '*'
            while (!isAtEnd()) 
            {
                if ((peek() == '*' && peek(1) == '/'))
                {
					advance(); // Consume '*'
                    advance(); // Consume '/'
					break;
                }
				advance();
            }
        } 
        else 
        {
            break;
        }
    }
}

Token Lexer::nextToken(bool keepPreprocessor)
{
	int startLine = line;
	int startColumn = column;
	char c = advance();

	// Identifiers and keywords
    if (std::isalpha(c) || c == '_') 
    {
        return scanIdentifierOrKeyword(startLine, startColumn);
	}

	// Numbers
    if (std::isdigit(c))
    {
		return scanNumber(startLine, startColumn);
    }

    // String literals
    if(c == '"')
    {
        return scanString(startLine, startColumn);
	}

    // Operators and punctuation
    switch (c) 
    {
    case '+':
        if (peek() == '+')
        {
            advance();
            return { TokenType::OP_INCREMENT, "++", startLine, startColumn };
        }
        if (peek() == '=')
        {
            advance();
            return { TokenType::OP_PLUS_ASSIGN, "+=", startLine, startColumn };
        }

        return { TokenType::OP_PLUS, "+", startLine, startColumn };

    case '-':
        if (peek() == '-')
        {
            advance();
            return { TokenType::OP_DECREMENT, "--", startLine, startColumn };
        }
        if (peek() == '=')
        {
            advance();
            return { TokenType::OP_MINUS_ASSIGN, "-=", startLine, startColumn };
        }

        return { TokenType::OP_MINUS, "-", startLine, startColumn };

    case '*':
        if (peek() == '=')
        {
            advance();
            return { TokenType::OP_MULTIPLY_ASSIGN, "*=", startLine, startColumn };
        }

        return { TokenType::OP_MULTIPLY, "*", startLine, startColumn };

    case '/':
        if (peek() == '=') 
        {
            advance();
            return { TokenType::OP_DIVIDE_ASSIGN, "/=", startLine, startColumn };
        }

        return { TokenType::OP_DIVIDE, "/", startLine, startColumn };

    case '=':
        if (peek() == '=')
        {
            advance();
            return { TokenType::OP_EQUAL, "==", startLine, startColumn };
        }

        return { TokenType::OP_ASSIGN, "=", startLine, startColumn };

    case '!':
        if (peek() == '=')
        {
            advance();
            return { TokenType::OP_NOT_EQUAL, "!=", startLine, startColumn };
        }

        return { TokenType::OP_LOGICAL_NOT, "!", startLine, startColumn };

    case '<':
        if (peek() == '=')
        {
            advance();
            return { TokenType::OP_LESS_EQUAL, "<=", startLine, startColumn };
        }

        return { TokenType::OP_LESS, "<", startLine, startColumn };

    case '>':
        if (peek() == '=') 
        {
            advance();
            return { TokenType::OP_GREATER_EQUAL, ">=", startLine, startColumn };
        }

        return { TokenType::OP_GREATER, ">", startLine, startColumn };

    case '&':
        if (peek() == '&') 
        {
            advance();
            return { TokenType::OP_LOGICAL_AND, "&&", startLine, startColumn };
        }

        return { TokenType::UNKNOWN, std::string(1, c), startLine, startColumn };

    case '|':
        if (peek() == '|') 
        {
            advance();
            return { TokenType::OP_LOGICAL_OR, "||", startLine, startColumn };
        }

        return { TokenType::UNKNOWN, std::string(1, c), startLine, startColumn };

    case ';': return { TokenType::SEMICOLON, ";", startLine, startColumn };
    case ',': return { TokenType::COMMA, ",", startLine, startColumn };
    case '.': return { TokenType::DOT, ".", startLine, startColumn };
    case '(': return { TokenType::LPAREN, "(", startLine, startColumn };
    case ')': return { TokenType::RPAREN, ")", startLine, startColumn };
    case '{': return { TokenType::LBRACE, "{", startLine, startColumn };
    case '}': return { TokenType::RBRACE, "}", startLine, startColumn };
    case '[': return { TokenType::LBRACKET, "[", startLine, startColumn };
    case ']': return { TokenType::RBRACKET, "]", startLine, startColumn };
    case ':': return { TokenType::COLON, ":", startLine, startColumn };
    case '?': return { TokenType::QUESTION, "?", startLine, startColumn };

    case '#':
        if (peek() == '#')
        {
            advance();
            return { TokenType::OP_HASH_HASH, "##", startLine, startColumn };
        }
        return { TokenType::OP_HASH, "#", startLine, startColumn };

    default:
        return { TokenType::UNKNOWN, std::string(1, c), startLine, startColumn };
    }
}

Token Lexer::scanIdentifierOrKeyword(int startLine, int startColumn) 
{
    size_t start = currentPos - 1; // because we've already consumed the first character

    while (std::isalnum(peek()) || peek() == '_') 
    {
        advance();
    }

    std::string lexeme = source.substr(start, currentPos - start);

	// Check if it's a keyword
    auto it = keywords.find(lexeme);
    if (it != keywords.end()) 
    {
        return { it->second, lexeme, startLine, startColumn, filename };
    }

    return { TokenType::IDENTIFIER, lexeme, startLine, startColumn, filename };
}

Token Lexer::scanNumber(int startLine, int startColumn) 
{
    size_t start = currentPos - 1; // because we've already consumed the first character
    bool hasDecimal = false;
	bool hasExponent = false;
	bool hasFloatSuffix = false;

    // Integer part
    while (std::isdigit(peek()) || peek() == '.') 
    {
        advance();
    }

	// Decimal part
    if(peek() == '.' && std::isdigit(peek(1)))
    {
        hasDecimal = true;
        advance(); // consume '.'
        while (std::isdigit(peek())) 
        {
            advance();
        }
	}

	// Scientific notation
    if (peek() == 'e' || peek() == 'E')
    {
        hasExponent = true;
        advance(); // consume 'e' or 'E'
        if (peek() == '+' || peek() == '-') // optional sign
            advance();
        while (std::isdigit(peek()))
        {
            advance();
        }
    }

	// Float suffix (f, h for half)
    if (peek() == 'f' || peek() == 'h' || peek() == 'F' || peek() == 'H')
    {
        hasFloatSuffix = true;
        advance(); // consume suffix
	}

    std::string lexeme = source.substr(start, currentPos - start);
    return { TokenType::NUMBER, lexeme, startLine, startColumn, filename };
}

Token Lexer::scanString(int startLine, int startColumn) 
{
    size_t start = currentPos; 
    
    // after the opening quote
    while (!isAtEnd() && peek() != '"') 
    {
        if (peek() == '\\' && peek(1) == '"') 
        {
            advance(); // Skip escaped quote
            if(!isAtEnd()) 
				advance(); // Consume the escaped quote
        }
        else
        {
            advance();
        }
    }

    if (isAtEnd())
    {
        // Unterminated string
        return { TokenType::UNKNOWN, "unterminated string", startLine, startColumn, filename};
    }

    advance(); // Consume closing quote

    // Now extract the lexeme including both quotes
    // start-1 is the opening quote, currentPos is just past the closing quote
    std::string lexeme = source.substr(start - 1, currentPos - start + 1);

    return { TokenType::STRING, lexeme, startLine, startColumn, filename };
}

Token Lexer::scanPreprocessorDirective() 
{
    int startLine = line;
    int startColumn = column;
    size_t start = currentPos - 1; // include '#'

    while (!isAtEnd() && peek() != '\n') 
    {
        advance();
    }

    std::string lexeme = source.substr(start, currentPos - start);
    return { TokenType::PP_DIRECTIVE, lexeme, startLine, startColumn, filename };
}