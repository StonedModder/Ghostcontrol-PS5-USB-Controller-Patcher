PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
CFLAGS  += -D__PROSPERO__ -std=gnu11 -Wall -Wextra -Wshadow -Werror -O2 -fPIC -fno-stack-protector
LDFLAGS += -Wl,--gc-sections,--build-id=sha1

TARGET := gc-app-save.elf

.PHONY: all clean
all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
