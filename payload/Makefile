PS5_HOST ?=
PORT     ?= 9021
TARGET   := ghost-control-manba-v2-nbjr.elf

WIRELESS_BRIDGE_TARGET      := ghost-control-wireless-ds4.elf
WIRELESS_BRIDGE_AUTO_TARGET := ghost-control-wireless-ds4-auto.elf
WIRELESS_STATUS_TARGET      := ghost-control-wireless-ds4-status.elf
WIRELESS_STOP_TARGET        := stop-ghost-control-wireless-ds4.elf

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is not set)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS  += -D__PROSPERO__ -Wall -Wextra -g -O2 -fPIC -fno-stack-protector
LDFLAGS += -lScePad -lSceUserService -lpthread -ldl

SRC := gc_main.c shellui_pad.c usb_helpers.c controller_nintendo.c controller_xbox.c controller_ds4.c controller_mamba.c

.PHONY: all clean deploy wireless-ds4 wireless-ds4-auto wireless-ds4-status stop-wireless-ds4

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

wireless-ds4: $(WIRELESS_BRIDGE_TARGET)

$(WIRELESS_BRIDGE_TARGET): game_pad_bridge_main.c wireless_ds4.c wireless_ds4.h gc_types.h
	$(CC) $(CFLAGS) -o $@ game_pad_bridge_main.c wireless_ds4.c $(LDFLAGS)

wireless-ds4-auto: $(WIRELESS_BRIDGE_AUTO_TARGET)

$(WIRELESS_BRIDGE_AUTO_TARGET): game_pad_bridge_main.c wireless_ds4.c wireless_ds4.h gc_types.h
	$(CC) $(CFLAGS) -DGC_WIRELESS_DS4_AUTO_WATCH=1 -o $@ game_pad_bridge_main.c wireless_ds4.c $(LDFLAGS)

wireless-ds4-status: $(WIRELESS_STATUS_TARGET)

$(WIRELESS_STATUS_TARGET): game_pad_bridge_status_main.c wireless_ds4.c wireless_ds4.h gc_types.h
	$(CC) $(CFLAGS) -o $@ game_pad_bridge_status_main.c wireless_ds4.c $(LDFLAGS)

stop-wireless-ds4: $(WIRELESS_STOP_TARGET)

$(WIRELESS_STOP_TARGET): stop_game_pad_bridge_main.c
	$(CC) $(CFLAGS) -o $@ stop_game_pad_bridge_main.c $(LDFLAGS)

clean:
	rm -f $(TARGET) $(WIRELESS_BRIDGE_TARGET) $(WIRELESS_BRIDGE_AUTO_TARGET) \
		$(WIRELESS_STATUS_TARGET) $(WIRELESS_STOP_TARGET)

deploy: $(TARGET)
ifndef PS5_HOST
	$(error PS5_HOST is not set)
endif
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)
