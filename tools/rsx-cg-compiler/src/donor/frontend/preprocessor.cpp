#include "preprocessor.h"
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <algorithm>
#include <iostream>

Preprocessor::Preprocessor()
	: currentProcessingFile(""), noLineMarkers(false), keepComments(false)
{
	initBuiltinMacros();
}

void Preprocessor::addIncludePath(const std::string& path)
{
	includePaths.push_back(path);
}

void Preprocessor::defineMacro(const std::string& name, const std::string& value)
{
	MacroDefinition macro;
	macro.name = name;
	macro.isFunctionLike = false;
	macro.isVariadic = false;

	// Tokenize the value
	if(!value.empty())
	{
		Lexer lexer(value);
		macro.replacementList = lexer.tokenize();

		// Remove any trailing EOF token
		if(!macro.replacementList.empty() && macro.replacementList.back().type == TokenType::END_OF_FILE)
		{
			macro.replacementList.pop_back();
		}
	}

	macros[name] = macro;
}

void Preprocessor::defineMacro(const std::string& definition)
{
	// Parse "NAME=VALUE" format
	size_t eqPos = definition.find('=');
	if (eqPos != std::string::npos) 
	{
		std::string name = definition.substr(0, eqPos);
		std::string value = definition.substr(eqPos + 1);
		defineMacro(trim(name), value);
	} 
	else 
	{
		defineMacro(trim(definition), "1");
	}
}

void Preprocessor::setNoLineMarkers(bool value)
{
	noLineMarkers = value;
}

void Preprocessor::setKeepComments(bool value)
{
	keepComments = value;
}

std::string Preprocessor::process(const std::string& source, const std::string& filename)
{
	// Phase 1: Handle backslash-newline continuation (line splicing)
	// This joins physical lines ending with '\' into logical lines
	std::string splicedSource = spliceLines(source);

	// If comments should be removed (-C not set), strip them up-front while preserving newlines
	const std::string& inputSource = keepComments ? splicedSource : stripCommentsPreserveNewlines(splicedSource);

	std::istringstream input(inputSource);
	std::string output;
	std::string line;
	int lineNum = 1;

	// Set current processing file
	currentProcessingFile = filename;

	// Update __FILE__ macro
	macros["__FILE__"].replacementList.clear();
	macros["__FILE__"].replacementList.push_back({ TokenType::STRING, "\"" + filename + "\"", 0, 0, filename });

	// Add to included files for dependency tracking
	includedFiles.insert(filename);

	bool inBlockComment = false; // Only used when keepComments == true

	while (std::getline(input, line))
	{
		// Update __LINE__ macro
		macros["__LINE__"].replacementList.clear();
		macros["__LINE__"].replacementList.push_back({ TokenType::NUMBER, std::to_string(lineNum), lineNum, 0, filename });

		// Check if we're in an active conditional block
		bool active = true;
		if(!conditionalStack.empty())
		{
			active = conditionalStack.top().active;
		}

		std::string trimmedLine = trim(line);
		if(trimmedLine.size() > 0 && trimmedLine[0] == '#')
		{
			processDirective(trimmedLine, output, filename, lineNum);
		}
		else if (active)
		{
			if (keepComments)
			{
				// Expand macros only in code segments; preserve comments verbatim
				std::string expanded = expandWithCommentsAware(line, inBlockComment);
				output += expanded + "\n";
			}
			else
			{
				// Comments were stripped globally; just expand
				std::string expandedLine = expandMacros(line);
				output += expandedLine + "\n";
			}
		}

		lineNum++;
	}

	// Check for unclosed conditionals
	if(!conditionalStack.empty())
	{
		throw std::runtime_error("Unterminated #if/#ifdef block in file " + filename);
	}

	return output;
}

const std::set<std::string> Preprocessor::getIncludedFiles() const
{
	return includedFiles;
}

void Preprocessor::initBuiltinMacros()
{
	// __LINE__ and __FILE__ are handled dynamically during processing
	defineMacro("__LINE__", "");
	defineMacro("__FILE__", "");

	// Define __DATE__ and __TIME__
	defineMacro("__DATE__", "\"" __DATE__ "\"");
	defineMacro("__TIME__", "\"" __TIME__ "\"");
	
	// PSVita specific macros
	defineMacro("__psp2__", "1");
	defineMacro("__SCE__", "1");
	defineMacro("__STDC__", "1");
	defineMacro("__STDC_VERSION__", "199901L"); // C99
}

void Preprocessor::processDirective(const std::string& directive, std::string& output, const std::string& currentFile, int lineNum)
{
	// Parse directive
	std::istringstream iss(directive);
	std::string first, cmd;
	iss >> first;

	if(first.empty() || first[0] != '#')
	{
		throw std::runtime_error("Invalid preprocessor directive: " + directive);
	}

	// Handle forms like "#line", "#include", etc. (no space)
	if(first.size() > 1)
	{
		cmd = first.substr(1); // e.g. "#include" -> "include"
	}
	else
	{
		iss >> cmd;
		if(cmd.empty())
		{
			throw std::runtime_error("Invalid preprocessor directive: " + directive);
		}
	}

	// From here on, 'cmd' is the directive name 

	if (cmd == "include")
	{
		processInclude(directive, output, currentFile, lineNum);
	}
	else if (cmd == "define")
	{
		if(conditionalStack.empty() || conditionalStack.top().active)
			processDefine(directive);
	}
	else if (cmd == "undef")
	{
		if(conditionalStack.empty() || conditionalStack.top().active)
			processUndef(directive);
	}
	else if (cmd == "ifdef")
	{
		processIfdef(directive, false);
	}
	else if (cmd == "ifndef")
	{
		processIfdef(directive, true);
	}
	else if (cmd == "if")
	{
		processIf(directive);
	}
	else if (cmd == "elif")
	{
		processElif(directive);
	}
	else if (cmd == "else")
	{
		processElse();
	}
	else if (cmd == "endif")
	{
		processEndif();
	}
	else if (cmd == "pragma")
	{
		if(conditionalStack.empty() || conditionalStack.top().active)
			processPragma(directive, output);
	}
	else if (cmd == "line")
	{
		if (conditionalStack.empty() || conditionalStack.top().active)
		{
			output += directive + "\n"; // Just pass it through
		}
	}
	else if (cmd == "error")
	{
		if (conditionalStack.empty() || conditionalStack.top().active)
		{
			std::string errorMsg = directive.substr(directive.find("error") + 5);
			throw std::runtime_error("Preprocessor Error Directive at " + currentFile + ":" + std::to_string(lineNum) + ": " + trim(errorMsg));
		}
	}
	else if(cmd == "warning")
	{
		if (conditionalStack.empty() || conditionalStack.top().active)
		{
			std::string warningMsg = directive.substr(directive.find("warning") + 7);
			// Just print a warning to stderr
			std::cerr << "Preprocessor Warning at " << currentFile << ":" << lineNum << ": " << trim(warningMsg) << std::endl;
		}
	}
	else
	{
		throw std::runtime_error("Unknown Preprocessor Directive: " + cmd);
	}
}

void Preprocessor::processInclude(const std::string& directive, std::string& output, const std::string& currentFile, int lineNum)
{
	// early out if not in active conditional
	if (!conditionalStack.empty() && !conditionalStack.top().active)
		return;

	// Extract filename
	std::smatch match;
	std::regex includeRegex(R"(#\s*include\s*([<"])([^>"]+)[>"])");

	if (std::regex_search(directive, match, includeRegex))
	{
		bool isSystem = (match[1] == "<");
		std::string filename = match[2];

		// Locate the file
		std::string filepath = findIncludeFile(filename, isSystem, currentFile);
		if (filepath.empty())
		{
			throw std::runtime_error(currentFile + ":" + std::to_string(lineNum) + ": Cannot find include file: " + filename);
		}

		if(includeGuards.count(filepath) > 0)
		{
			// Already included with #pragma once
			return;
		}

		currentProcessingFile = filepath;

		std::string fileContent = readFile(filepath);

		if (fileContent.empty())
		{
			throw std::runtime_error("Failed to read include file: " + filepath);
		}

		if (!noLineMarkers)
		{
			output += "#line 1 \"" + filepath + "\"\n";
		}

		// Recursively process the included file
		Preprocessor subProcessor = *this; // Copy current state
		std::string processedContent = subProcessor.process(fileContent, filepath);
		output += processedContent;

		if(!noLineMarkers)
		{
			output += "#line " + std::to_string(lineNum + 1) + " \"" + currentFile + "\"\n";
		}

		// Merge included files
		for(const auto& incFile : subProcessor.getIncludedFiles())
		{
			includedFiles.insert(incFile);
		}
	}
	else
	{
		throw std::runtime_error("Malformed #include directive at " + currentFile + ":" + std::to_string(lineNum));
	}
}

void Preprocessor::processDefine(const std::string& directive)
{
	// Extract macro definition
	std::smatch match;
	std::regex defineRegex(R"(#\s*define\s+(\w+)(\s*\(([^)]*)\))?\s*(.*))");

	if (std::regex_search(directive, match, defineRegex))
	{
		MacroDefinition macro;
		macro.name = match[1];

		// Check for function-like macro parameters
		if(match[2].matched)
		{
			macro.isFunctionLike = true;
			// match[3] is the content inside the parentheses (without the parens themselves)
			std::string paramsStr = match[3];

			if (!paramsStr.empty())
			{
				macro.parameters = tokenizeParams(paramsStr);

				// Check for variadic and handle __VA_ARGS__
				if (!macro.parameters.empty() && macro.parameters.back().find("...") != std::string::npos)
				{
					macro.isVariadic = true;
				}
			}
		}
		else
		{
			macro.isFunctionLike = false;
			macro.isVariadic = false;
		}
		
		std::string replacement = match[4];
		if (!replacement.empty())
		{
			Lexer lexer(replacement);
			macro.replacementList = lexer.tokenize();
			// Remove any trailing EOF token
			if(!macro.replacementList.empty() && macro.replacementList.back().type == TokenType::END_OF_FILE)
			{
				macro.replacementList.pop_back();
			}
		}
		macros[macro.name] = macro;
	}
	else
	{
		throw std::runtime_error("Malformed #define directive: " + directive);
	}
}

void Preprocessor::processUndef(const std::string& directive)
{
	// Extract macro name
	std::smatch match;
	std::regex undefRegex(R"(#\s*undef\s+(\w+))");

	if (std::regex_search(directive, match, undefRegex))
	{
		std::string name = match[1];
		macros.erase(name);
	}
	else
	{
		throw std::runtime_error("Malformed #undef directive: " + directive);
	}
}

void Preprocessor::processIfdef(const std::string& directive, bool isIfndef)
{
	// Extract macro name
	std::smatch match;
	std::regex ifdefRegex(R"(#\s*if(?:n)?def\s+(\w+))");

	if (std::regex_search(directive, match, ifdefRegex))
	{
		std::string name = match[1];
		bool defined = (macros.find(name) != macros.end());

		ConditionalState state;
		state.active = isIfndef ? !defined : defined;
		state.hasElse = false;
		state.everActive = state.active;

		// If parent block is inactive, this block is also inactive
		if(!conditionalStack.empty() && !conditionalStack.top().active)
		{
			state.active = false;
		}

		conditionalStack.push(state);
	}
	else
	{
		throw std::runtime_error("Malformed #ifdef/#ifndef directive: " + directive);
	}
}

void Preprocessor::processIf(const std::string& directive)
{
	// Extract expression after #if
	size_t ifPos = directive.find("if");
	if(ifPos == std::string::npos)
	{
		throw std::runtime_error("Malformed #if directive: " + directive);
	}

	std::string expr = directive.substr(ifPos + 2);
	expr = trim(expr);

	ConditionalState state;
	state.active = evaluateExpression(expr);
	state.hasElse = false;
	state.everActive = state.active;

	// If parent block is inactive, this block is also inactive
	if(!conditionalStack.empty() && !conditionalStack.top().active)
	{
		state.active = false;
	}

	conditionalStack.push(state);
}

void Preprocessor::processElif(const std::string& directive)
{
	if(conditionalStack.empty())
	{
		throw std::runtime_error("Unexpected #elif without matching #if");
	}

	ConditionalState& state = conditionalStack.top();
	if(state.hasElse)
	{
		throw std::runtime_error("#elif after #else is not allowed");
	}

	if(!state.everActive) // Only evaluate if no previous branch was active
	{
		// Extract expression after #elif
		size_t elifPos = directive.find("elif");
		if (elifPos == std::string::npos)
		{
			throw std::runtime_error("Malformed #elif directive: " + directive);
		}

		std::string expr = directive.substr(elifPos + 4);
		expr = trim(expr);

		state.active = evaluateExpression(expr);

		// Check parent
		if (conditionalStack.size() > 1)
		{
			std::stack<ConditionalState> temp = conditionalStack;
			temp.pop();
			if (!temp.top().active)
			{
				state.active = false;
			}
		}

		if(state.active)
		{
			state.everActive = true;
		}
	}
	else
	{
		state.active = false; // Previous branch was active, so this one is not
	}
}

void Preprocessor::processElse()
{
	if(conditionalStack.empty())
	{
		throw std::runtime_error("Unexpected #else without matching #if");
	}

	ConditionalState& state = conditionalStack.top();
	if(state.hasElse)
	{
		throw std::runtime_error("Multiple #else directives in the same #if block are not allowed");
	}

	state.hasElse = true;

	if(!state.everActive) // Only active if no previous branch was active
	{
		state.active = true;
		// Check parent
		if (conditionalStack.size() > 1)
		{
			std::stack<ConditionalState> temp = conditionalStack;
			temp.pop();
			if (!temp.top().active)
			{
				state.active = false;
			}
		}
		state.everActive = state.active;
	}
	else
	{
		state.active = false; // Previous branch was active, so this one is not
	}
}

void Preprocessor::processEndif()
{
	if(conditionalStack.empty())
	{
		throw std::runtime_error("Unexpected #endif without matching #if");
	}
	conditionalStack.pop();
}

// TO DO:
void Preprocessor::processPragma(const std::string& directive, std::string& output)
{
	// Extract pragma content
	size_t pragmaPos = directive.find("pragma");
	if(pragmaPos == std::string::npos)
	{
		throw std::runtime_error("Malformed #pragma directive: " + directive);
	}

	std::string content = directive.substr(pragmaPos + 6);
	content = trim(content);

	// Handle specific pragmas
	if(content == "once")
	{
		// Mark current file as included to prevent future inclusions
		if(!currentProcessingFile.empty())
		{
			includeGuards.insert(currentProcessingFile);
		}
	}
	else if (content.rfind("alphakill", 0) == 0)
	{
		// #pragma alphakill <samplerName>
		// RSX Cg extension: tells the runtime to discard fragments
		// where the named sampler returns alpha == 0.  Container-only
		// (ucode unchanged) — recorded here so the .fpo emitter can
		// inject one synthetic $kill_NNNN CgBinaryParameter per sampler
		// in declaration order.
		std::string rest = trim(content.substr(9));
		// Strip a trailing semicolon if the user wrote one.
		if (!rest.empty() && rest.back() == ';') rest.pop_back();
		rest = trim(rest);
		if (!rest.empty() && alphakillSeen_.insert(rest).second)
			alphakillSamplers_.push_back(rest);
	}
	else if (content.find("warning") == 0)
	{
		// TO DO: Pass through for now, but could track warning state
		output += directive + "\n"; // Pass through
	}
	else if(content.find("pack_matrix") == 0)
	{
		// #pragma pack_matrix (row_major/column_major)
		// Pass through, affects parsing pahse
		output += directive + "\n"; // Pass through
	}
	else if (content.find("register_buffer") == 0 ||
		content.find("memory_buffer") == 0 ||
		content.find("readwrite_buffer") == 0 ||
		content.find("strip_buffer") == 0 ||
		content.find("enable_buffer_symbols") == 0 ||
		content.find("disable_buffer_symbols") == 0) {
		// PSVita uniform buffer pragmas
		output += directive + "\n"; // Pass through
	}
	else if (content.find("loop") == 0) {
		// #pragma loop (unroll: always/never/default)
		output += directive + "\n"; // Pass through
	}
	else if (content.find("branch") == 0) {
		// #pragma branch (flatten: always/never/default)
		output += directive + "\n"; // Pass through
	}
	else if (content.find("argument") == 0) {
		// #pragma argument (O0-O4, fastmath, etc.)
		output += directive + "\n"; // Pass through
	}
	else if (content.find("position_invariant") == 0) {
		// #pragma position_invariant <func>
		output += directive + "\n"; // Pass through
	}
	else if (content.find("disable_bank_clash_adjustment") == 0) {
		// PSVita specific optimization pragma
		output += directive + "\n"; // Pass through
	}
	else
	{
		// Unknown pragmas are ignored or can be passed through
		output += directive + "\n"; // Pass through unknown pragmas
	}
}

// Handle backslash-newline continuation (line splicing)
// This is Phase 1 of preprocessing per the C standard
// Lines ending with '\' followed by newline are joined together
std::string Preprocessor::spliceLines(const std::string& src)
{
	std::string result;
	result.reserve(src.size());

	size_t i = 0;
	while (i < src.size())
	{
		char c = src[i];

		// Check for backslash followed by newline (line continuation)
		if (c == '\\')
		{
			// Look ahead for newline (handle both \n and \r\n)
			size_t next = i + 1;

			// Skip any trailing whitespace between \ and newline (non-standard but common)
			while (next < src.size() && (src[next] == ' ' || src[next] == '\t'))
			{
				next++;
			}

			if (next < src.size())
			{
				if (src[next] == '\n')
				{
					// Backslash-newline: skip both, continue on next line
					i = next + 1;
					continue;
				}
				else if (src[next] == '\r' && next + 1 < src.size() && src[next + 1] == '\n')
				{
					// Backslash-CRLF: skip all three characters
					i = next + 2;
					continue;
				}
			}

			// Not a line continuation, just a regular backslash
			result.push_back(c);
			i++;
		}
		else
		{
			result.push_back(c);
			i++;
		}
	}

	return result;
}

// Remove // and /* */ comments while preserving newlines (for line numbers)
std::string Preprocessor::stripCommentsPreserveNewlines(const std::string& src)
{
	std::string out;
	out.reserve(src.size());

	bool inString = false;
	char stringQuote = '\0';
	bool escaped = false;
	size_t i = 0;
	while (i < src.size())
	{
		char c = src[i];
		char n = (i + 1 < src.size()) ? src[i + 1] : '\0';

		if (inString)
		{
			out.push_back(c);
			if (escaped) { escaped = false; }
			else if (c == '\\') { escaped = true; }
			else if (c == stringQuote) { inString = false; stringQuote = '\0'; }
			i++;
			continue;
		}

		// Enter string
		if (c == '"' || c == '\'')
		{
			inString = true; stringQuote = c; escaped = false;
			out.push_back(c);
			i++;
			continue;
		}

		// Line comment
		if (c == '/' && n == '/')
		{
			// Consume until newline; keep newline(s)
			i += 2;
			while (i < src.size() && src[i] != '\n') i++;
			// Newline (if any) will be processed normally below
			continue;
		}

		// Block comment
		if (c == '/' && n == '*')
		{
			i += 2;
			while (i < src.size())
			{
				if (src[i] == '\n') { out.push_back('\n'); i++; continue; }
				if (src[i] == '*' && (i + 1 < src.size()) && src[i + 1] == '/')
				{
					i += 2;
					break;
				}
				i++;
			}
			continue;
		}

		// Normal char
		out.push_back(c);
		i++;
	}
	return out;
}

// Expand macros in code regions, while preserving comments verbatim
std::string Preprocessor::expandWithCommentsAware(const std::string& line, bool& inBlockComment)
{
	// Fast path: if no comment markers and not inside block comment
	if (!inBlockComment && line.find('/') == std::string::npos)
	{
		return expandMacros(line);
	}

	std::string result;
	result.reserve(line.size());

	bool inString = false;
	char stringQuote = '\0';
	bool escaped = false;

	size_t i = 0;
	size_t segStart = 0; // start of code segment to expand

	auto flushCodeSegment = [&](size_t endExclusive) {
		if (endExclusive > segStart)
		{
			std::string code = line.substr(segStart, endExclusive - segStart);
			result += expandMacros(code);
		}
		segStart = endExclusive;
	};

	while (i < line.size())
	{
		char c = line[i];
		char n = (i + 1 < line.size()) ? line[i + 1] : '\0';

		if (inBlockComment)
		{
			// Emit comment chars as-is
			result.push_back(c);
			if (c == '*' && n == '/')
			{
				result.push_back(n);
				i += 2;
				inBlockComment = false;
				segStart = i;
				continue;
			}
			i++;
			continue;
		}

		if (inString)
		{
			if (escaped) { escaped = false; }
			else if (c == '\\') { escaped = true; }
			else if (c == stringQuote) { inString = false; stringQuote = '\0'; }
			i++;
			continue;
		}

		// Enter string literal
		if (c == '"' || c == '\'')
		{
			inString = true; stringQuote = c; escaped = false;
			i++;
			continue;
		}

		// Line comment start
		if (c == '/' && n == '/')
		{
			// Flush code before the comment, then append comment verbatim and finish line
			flushCodeSegment(i);
			result.append(line.begin() + i, line.end());
			return result;
		}

		// Block comment start
		if (c == '/' && n == '*')
		{
			// Flush code before the block comment, append '/*', then emit rest as comment until closing
			flushCodeSegment(i);
			result.push_back('/'); result.push_back('*');
			i += 2;
			inBlockComment = true;
			continue;
		}

		i++;
	}

	// Flush trailing code (if any)
	flushCodeSegment(i);
	return result;
}

// Helper to determine if a space is needed before a token
static bool needsSpaceBefore(TokenType prevType, TokenType currType)
{
	// Don't add space at the very beginning
	if (prevType == TokenType::END_OF_FILE) return false;

	// Punctuation that doesn't want space before
	switch (currType)
	{
	case TokenType::COMMA:
	case TokenType::SEMICOLON:
	case TokenType::RPAREN:
	case TokenType::RBRACKET:
	case TokenType::RBRACE:
	case TokenType::DOT:
		return false;
	default:
		break;
	}

	// Punctuation that doesn't want space after
	switch (prevType)
	{
	case TokenType::LPAREN:
	case TokenType::LBRACKET:
	case TokenType::LBRACE:
	case TokenType::DOT:
		return false;
	default:
		break;
	}

	// NUMBER followed by IDENTIFIER or vice versa should not have space
	// This handles cases like "2D" in sampler2D or "float4"
	if ((prevType == TokenType::NUMBER && currType == TokenType::IDENTIFIER) ||
		(prevType == TokenType::IDENTIFIER && currType == TokenType::NUMBER))
	{
		return false;
	}

	// Most other tokens need space between them
	return true;
}

// Expand macros; only rewrite the line when we actually substitute something
std::string Preprocessor::expandMacros(const std::string& text)
{
	std::string result = text;

	bool expandedAny = false;
	bool progress;
	do
	{
		progress = false;

		// Tokenize the current result
		Lexer lexer(result);
		std::vector<Token> tokens = lexer.tokenize();

		std::string newResult;
		size_t i = 0;
		bool touchedThisPass = false;
		TokenType prevTokenType = TokenType::END_OF_FILE;

		// Helper lambda to append with proper spacing
		auto appendToken = [&](const std::string& lexeme, TokenType type) {
			if (needsSpaceBefore(prevTokenType, type) && !newResult.empty())
			{
				newResult += ' ';
			}
			newResult += lexeme;
			prevTokenType = type;
		};

		while (i < tokens.size())
		{
			const Token& token = tokens[i];

			// Skip EOF tokens if present
			if (token.type == TokenType::END_OF_FILE) { i++; continue; }

			if (token.type == TokenType::IDENTIFIER && macros.find(token.lexeme) != macros.end())
			{
				const MacroDefinition& macro = macros[token.lexeme];
				if (macro.isFunctionLike)
				{
					// Function-like macro, expect '('
					if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::LPAREN)
					{
						i += 2; // Skip macro name and '('
						std::vector<std::string> args;
						std::string currentArg;
						TokenType prevArgTokenType = TokenType::END_OF_FILE;
						int parenDepth = 1;

						// Helper to append to currentArg with proper spacing
						auto appendToArg = [&](const std::string& lexeme, TokenType type) {
							if (needsSpaceBefore(prevArgTokenType, type) && !currentArg.empty())
							{
								currentArg += ' ';
							}
							currentArg += lexeme;
							prevArgTokenType = type;
						};

						while (i < tokens.size() && parenDepth > 0)
						{
							const Token& argToken = tokens[i];
							if (argToken.type == TokenType::LPAREN)
							{
								parenDepth++;
								appendToArg(argToken.lexeme, argToken.type);
							}
							else if (argToken.type == TokenType::RPAREN)
							{
								parenDepth--;
								if (parenDepth > 0)
								{
									appendToArg(argToken.lexeme, argToken.type);
								}
							}
							else if (argToken.type == TokenType::COMMA && parenDepth == 1)
							{
								args.push_back(currentArg);
								currentArg.clear();
								prevArgTokenType = TokenType::END_OF_FILE;
							}
							else
							{
								appendToArg(argToken.lexeme, argToken.type);
							}
							i++;
						}
						if (!currentArg.empty())
						{
							args.push_back(currentArg);
						}
						// Arity checks
						if ((!macro.isVariadic && args.size() != macro.parameters.size()) ||
							(macro.isVariadic && args.size() < (macro.parameters.empty() ? 0 : macro.parameters.size() - 1)))
						{
							throw std::runtime_error("Macro " + macro.name + " called with incorrect number of arguments");
						}
						// Map parameters to arguments
						std::unordered_map<std::string, std::string> paramMap;
						for (size_t p = 0; p < macro.parameters.size(); ++p)
						{
							if (p < args.size())
							{
								paramMap[macro.parameters[p]] = args[p];
							}
							else if (macro.isVariadic && p == macro.parameters.size() - 1)
							{
								std::string variadicArgs;
								for (size_t v = p; v < args.size(); ++v)
								{
									if (v > p) variadicArgs += ",";
									variadicArgs += args[v];
								}
								paramMap[macro.parameters[p]] = variadicArgs;
							}
						}
						// Emit replacement with ## token pasting support
						// First pass: substitute parameters and build intermediate list
						// Also combine adjacent NUMBER+IDENTIFIER tokens around ## operators
						std::vector<std::string> replacementParts;
						for (size_t ri = 0; ri < macro.replacementList.size(); ++ri)
						{
							const Token& repToken = macro.replacementList[ri];

							if (repToken.type == TokenType::OP_HASH_HASH)
							{
								// Token pasting operator - mark it for processing
								replacementParts.push_back("\x01##\x01"); // Special marker

								// Look ahead: combine NUMBER+IDENTIFIER sequence (e.g., "2D" -> "2D")
								// This handles cases like "SMPTY ## 2D" where 2D is tokenized separately
								if (ri + 1 < macro.replacementList.size())
								{
									std::string combined;
									size_t lookahead = ri + 1;

									// Collect adjacent NUMBER/IDENTIFIER tokens
									while (lookahead < macro.replacementList.size())
									{
										const Token& nextTok = macro.replacementList[lookahead];
										if (nextTok.type == TokenType::NUMBER ||
										    nextTok.type == TokenType::IDENTIFIER)
										{
											// Check if it's a parameter
											if (nextTok.type == TokenType::IDENTIFIER &&
											    paramMap.find(nextTok.lexeme) != paramMap.end())
											{
												combined += paramMap[nextTok.lexeme];
											}
											else
											{
												combined += nextTok.lexeme;
											}

											// Check if next token continues the sequence
											if (lookahead + 1 < macro.replacementList.size())
											{
												const Token& afterNext = macro.replacementList[lookahead + 1];
												if ((afterNext.type == TokenType::NUMBER ||
												     afterNext.type == TokenType::IDENTIFIER) &&
												    afterNext.type != macro.replacementList[lookahead].type)
												{
													// Different type, continue combining (NUMBER->ID or ID->NUMBER)
													lookahead++;
													continue;
												}
											}
											lookahead++;
											break;
										}
										else
										{
											break;
										}
									}

									if (!combined.empty())
									{
										replacementParts.push_back(combined);
										ri = lookahead - 1; // -1 because loop will increment
									}
								}
							}
							else if (repToken.type == TokenType::IDENTIFIER &&
							         paramMap.find(repToken.lexeme) != paramMap.end())
							{
								// Parameter substitution
								replacementParts.push_back(paramMap[repToken.lexeme]);
							}
							else
							{
								replacementParts.push_back(repToken.lexeme);
							}
						}

						// Second pass: process ## operators
						std::vector<std::string> pastedParts;
						for (size_t pi = 0; pi < replacementParts.size(); ++pi)
						{
							if (replacementParts[pi] == "\x01##\x01")
							{
								// Token pasting: concatenate previous and next
								if (!pastedParts.empty() && pi + 1 < replacementParts.size())
								{
									// Get previous part (remove trailing whitespace)
									std::string prev = pastedParts.back();
									while (!prev.empty() && (prev.back() == ' ' || prev.back() == '\t'))
										prev.pop_back();
									pastedParts.pop_back();

									// Get next part (skip leading whitespace)
									std::string next = replacementParts[pi + 1];
									size_t start = 0;
									while (start < next.size() && (next[start] == ' ' || next[start] == '\t'))
										start++;
									next = next.substr(start);

									// Concatenate
									pastedParts.push_back(prev + next);
									pi++; // Skip the next part since we consumed it
								}
							}
							else
							{
								pastedParts.push_back(replacementParts[pi]);
							}
						}

						// Third pass: tokenize and emit with proper spacing
						for (const auto& part : pastedParts)
						{
							Lexer partLexer(part);
							auto partTokens = partLexer.tokenize();
							for (const auto& partTok : partTokens)
							{
								if (partTok.type != TokenType::END_OF_FILE)
								{
									appendToken(partTok.lexeme, partTok.type);
								}
							}
						}

						// We performed a substitution in this pass
						touchedThisPass = true;
						progress = true;
						expandedAny = true;
						continue;
					}
					else
					{
						// Not a macro call, just append
						appendToken(token.lexeme, token.type);
						i++;
						continue;
					}
				}
				else
				{
					// Object-like macro
					for (const Token& repToken : macro.replacementList)
					{
						appendToken(repToken.lexeme, repToken.type);
					}
					touchedThisPass = true;
					progress = true;
					expandedAny = true;
					i++;
					continue;
				}
			}

			// Not a macro, just append
			appendToken(token.lexeme, token.type);
			i++;
		}

		if (!touchedThisPass)
		{
			// No substitutions this pass; keep original spacing/text
			break;
		}

		// Apply this pass' result and try again
		result = newResult;

	} while (progress);

	return result;
}

// Recursive descent parser for C preprocessor #if expressions.
// Supports: integer literals (decimal, hex 0x, octal 0), unary !/-/~/+,
// arithmetic */% +/-, shifts <</>>, comparisons </<=/>/>=, equality ==/!=,
// bitwise &/^/|, logical &&/||, ternary ?:, and parentheses.
namespace {

struct ExprParser
{
	const std::string& src;
	size_t pos;

	ExprParser(const std::string& s) : src(s), pos(0) {}

	void skipWhitespace()
	{
		while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t'))
			++pos;
	}

	char peek()
	{
		skipWhitespace();
		return pos < src.size() ? src[pos] : '\0';
	}

	char advance()
	{
		skipWhitespace();
		return pos < src.size() ? src[pos++] : '\0';
	}

	bool match(char c)
	{
		if (peek() == c) { advance(); return true; }
		return false;
	}

	bool matchStr(const char* s)
	{
		skipWhitespace();
		size_t len = strlen(s);
		if (pos + len <= src.size() && src.compare(pos, len, s) == 0)
		{
			// For multi-char operators, make sure we don't match a prefix of a longer token
			pos += len;
			return true;
		}
		return false;
	}

	// Primary: integer literal or parenthesized expression
	long long parsePrimary()
	{
		skipWhitespace();
		if (pos >= src.size())
			return 0;

		// Parenthesized expression
		if (src[pos] == '(')
		{
			++pos;
			long long val = parseTernary();
			match(')');
			return val;
		}

		// Integer literal (decimal, hex, octal)
		if (isdigit(src[pos]))
		{
			long long val = 0;
			if (src[pos] == '0' && pos + 1 < src.size() && (src[pos + 1] == 'x' || src[pos + 1] == 'X'))
			{
				pos += 2; // skip 0x
				while (pos < src.size() && isxdigit(src[pos]))
				{
					char c = src[pos++];
					val = val * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
				}
			}
			else if (src[pos] == '0')
			{
				while (pos < src.size() && src[pos] >= '0' && src[pos] <= '7')
					val = val * 8 + (src[pos++] - '0');
			}
			else
			{
				while (pos < src.size() && isdigit(src[pos]))
					val = val * 10 + (src[pos++] - '0');
			}
			// Skip optional integer suffixes (U, L, UL, LL, ULL)
			while (pos < src.size() && (src[pos] == 'u' || src[pos] == 'U' || src[pos] == 'l' || src[pos] == 'L'))
				++pos;
			return val;
		}

		// Character literal 'x'
		if (src[pos] == '\'')
		{
			++pos;
			long long val = 0;
			if (pos < src.size() && src[pos] == '\\')
			{
				++pos;
				if (pos < src.size())
				{
					switch (src[pos]) {
					case 'n': val = '\n'; break;
					case 't': val = '\t'; break;
					case '\\': val = '\\'; break;
					case '\'': val = '\''; break;
					case '0': val = '\0'; break;
					default: val = src[pos]; break;
					}
					++pos;
				}
			}
			else if (pos < src.size())
			{
				val = src[pos++];
			}
			if (pos < src.size() && src[pos] == '\'') ++pos;
			return val;
		}

		// Any remaining identifier that wasn't expanded by macros evaluates to 0
		// (C preprocessor rule: unknown identifiers in #if are 0)
		if (isalpha(src[pos]) || src[pos] == '_')
		{
			while (pos < src.size() && (isalnum(src[pos]) || src[pos] == '_'))
				++pos;
			return 0;
		}

		return 0;
	}

	// Unary: !, -, ~, +
	long long parseUnary()
	{
		skipWhitespace();
		if (pos < src.size())
		{
			if (src[pos] == '!' && (pos + 1 >= src.size() || src[pos + 1] != '='))
			{
				++pos;
				return parseUnary() ? 0 : 1;
			}
			if (src[pos] == '~')
			{
				++pos;
				return ~parseUnary();
			}
			if (src[pos] == '-' && (pos + 1 >= src.size() || !isdigit(src[pos + 1]) || true))
			{
				// Check it's not just a negative literal (handled by parsePrimary for leading digits)
				// Actually, unary minus should always work here
				++pos;
				return -parseUnary();
			}
			if (src[pos] == '+')
			{
				++pos;
				return parseUnary();
			}
		}
		return parsePrimary();
	}

	// Multiplicative: *, /, %
	long long parseMul()
	{
		long long left = parseUnary();
		for (;;)
		{
			skipWhitespace();
			if (pos < src.size() && src[pos] == '*') { ++pos; long long r = parseUnary(); left = left * r; }
			else if (pos < src.size() && src[pos] == '/' ) { ++pos; long long r = parseUnary(); left = r ? left / r : 0; }
			else if (pos < src.size() && src[pos] == '%') { ++pos; long long r = parseUnary(); left = r ? left % r : 0; }
			else break;
		}
		return left;
	}

	// Additive: +, -
	long long parseAdd()
	{
		long long left = parseMul();
		for (;;)
		{
			skipWhitespace();
			if (pos < src.size() && src[pos] == '+') { ++pos; left += parseMul(); }
			else if (pos < src.size() && src[pos] == '-') { ++pos; left -= parseMul(); }
			else break;
		}
		return left;
	}

	// Shift: <<, >>
	long long parseShift()
	{
		long long left = parseAdd();
		for (;;)
		{
			skipWhitespace();
			if (pos + 1 < src.size() && src[pos] == '<' && src[pos + 1] == '<') { pos += 2; left <<= parseAdd(); }
			else if (pos + 1 < src.size() && src[pos] == '>' && src[pos + 1] == '>') { pos += 2; left >>= parseAdd(); }
			else break;
		}
		return left;
	}

	// Relational: <, <=, >, >=
	long long parseRelational()
	{
		long long left = parseShift();
		for (;;)
		{
			skipWhitespace();
			if (pos + 1 < src.size() && src[pos] == '<' && src[pos + 1] == '=') { pos += 2; left = left <= parseShift() ? 1 : 0; }
			else if (pos + 1 < src.size() && src[pos] == '>' && src[pos + 1] == '=') { pos += 2; left = left >= parseShift() ? 1 : 0; }
			else if (pos < src.size() && src[pos] == '<' && (pos + 1 >= src.size() || src[pos + 1] != '<')) { ++pos; left = left < parseShift() ? 1 : 0; }
			else if (pos < src.size() && src[pos] == '>' && (pos + 1 >= src.size() || src[pos + 1] != '>')) { ++pos; left = left > parseShift() ? 1 : 0; }
			else break;
		}
		return left;
	}

	// Equality: ==, !=
	long long parseEquality()
	{
		long long left = parseRelational();
		for (;;)
		{
			skipWhitespace();
			if (pos + 1 < src.size() && src[pos] == '=' && src[pos + 1] == '=') { pos += 2; left = left == parseRelational() ? 1 : 0; }
			else if (pos + 1 < src.size() && src[pos] == '!' && src[pos + 1] == '=') { pos += 2; left = left != parseRelational() ? 1 : 0; }
			else break;
		}
		return left;
	}

	// Bitwise AND: &
	long long parseBitwiseAnd()
	{
		long long left = parseEquality();
		for (;;)
		{
			skipWhitespace();
			if (pos < src.size() && src[pos] == '&' && (pos + 1 >= src.size() || src[pos + 1] != '&'))
			{ ++pos; left &= parseEquality(); }
			else break;
		}
		return left;
	}

	// Bitwise XOR: ^
	long long parseBitwiseXor()
	{
		long long left = parseBitwiseAnd();
		for (;;)
		{
			skipWhitespace();
			if (pos < src.size() && src[pos] == '^') { ++pos; left ^= parseBitwiseAnd(); }
			else break;
		}
		return left;
	}

	// Bitwise OR: |
	long long parseBitwiseOr()
	{
		long long left = parseBitwiseXor();
		for (;;)
		{
			skipWhitespace();
			if (pos < src.size() && src[pos] == '|' && (pos + 1 >= src.size() || src[pos + 1] != '|'))
			{ ++pos; left |= parseBitwiseXor(); }
			else break;
		}
		return left;
	}

	// Logical AND: &&
	long long parseLogicalAnd()
	{
		long long left = parseBitwiseOr();
		for (;;)
		{
			skipWhitespace();
			if (pos + 1 < src.size() && src[pos] == '&' && src[pos + 1] == '&')
			{ pos += 2; long long right = parseBitwiseOr(); left = (left && right) ? 1 : 0; }
			else break;
		}
		return left;
	}

	// Logical OR: ||
	long long parseLogicalOr()
	{
		long long left = parseLogicalAnd();
		for (;;)
		{
			skipWhitespace();
			if (pos + 1 < src.size() && src[pos] == '|' && src[pos + 1] == '|')
			{ pos += 2; long long right = parseLogicalAnd(); left = (left || right) ? 1 : 0; }
			else break;
		}
		return left;
	}

	// Ternary: ? :
	long long parseTernary()
	{
		long long cond = parseLogicalOr();
		skipWhitespace();
		if (pos < src.size() && src[pos] == '?')
		{
			++pos;
			long long trueVal = parseTernary();
			match(':');
			long long falseVal = parseTernary();
			return cond ? trueVal : falseVal;
		}
		return cond;
	}

	long long evaluate() { return parseTernary(); }
};

} // anonymous namespace

bool Preprocessor::evaluateExpression(const std::string& expr)
{
	std::string expanded = expandMacros(expr);
	expanded = trim(expanded);

	// Handle defined() operator before expression parsing
	std::regex definedRegex(R"(defined\s*\(\s*(\w+)\s*\))");
	std::smatch match;
	std::string processedExpr = expanded;

	while (std::regex_search(processedExpr, match, definedRegex))
	{
		std::string macroName = match[1];
		bool isDefined = (macros.find(macroName) != macros.end());
		processedExpr.replace(match.position(0), match.length(0), isDefined ? "1" : "0");
	}

	// Handle defined without parentheses
	std::regex definedNoParenRegex(R"(defined\s+(\w+))");
	while (std::regex_search(processedExpr, match, definedNoParenRegex))
	{
		std::string macroName = match[1];
		bool isDefined = (macros.find(macroName) != macros.end());
		processedExpr.replace(match.position(0), match.length(0), isDefined ? "1" : "0");
	}

	try
	{
		ExprParser parser(processedExpr);
		return parser.evaluate() != 0;
	}
	catch (...)
	{
		throw std::runtime_error("Failed to evaluate preprocessor expression: " + expr);
	}
}

std::string Preprocessor::findIncludeFile(const std::string& filename, bool isSystem, const std::string& currentFile)
{
	std::vector<std::string> searchPaths;

	if (!isSystem)
	{
		// First search relative to the current file
		std::filesystem::path currentPath(currentFile);
		searchPaths.push_back(currentPath.parent_path().string());
	}

	// Add include paths
	for (const auto& path : includePaths)
	{
		searchPaths.push_back(path);
	}

	// Search for the file
	for (const auto& path : searchPaths)
	{
		std::filesystem::path fullPath = std::filesystem::path(path) / filename;
		if (std::filesystem::exists(fullPath))
		{
			return fullPath.string();
		}
	}

	return "";
}

std::string Preprocessor::readFile(const std::string& filepath)
{
	std::ifstream file(filepath, std::ios::binary);
	if (!file.is_open())
	{
		throw std::runtime_error("Failed to open file: " + filepath);
	}

	std::stringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

std::string Preprocessor::trim(const std::string& str)
{
	size_t first = str.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return "";

	size_t last = str.find_last_not_of(" \t\r\n");
	return str.substr(first, (last - first + 1));
}

// Parse macro definition parameters (comma-separated identifiers)
// e.g., "TYPE, VALUE" -> ["TYPE", "VALUE"]
std::vector<std::string> Preprocessor::tokenizeParams(const std::string& params)
{
	std::vector<std::string> result;
	std::string current;

	for (char c : params)
	{
		if (c == ',')
		{
			std::string trimmed = trim(current);
			if (!trimmed.empty())
			{
				result.push_back(trimmed);
			}
			current.clear();
		}
		else
		{
			current += c;
		}
	}

	// Don't forget the last parameter
	std::string trimmed = trim(current);
	if (!trimmed.empty())
	{
		result.push_back(trimmed);
	}

	return result;
}

// Parse macro invocation arguments (can have nested parens and complex expressions)
std::vector<std::string> Preprocessor::tokenizeArgs(const std::string& args)
{
	std::vector<std::string> result;
	std::string currentArg;
	int parenDepth = 0;

	for (char c : args)
	{
		if (c == ',' && parenDepth == 0)
		{
			result.push_back(trim(currentArg));
			currentArg.clear();
		}
		else
		{
			if (c == '(') parenDepth++;
			if (c == ')') parenDepth--;
			currentArg += c;
		}
	}
	if (!currentArg.empty())
	{
		result.push_back(trim(currentArg));
	}

	return result;
}