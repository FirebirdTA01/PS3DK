#pragma once

#include "lexer.h"
#include <vector>
#include <stack>
#include <set>

struct MacroDefinition
{
	std::string name;
	std::vector<std::string> parameters; // empty if not function-like
	std::vector<Token> replacementList;
	bool isFunctionLike = false;
	bool isVariadic = false; // read by the expander for EVERY function-like
	                         // macro, but processDefine set it only when a
	                         // variadic tail was present - an uninitialized
	                         // read that decided arity from stack garbage and
	                         // gave one build's binary a different answer than
	                         // another's on the same source (t_53363b4b).
};

struct ConditionalState
{
	// In-class defaults on every bool: today each is assigned at both
	// construction sites, but that is a property of the writers, not the
	// type, and a single missing assignment would be another indeterminate
	// read like MacroDefinition::isVariadic was (t_53363b4b).
	bool active = false; // Current branch is active
	bool hasElse = false; // Already seen else
	bool everActive = false; // Any branch has been active
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

	// PSL1GHT / the reference SDK Cg pragma surface — collected during preprocess
	// so the back-end can pick them up (lexer drops #pragma lines, so
	// they don't reach the parser).  Insertion order is preserved; it
	// matters for the $kill_NNNN parameter index in the .fpo container.
	const std::vector<std::string>& alphakillSamplers() const { return alphakillSamplers_; }

private:
	std::vector<std::string> includePaths;
	std::unordered_map<std::string, MacroDefinition> macros;
	std::stack<ConditionalState> conditionalStack;
	int includeDepth = 0; // >0 while processing an #include'd file
	std::set<std::string> includedFiles;
	std::set<std::string> includeGuards; // For #pragma once
	std::string currentProcessingFile;
	bool noLineMarkers;
	bool keepComments;

	// the reference SDK Cg pragma collectors.  Populated by processPragma.
	std::vector<std::string> alphakillSamplers_;
	std::set<std::string>    alphakillSeen_;     // dedup helper

	void initBuiltinMacros();

	// Directive processing
	void processDirective(const std::string& directive, std::string& output, const std::string& currentFile, int lineNum);
	void processInclude(const std::string& directive, std::string& output, const std::string& currentFile, int lineNum);
	void processDefine(const std::string& directive, const std::string& currentFile, int lineNum);
	void processUndef(const std::string& directive);
	void processIfdef(const std::string& directive, bool isIfndef);
	void processIf(const std::string& directive);
	void processElif(const std::string& directive);
	void processElse();
	void processEndif();
	void processPragma(const std::string& directive, std::string& output);

	// Macros
	std::string expandMacros(
		const std::string& text,
		const std::string& currentFile = "<input>",
		int lineNum = 1,
		int startColumn = 1);
	bool evaluateExpression(const std::string& expr);

	// File handling
	std::string findIncludeFile(const std::string& filename, bool isSystem, const std::string& currentFile);
	std::string readFile(const std::string& filepath);

	// Utilities
	std::string trim(const std::string& str);
	std::vector<std::string> tokenizeParams(const std::string& params);  // For macro definition parameters
	std::vector<std::string> tokenizeArgs(const std::string& args);      // For macro invocation arguments

	// Line splicing (backslash-newline continuation)
	// Joins backslash-newline continuations.  `physicalLinesPerSpliced`, when
	// given, receives how many SOURCE lines each spliced line consumed, so a
	// caller can keep counting the lines the author wrote rather than the
	// lines that survived splicing.
	std::string spliceLines(const std::string& src,
	                        std::vector<int>* physicalLinesPerSpliced = nullptr);

	// Comment handling
	std::string stripCommentsPreserveNewlines(const std::string& src);
	std::string expandWithCommentsAware(
		const std::string& line,
		bool& inBlockComment,
		const std::string& currentFile,
		int lineNum);
};
