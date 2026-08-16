# gthread - build rules.
#
# The library is built at -Og with full warnings on: this is a project that is
# meant to be read and stepped through in a debugger, and -Og keeps the
# generated code close enough to the source for that to work. Benchmarks are
# the exception and are built at -O2, see the bench target.

CC       ?= gcc
# -U_FORTIFY_SOURCE because this toolchain enables fortification by default at
# any optimisation level, and fortified longjmp aborts when the target frame is
# not on the current stack. A user level thread switch is nothing but that.
CFLAGS   := -std=c11 -D_GNU_SOURCE -Og -g3 -U_FORTIFY_SOURCE \
            -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  :=
LDLIBS   :=

# Benchmarks are built at -O2, and so is the copy of the library they link
# against. Measuring an -Og library against an -O2 pthread baseline would make
# the comparison differ in two things at once, which is exactly what section
# "benchmark integrity" of the brief forbids.
#
# -U_FORTIFY_SOURCE because this toolchain turns fortification on by default at
# -O2, and fortified longjmp aborts on a jump to a frame that is not on the
# current stack - which is precisely what a user level thread switch is.
BENCH_CFLAGS := -std=c11 -D_GNU_SOURCE -O2 -U_FORTIFY_SOURCE -g -Wall -Wextra \
                -Wpedantic -Iinclude -Ibench

# ASan and ucontext do not get on. The sanitizer tracks one stack per thread
# and a green thread switch moves the stack pointer somewhere it has never
# heard of, so without being told it reports stack-buffer-overflow on
# perfectly good frames and misses real ones. sched.c tells it, through
# __sanitizer_start_switch_fiber/__sanitizer_finish_switch_fiber, which are
# compiled in only when __SANITIZE_ADDRESS__ is defined. If a future ASan
# still produces false positives here, that belongs in the README rather than
# in a quietly deleted target.
ASAN_CFLAGS := -std=c11 -D_GNU_SOURCE -O1 -g3 -U_FORTIFY_SOURCE -fsanitize=address,undefined \
               -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Iinclude

BUILD    := build
O2BUILD  := $(BUILD)/o2
ASANBUILD := $(BUILD)/asan
LIB      := $(BUILD)/libgthread.a
LIBO2    := $(O2BUILD)/libgthread.a
SRCS     := $(wildcard src/*.c)
OBJS     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
O2OBJS   := $(patsubst src/%.c,$(O2BUILD)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d) $(O2OBJS:.o=.d)
BENCHES  := $(patsubst bench/%.c,$(BUILD)/%,$(wildcard bench/*.c))

TESTS    := $(patsubst test/%.c,$(BUILD)/%,$(wildcard test/*.c))
ASANTESTS := $(patsubst test/%.c,$(ASANBUILD)/%,$(wildcard test/*.c))
ASANOBJS := $(patsubst src/%.c,$(ASANBUILD)/%.o,$(SRCS))
DEMOS    := $(patsubst demo/%.c,$(BUILD)/%,$(wildcard demo/*.c))

.PHONY: all clean test demos demo deadlock server bench asan

# Everything buildable, including the benchmarks - `make bench` runs them,
# this only compiles them, so they are there to be run or profiled by hand.
all: $(LIB) $(TESTS) $(DEMOS) $(BENCHES)

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

$(O2BUILD):
	@mkdir -p $(O2BUILD)

$(O2BUILD)/%.o: src/%.c | $(O2BUILD)
	$(CC) $(BENCH_CFLAGS) -MMD -MP -c $< -o $@

$(LIBO2): $(O2OBJS)
	$(AR) rcs $@ $^

# pthreads appear here and nowhere else in the project: in the benchmarks they
# are the thing being compared against.
$(BUILD)/%: bench/%.c $(LIBO2) | $(BUILD)
	$(CC) $(BENCH_CFLAGS) $< $(LIBO2) $(LDFLAGS) -lpthread -o $@

# Acceptance tests. Each one exits non-zero if the property it checks fails.
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

demos: $(DEMOS)

# The CSP pipeline: generator -> filter -> consumer, then a select with a
# timeout.
demo: $(BUILD)/pipeline
	$(BUILD)/pipeline

# The deliberate deadlock, built with the locks taken in opposite orders.
# It is supposed to hang: that is the demonstration. Ctrl-C to get out.
$(BUILD)/deadlock_demo: demo/deadlock.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -DDEADLOCK_DEMO $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

deadlock: $(BUILD)/deadlock_demo
	$(BUILD)/deadlock_demo

# TCP echo server, one green thread per connection. Ctrl-C to stop.
# Override the port and the connection limit: make server ARGS="9002 20000"
ARGS ?= 9001 12000
server: $(BUILD)/echo_server
	$(BUILD)/echo_server $(ARGS)

clean:
	rm -rf $(BUILD)

-include $(DEPS)

$(ASANBUILD):
	@mkdir -p $(ASANBUILD)

$(ASANBUILD)/%.o: src/%.c | $(ASANBUILD)
	$(CC) $(ASAN_CFLAGS) -MMD -MP -c $< -o $@

$(ASANBUILD)/libgthread.a: $(ASANOBJS)
	$(AR) rcs $@ $^

$(ASANBUILD)/%: test/%.c $(ASANBUILD)/libgthread.a | $(ASANBUILD)
	$(CC) $(ASAN_CFLAGS) $< $(ASANBUILD)/libgthread.a -o $@

# The whole test suite under AddressSanitizer and UBSan.
asan: $(ASANTESTS)
	@for t in $(ASANTESTS); do echo "== $$t"; \
		ASAN_OPTIONS=detect_stack_use_after_return=0 $$t || exit 1; done

# Every benchmark, into a fresh bench/results.md. Takes a few minutes: each
# one runs its workload 11 times plus a discarded warmup.
bench: $(BENCHES)
	@rm -f bench/results.md
	@printf '# Benchmark results\n\nProduced by `make bench`. Every number here was measured on the machine described in each section; none are estimates.\n\n' > bench/results.md
	$(BUILD)/ctxswitch
	$(BUILD)/spawn
	$(BUILD)/scale
	$(BUILD)/echo_bench
	@echo
	@echo "results written to bench/results.md"
