PS5_HOST ?= PS5
PORT     ?= 9021
TARGET   := ghost-control-xbox-ps5.elf
PS5_LOC := /data/pldmgr/payloads/local/ghost_control/$(ELF)
PS5_FTPPORT := 2121

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS  += -D__PROSPERO__ -Wall -Wextra -g -O2 -fPIC -fno-stack-protector
LDFLAGS += -lScePad -lSceUserService -lpthread -ldl

SRC := gc_main.c shellui_pad.c usb_helpers.c controller_nintendo.c controller_xbox.c controller_gcpad.c controller_n64.c

.PHONY: all clean deploy

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

deploy: $(TARGET)
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)
	
publish: $(ELF)
	@echo "Sending $(TARGET) to PS5 at $(PS5_IP):$(PS5_PORT)..."
	curl -T $(TARGET) -Q "MKD $(PS5_LOC)" ftp://$(PS5_HOST):$(PS5_FTPPORT)$(PS5_LOC)
	@echo "Payload sent successfully!"
