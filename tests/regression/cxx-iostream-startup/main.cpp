// cxx-iostream-startup: the runtime is ready for every constructor above its start-up ladder, and still alive for
// teardown.
//
// The runtime sets itself up in implementation-range constructors: heap 83, syscall table 84, newlib's lock-creation
// lock 85, finalization 86.  GCC 16's libstdc++ initialises the standard streams at priority 90; user code starts at
// 101.  Before the ladder moved there (it was 103-106), a libc lock taken in an earlier constructor hung the program
// before main, and malloc in a priority-101 constructor got no heap.  Witnesses, on both ABIs:
//   - the standard streams work in main;
//   - priority 88 (implementation range, after the ladder, before libstdc++): malloc and a syscall-hook write work;
//   - priority 101 (the first user priority): malloc works;
//   - teardown: an atexit handler registered in a constructor and a C++ static destructor both run, and each can
//     still allocate and write.
// Witness output goes straight to fd 1 with write(): stdout may be fully buffered and flush only at exit.
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>

static int impl_malloc_ok = -1, impl_write_ok = -1;
static int early_malloc_ok = -1;

static void say(const char *text)
{
	(void)!write(1, text, strlen(text));
}

static int allocate_and_touch(void)
{
	char *p = static_cast<char *>(malloc(64));
	if (!p)
		return 0;
	strcpy(p, "live");
	free(p);
	return 1;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
__attribute__((constructor(88))) static void implementation_range_constructor()
{
	impl_malloc_ok = allocate_and_touch();
	static const char text[] = "cxx-iostream-startup: priority-88 write\n";
	impl_write_ok = write(1, text, sizeof text - 1) == (ssize_t)(sizeof text - 1);
}
#pragma GCC diagnostic pop

static void teardown_atexit()
{
	say(allocate_and_touch() ? "STARTUP_TEARDOWN_ATEXIT\n" : "cxx-iostream-startup: atexit handler malloc FAIL\n");
}

__attribute__((constructor(101))) static void early_user_constructor()
{
	early_malloc_ok = allocate_and_touch();
	if (atexit(teardown_atexit) != 0)
		say("cxx-iostream-startup: atexit registration FAIL\n");
}

struct TeardownWitness {
	~TeardownWitness()
	{
		say(allocate_and_touch() ? "STARTUP_TEARDOWN_DTOR\n" : "cxx-iostream-startup: static destructor malloc FAIL\n");
	}
};
static TeardownWitness teardown_witness;

int main()
{
	int fails = 0;
	printf("cxx-iostream-startup: main reached\n");
	if (impl_malloc_ok != 1 || impl_write_ok != 1) {
		printf("cxx-iostream-startup: priority-88 constructor malloc %d write %d FAIL\n", impl_malloc_ok,
		       impl_write_ok);
		fails++;
	}
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
	fflush(stdout);
	return fails;
}
