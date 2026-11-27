# Host tests for the updater's release policy (version ordering). They compile
# the production gc_update_policy.c on its own; no console or SDK is needed.
# Run: make -C core/tests/update
CC ?= cc
SRC := ../../src
OUT ?= build
CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Wshadow -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS := -I$(SRC)

TESTS := $(OUT)/test_version_compare

.PHONY: test clean
test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done

$(OUT)/test_version_compare: test_version_compare.c $(SRC)/gc_update_policy.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_version_compare.c $(SRC)/gc_update_policy.c

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)
