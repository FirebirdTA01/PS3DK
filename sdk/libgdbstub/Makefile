# Host-only RED replay. Override CORE later; the runner/oracles stay unchanged.
CC ?= cc
BUILD ?= build
CORE ?= gdbstub_stub.c
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Werror -pedantic
CPPFLAGS ?=
LDFLAGS ?=
FIXTURES ?= fixtures.json
LAYOUT ?= register-layout.json
NODE ?= node

.PHONY: all test regenerate
all: $(BUILD)/red_test
$(BUILD)/red_test: red_test.c $(CORE) gdbstub_layout.c gdbstub.h fnv1a.h red_data.h
	mkdir -p "$(BUILD)"
	$(CC) $(CPPFLAGS) $(CFLAGS) red_test.c $(CORE) gdbstub_layout.c $(LDFLAGS) -o "$@"
test: all
	"$(BUILD)/red_test" "$(FIXTURES)" "$(LAYOUT)"
regenerate:
	$(NODE) generate_red_data.js "$(FIXTURES)" "$(LAYOUT)" red_data.h
