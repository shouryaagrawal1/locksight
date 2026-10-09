# LockSight Makefile
#
#   make          build the library and all test programs
#   make lib      build only src/liblocksight.so
#   make tests    build only the test programs
#   make demo     run one test program under LockSight
#   make install  install the 'locksight' command (PREFIX=~/.local by default)
#   make uninstall remove it again
#   make check    run every test program under LockSight and verify the results
#   make clean    remove everything that was built
#
# Note: normally Makefile command lines must start with a TAB character.
# The next line makes them start with ">" instead, which is easier to copy
# and paste without breaking.
.RECIPEPREFIX = >

CC      = gcc
PREFIX ?= $(HOME)/.local
CFLAGS  = -g -O0 -Wall -Wextra -pthread

LIB        = src/liblocksight.so
LIB_CFLAGS = -shared -fPIC -O2 -g -Wall -Wextra -pthread

TEST_NAMES = classic_deadlock three_lock_cycle safe_ordered bank_transfer dining_philosophers \
             single_thread_both_orders gate_lock partial_gate self_deadlock recursive_ok \
             errorcheck_ok trylock_case mutex_reuse many_locks big_ring worker_pool_outlier \
             random_orders
TEST_BINS  = $(addprefix tests/, $(TEST_NAMES))

.PHONY: all lib launcher tests demo check install uninstall clean

all: lib launcher tests

# The launcher script must be executable.
launcher:
> chmod +x locksight

lib: $(LIB)

# The LockSight library: src/liblocksight.c -> src/liblocksight.so
$(LIB): src/liblocksight.c
> $(CC) $(LIB_CFLAGS) -o $@ $< -ldl

tests: $(TEST_BINS)

# Build rule: tests/xyz.c (+ tests/common.h) -> tests/xyz
tests/%: tests/%.c tests/common.h
> $(CC) $(CFLAGS) -o $@ $<

# Quick demo: the "lucky" run never freezes, yet LockSight still warns.
demo: all
> LD_PRELOAD=./$(LIB) ./tests/classic_deadlock --lucky

check: all
> bash tests/check.sh

install: lib launcher
> mkdir -p $(PREFIX)/bin $(PREFIX)/lib/locksight
> install -m 755 locksight $(PREFIX)/bin/locksight
> install -m 755 $(LIB) $(PREFIX)/lib/locksight/liblocksight.so
> @echo "Installed: $(PREFIX)/bin/locksight"
> @echo "If 'locksight' is not found, add $(PREFIX)/bin to your PATH."

uninstall:
> rm -f $(PREFIX)/bin/locksight
> rm -rf $(PREFIX)/lib/locksight

clean:
> rm -f $(TEST_BINS) $(LIB)
