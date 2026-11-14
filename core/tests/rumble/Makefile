# Host tests for the game-rumble read-back parser. They compile the production
# decode (core/src/bt/vpad_rumble_rs.h) with no SDK or console, under
# ASan/UBSan. Run: make -C core/tests/rumble
CC ?= cc
SRC := ../../src
OUT ?= build
CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Wshadow -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS := -I$(SRC)/bt

TESTS := $(OUT)/test_rumble_parse

.PHONY: test clean
test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done

$(OUT)/test_rumble_parse: test_rumble_parse.c $(SRC)/bt/vpad_rumble_rs.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_rumble_parse.c

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)
