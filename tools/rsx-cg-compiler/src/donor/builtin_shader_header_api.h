#pragma once
#include <string>
#include <string_view>

// Accessors for the embedded (auto-generated) builtin shader header.
const char* GetBuiltinShaderHeaderName();
std::string GetBuiltinShaderHeaderSource();
std::string_view GetBuiltinShaderHeaderView();