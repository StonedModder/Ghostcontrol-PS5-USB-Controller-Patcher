# gc-app-stop - stop the app-native GhosttoothAPI companion from the console.
#
# Sent to the same ELF loader (9021) as the service. Sends its own loopback STOP,
# then reaps the pid named in runtime-instance.json, logging to stop-app.txt.
#
#   make            build gc-app-stop.elf
#   make deploy     send it to the console's ELF loader (deploy-core.py)

PS5_HOST ?= 192.168.69.38
PORT     ?= 9021
TARGET   := gc-app-stop.elf
TOOLS    := ../../

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS  += -D__PROSPERO__ -std=gnu11 -Wall -Wextra -Wshadow -Werror \
	-O2 -fPIC -fno-stack-protector -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections,--build-id=sha1

.PHONY: all deploy clean
all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

deploy: $(TARGET)
	python3 $(TOOLS)tools/deploy-core.py --host $(PS5_HOST) --file $(TARGET)

clean:
	rm -f $(TARGET)
