# Ghostcontrol Toolbox 2.0 — controller service (standalone ELF)
#
# One artifact, no ShellUI injection and no HomeUI/Settings bundle patching.
# The service owns the controllers and exposes everything over its IPC
# socket; the UI is a separate native homebrew app that connects to it.
# Nothing here depends on a firmware offset table or bundle fingerprint, so
# the build is firmware-agnostic by construction.

PS5_HOST ?= 192.168.69.8
PORT     ?= 9021
VERSION  := 2.0.0-dev.1
TARGET   := out/$(VERSION)/ghostcontrol-core-$(VERSION).elf
BUILD_DIR := .build
TEST_DIR  := $(BUILD_DIR)/tests
TOOLS    := ../tools
.DEFAULT_GOAL := app-service

include ../tools/openprospero.mk

GC_OBJCOPY    ?= llvm-objcopy
GC_PORTS_ROOT ?= $(abspath ../.build/openprospero/deps/pacbrew-v0.40.2)
GC_CA_BUNDLE  ?= $(GC_PORTS_ROOT)/etc/ca-bundle.crt

S  := src
BT := $(S)/bt
TS := tests

CFLAGS  += -D__PROSPERO__ -std=gnu11 -Wall -Wextra -Wshadow -Wunreachable-code \
	-Werror -g -O2 -fPIC -fno-stack-protector \
	-ffunction-sections -fdata-sections -fdebug-compilation-dir=. \
	-I$(S) -I$(BT) -I$(GC_PORTS_ROOT)/include \
	-DGC_CA_BUNDLE_PATH=\"$(GC_CA_BUNDLE)\"
LDFLAGS += -Wl,--gc-sections,--build-id=sha1 -L$(GC_PORTS_ROOT)/lib
LDLIBS  := -lScePad -lSceUserService -lSceNotification -lkernel_sys -ljson-c \
	-lcurl -lpsl -lssl -lcrypto -lzstd -lz -lpthread -lm

# Userland Bluetooth host (USB-HCI), compiled straight into the service.
BT_SRC := $(BT)/host.c $(BT)/le.c $(BT)/generic.c $(BT)/profiles.c \
	$(BT)/hci_usb.c $(BT)/usb_desc.c $(BT)/hci_rx.c $(BT)/evstream.c $(BT)/smp_crypto.c \
	$(BT)/crc32.c $(BT)/util.c $(BT)/log.c

SRC := $(S)/gc_main.c $(S)/gc_ini.c $(S)/gc_profile.c $(S)/gc_config.c \
	$(S)/gc_state.c $(S)/gc_ipc.c $(S)/gc_notify.c $(S)/gc_update_policy.c \
	$(S)/gc_update.c $(S)/gc_console_id.c $(S)/gc_vda_klog.c \
	$(S)/gc_ca_bundle.S $(S)/gc_icon.S $(S)/gc_ds4_policy.c \
	$(S)/gc_ds4_bridge.c $(S)/gc_ds4_source.c $(S)/gc_ds4_game.c \
	$(S)/gc_ds4_game_stub.c $(S)/gc_remote.c $(S)/ghostPad/gc_ghostpad.c \
	$(S)/usb_helpers.c $(S)/controller_nintendo.c $(S)/controller_xbox.c \
	$(S)/controller_ds4.c $(S)/controller_ds3.c $(S)/controller_logitech.c \
	$(S)/controller_mamba.c
SRC += $(S)/gc_hid.c $(S)/gc_usb.c $(S)/gc_setup.c $(S)/gc_file_safety.c \
	$(S)/gc_singleton.c $(S)/gc_bluetooth.c $(S)/gc_dualsense_fx.c \
	$(S)/gc_dualsense_fx_scan.c $(S)/gc_rawmap.c $(S)/gc_log_tail.c \
	probes/gc_fx_probe.c
SRC += $(S)/gc_bt.c $(S)/gc_bt_hci.c $(BT_SRC)
# AUDIO-07: the capture adapter that fills the GcAudioProvider seam over the
# console's capture service. gc_audio_capture.c is backend-agnostic and host
# testable; gc_audio_capture_sce.c is the console backend and compiles to
# nothing without __PROSPERO__ (the target build defines it).
SRC += $(S)/audio/gc_audio_capture.c $(S)/audio/gc_audio_capture_sce.c
# AUDIO-07 wiring: the service's audio owner gets its PCM provider from the
# capture adapter instead of running with no source. gc_service_audio.c and the
# audio module are part of the app-service build (tools/build-app-service.sh);
# this line keeps the legacy target's list complete too.
SRC += $(S)/gc_service_capture.c
# The runtime command table (rumble / fx status / get / set) lives here and
# gc_main.c calls it, but the legacy list never named it: the target linked with
# gc_runtime_command_get/set/rumble/fx_status undefined. The app-service build
# already carries it.
SRC += $(S)/gc_runtime_commands.c

# These three are read by an injected reader and must stay position
# independent with no external data references or direct calls.
STUB_SRC := $(S)/gc_ds4_source.c $(S)/gc_ds4_game_stub.c \
	$(S)/gc_dualsense_fx_remote.c
STUB_OBJ := $(STUB_SRC:.c=.o)

.PHONY: all clean deploy release test print-target app-service test-app-service print-app-service
.PHONY: console-test-audio-capture

# The default builds only the reviewed normal-API companion. The legacy full
# service remains archived in this tree and is not an app-native candidate.
APP_SERVICE_BUILD_ROOT ?= $(abspath ../.build/openprospero/service)
GC_APP_SERVICE_PORT ?= 39073
app-service:
	$(PYTHON) $(TOOLS)/build-openprospero.py service --openprospero "$(OPENPROSPERO_ROOT)" \
		--output "$(abspath $(APP_SERVICE_BUILD_ROOT)/..)" --service-port "$(GC_APP_SERVICE_PORT)"

test-app-service:
	GC_APP_SERVICE_BUILD_ROOT="$(APP_SERVICE_BUILD_ROOT)" GC_APP_SERVICE_PORT="$(GC_APP_SERVICE_PORT)" \
		bash $(TOOLS)/build-app-service.sh host

print-app-service:
	@printf '%s\n' '$(APP_SERVICE_BUILD_ROOT)/ghostcontrol-app-service.elf'


all: app-service

.PHONY: legacy
legacy: $(TARGET)

print-target:
	@printf '%s\n' '$(TARGET)'

