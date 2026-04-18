#pragma once

#include "lexer.h"
#include <stack>
#include <set>

struct MacroDefinition
{
	std::string name;
	std::vector<std::string> parameters; // empty if not function-like
	std::vector<Token> replacementList;
	bool isFunctionLike;
	bool isVariadic;
};

struct ConditionalState
{
	bool active; // Current branch is active
	bool hasElse; // Already seen else
	bool everActive; // Any branch has been active
};

class Preprocessor
{
public:
	Preprocessor();

	void addIncludePath(const std::string& path);

	void defineMacro(const std::string& name, const std::string& value);
	void defineMacro(const std::string& definition); // Parse "NAME=VALUE" format

	// Options
	void setNoLineMarkers(bool value);
	void setKeepComments(bool value);

	// Process source
	std::string process(const std::string& source, const std::string& filename);

	// For dependency generation
	const std::set<std::string> getIncludedFiles() const;

private:
	std::vector<std::string> includePaths;
	std::unordered_map<std::string, MacroDefinition> macros;
	std::stack<ConditionalState> conditionalStack;
	std::set<std::string> includedFiles;
	std::set<std::string> includeGuards; // For #pragma once
	std::string currentProcessingFile;
	bool noLineMarkers;
	bool keepComments;

	void initBuiltinMacros();

	// Directive processing
	void processDirective(const std::string& directive, std::string& output, const std::string& currentFile, int lineNum);
	void processInclude(const std::string& directive, std::string& output, const std::string& currentFile, int lineNum);
	void processDefine(const std::string& directive);
	void processUndef(const std::string& directive);
	void processIfdef(const std::string& directive, bool isIfndef);
	void processIf(const std::string& directive);
	void processElif(const std::string& directive);
	void processElse();
	void processEndif();
	void processPragma(const std::string& directive, std::string& output);

	// Macros
	std::string expandMacros(const std::string& text);
	bool evaluateExpression(const std::string& expr);

	// File handling
	std::string findIncludeFile(const std::string& filename, bool isSystem, const std::string& currentFile);
	std::string readFile(const std::string& filepath);

	// Utilities
	std::string trim(const std::string& str);
	std::vector<std::string> tokenizeParams(const std::string& params);  // For macro definition parameters
	std::vector<std::string> tokenizeArgs(const std::string& args);      // For macro invocation arguments

	// Line splicing (backslash-newline continuation)
	std::string spliceLines(const std::string& src);

	// Comment handling
	std::string stripCommentsPreserveNewlines(const std::string& src);
	std::string expandWithCommentsAware(const std::string& line, bool& inBlockComment);
};