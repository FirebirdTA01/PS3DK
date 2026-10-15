// cxx-iostream-startup: libc locks taken before newlib's own start-up constructor.
//
// newlib creates its lock-creation lock in a priority-105 constructor.  Anything that takes a libc lock earlier
// (malloc, stdio) used to lock a lock that did not exist yet and abort() before stdio was up, a silent hang before
// main.  GCC 16's libstdc++ initialises the standard streams at constructor priority 90, so every iostream program
// hit it.  Two witnesses: the streams themselves, and malloc from a user constructor at priority 101 (the first
// priority user code may use, which also runs before 105, on any compiler).
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int early_malloc_ok = -1;

__attribute__((constructor(101))) static void early_user_constructor()
{
	char *p = static_cast<char *>(malloc(64));
	early_malloc_ok = p != nullptr;
	if (p) {
		strcpy(p, "early");
		free(p);
	}
}

int main()
{
	int fails = 0;
	printf("cxx-iostream-startup: main reached\n");
	if (early_malloc_ok != 1) {
		printf("cxx-iostream-startup: malloc in a priority-101 constructor FAIL\n");
		fails++;
	}
	std::string text = "cout " + std::to_string(42);
	std::cout << "cxx-iostream-startup: " << text << std::endl;
	std::cerr << "cxx-iostream-startup: cerr" << std::endl;
	if (!std::cout.good() || !std::cerr.good()) {
		printf("cxx-iostream-startup: stream state FAIL\n");
		fails++;
	}
	if (fails)
		printf("CXX_IOSTREAM_STARTUP_FAIL %d\n", fails);
	else
		printf("CXX_IOSTREAM_STARTUP_OK\n");
	return fails;
}
