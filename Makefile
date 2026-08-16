# gthread - build rules.
#
# The library is built at -Og with full warnings on: this is a project that is
# meant to be read and stepped through in a debugger, and -Og keeps the
# generated code close enough to the source for that to work. Benchmarks are
# the exception and are built at -O2, see the bench target.

CC       ?= gcc
CFLAGS   := -std=c11 -D_GNU_SOURCE -Og -g3 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  :=
LDLIBS   :=

BUILD    := build
LIB      := $(BUILD)/libgthread.a
SRCS     := $(wildcard src/*.c)
OBJS     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

TESTS    := $(patsubst test/%.c,$(BUILD)/%,$(wildcard test/*.c))
DEMOS    := $(patsubst demo/%.c,$(BUILD)/%,$(wildcard demo/*.c))

.PHONY: all clean test demos deadlock

all: $(LIB) $(TESTS) $(DEMOS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(BUILD)/%: test/%.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/%: demo/%.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

# Acceptance tests. Each one exits non-zero if the property it checks fails.
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

demos: $(DEMOS)

# The deliberate deadlock, built with the locks taken in opposite orders.
# It is supposed to hang: that is the demonstration. Ctrl-C to get out.
$(BUILD)/deadlock_demo: demo/deadlock.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -DDEADLOCK_DEMO $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

deadlock: $(BUILD)/deadlock_demo
	$(BUILD)/deadlock_demo

clean:
	rm -rf $(BUILD)

-include $(DEPS)
