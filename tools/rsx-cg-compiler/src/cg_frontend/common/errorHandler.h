#pragma once

#include <string>

class ErrorHandler
{
	public:
	ErrorHandler() = default;
	~ErrorHandler() = default;
	void reportError(const std::string& message);
};