# Host tests for the service's USB start-up paths. They compile the production
# sources against stand-in FreeBSD USB headers (stub/) and intercept the
# device calls, so no console is needed. Run: make -C core/tests/usb
CC ?= cc
SRC := ../../src
OUT ?= build
CFLAGS ?= -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Wshadow -Werror \
	-fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS := -Istub -I$(SRC) -I$(SRC)/bt
LDLIBS := -pthread

TESTS := $(OUT)/test_hid_layout_cache $(OUT)/test_gip_startup

.PHONY: test clean
test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done

$(OUT)/test_hid_layout_cache: test_hid_layout_cache.c $(SRC)/gc_usb.c $(SRC)/gc_hid.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_hid_layout_cache.c $(SRC)/gc_hid.c $(LDLIBS)

$(OUT)/test_gip_startup: test_gip_startup.c $(SRC)/controller_xbox.c $(SRC)/gc_usb_opener.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_gip_startup.c $(SRC)/gc_usb_opener.c $(LDLIBS)

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(OUT)
