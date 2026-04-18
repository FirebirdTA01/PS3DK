#include "errorHandler.h"
#include <iostream>

void ErrorHandler::reportError(const std::string& message)
{
	// For simplicity, we just print the error message to standard error.
	// TO DO: Log this to a file or handle it as needed.
	std::cerr << "Error: " << message << std::endl;
}