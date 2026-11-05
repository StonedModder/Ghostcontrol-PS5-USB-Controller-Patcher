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

.PHONY: all clean deploy release print-target app-service print-app-service
.PHONY: console-test-audio-capture

# The default builds only the reviewed normal-API companion. The legacy full
# service remains archived in this tree and is not an app-native candidate.
APP_SERVICE_BUILD_ROOT ?= $(abspath ../.build/openprospero/service)
GC_APP_SERVICE_PORT ?= 39073
app-service:
	$(PYTHON) $(TOOLS)/build-openprospero.py service --openprospero "$(OPENPROSPERO_ROOT)" \
		--output "$(abspath $(APP_SERVICE_BUILD_ROOT)/..)" --service-port "$(GC_APP_SERVICE_PORT)"

print-app-service:
	@printf '%s\n' '$(APP_SERVICE_BUILD_ROOT)/ghostcontrol-app-service.elf'


all: app-service

.PHONY: legacy
legacy: $(TARGET)

print-target:
	@printf '%s\n' '$(TARGET)'

$(STUB_OBJ): %.o: %.c $(wildcard $(S)/*.h)
	$(CC) $(CFLAGS) -fno-vectorize -fno-slp-vectorize -fno-builtin -c $< -o $@

$(TARGET): $(SRC) $(wildcard $(S)/*.h) $(STUB_OBJ)
	@mkdir -p $(dir $(TARGET))
	$(CC) $(CFLAGS) -o $@ $(filter-out $(STUB_SRC),$(SRC)) $(STUB_OBJ) \
		$(LDFLAGS) $(LDLIBS)
	python3 $(TOOLS)/check_ds4_stubs.py $@
	@sh $(TOOLS)/check_imports.sh $@; rc=$$?; \
	if [ $$rc -eq 1 ]; then echo "check_imports: refusing $@" >&2; rm -f $@; exit 1; fi; \
	if [ $$rc -eq 2 ]; then echo "check_imports: skipped (no readelf on this host)" >&2; fi

release: $(TARGET)
	@mkdir -p $(BUILD_DIR)
	@set -e; r="$(BUILD_DIR)/core.elf"; s="$(BUILD_DIR)/core-stripped.elf"; \
	trap 'rm -f "$$r" "$$s"' EXIT; \
	cp $(TARGET) "$$r"; \
	$(GC_OBJCOPY) --strip-debug "$$r" "$$s"; \
	python3 $(TOOLS)/check_ds4_stubs.py "$$s"; \
	cp "$$s" $(TARGET)

deploy: $(TARGET)
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)

# Host test suite. Ported from the 1.x payload Makefile with every path
# adapted to this tree (sources under src/, Bluetooth host under src/bt/,
# tests under tests/). Each binary is built and run immediately so a failure
# stops the suite. tests/bt/ holds the separate Ghosttooth runtime sim
# harnesses; those were not wired by the reference target (their includes are
# rooted at the old runtime layout) and are intentionally left out here.
# Run this focused producer regression before older broad steps, so a known
# unrelated failure cannot hide the new acceptance check.
SETUP_PREVIEW_TEST_ROOT ?= $(abspath $(TEST_DIR)/setup-preview-$(shell date +%Y%m%d-%H%M%S-%N))
# Many fixtures mkdtemp a private store under TMPDIR and refuse to run without
# one. Honour a caller-provided TMPDIR, and otherwise keep the temp root inside
# TEST_DIR so the suite does not depend on the host exporting one.
TMPDIR ?= $(TEST_DIR)/tmp
export TMPDIR
.PHONY: 
clean:
	rm -f $(STUB_OBJ)
	rm -rf $(BUILD_DIR) out

# AUDIO-07 console gate: the same gate source as the host test, compiled for
# the console with the payload SDK and run there (the devloop's file channel
# pulls /data/gc-audio-capture-gate.log back). Not part of the host `test`
# target, and the gate itself opens no radio, capture service or console API:
# its service is the fake backend. Load it with tools/ps5loop.py load, which
# sends any payload ELF to the console's loader on port 9021.
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
CONSOLE_GATE := $(BUILD_DIR)/gc-audio-capture-gate.elf
CONSOLE_GATE_SHA := $(shell sha256sum $(TS)/test_gc_audio_capture.c | cut -d' ' -f1)
CONSOLE_GATE_DEF := -DGC_CAPTURE_GATE_TESTSHA="\"$(CONSOLE_GATE_SHA)\""
console-test-bt-rx:
	@test -x "$(PS5_PAYLOAD_SDK)/bin/prospero-clang" || { echo "console-test-bt-rx: no payload SDK at PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK)" >&2; exit 1; }
	@mkdir -p $(BUILD_DIR)
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang -I$(S) -I$(BT) -I$(PS5_PAYLOAD_SDK)/target/include \
		-D__PROSPERO__ -std=gnu11 -Wall -Wextra -Wshadow -Werror -g -O2 \
		-Dmain=gc_bt_rx_gate_main -DGC_BT_RX_GATE_TESTSHA="\"$$(sha256sum $(TS)/test_bt_rx.c | cut -d' ' -f1)\"" \
		-c $(TS)/test_bt_rx.c -o $(TEST_DIR)/rx-gate-test.o
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang -I$(S) -I$(BT) -I$(PS5_PAYLOAD_SDK)/target/include \
		-D__PROSPERO__ -std=gnu11 -Wall -Wextra -Wshadow -Werror -g -O2 \
		-DGC_BT_RX_GATE_TESTSHA="\"$$(sha256sum $(TS)/test_bt_rx.c | cut -d' ' -f1)\"" \
		-o $(BUILD_DIR)/gc-bt-rx-gate.elf ../tools/bt-rx-gate.c $(TEST_DIR)/rx-gate-test.o \
		$(BT)/hci_rx.c $(BT)/evstream.c
	@printf '%s\n' '$(abspath $(BUILD_DIR)/gc-bt-rx-gate.elf)'

console-test-audio-capture:
	@test -x "$(PS5_PAYLOAD_SDK)/bin/prospero-clang" || { echo "console-test-audio-capture: no payload SDK at PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK)" >&2; exit 1; }
	@mkdir -p $(BUILD_DIR)
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang -I$(S) -I$(S)/audio -I$(PS5_PAYLOAD_SDK)/target/include \
		-D__PROSPERO__ -std=gnu11 -Wall -Wextra -Wshadow -Werror -g -O2 \
		-Dmain=gc_capture_gate_main $(CONSOLE_GATE_DEF) \
		-c $(TS)/test_gc_audio_capture.c -o $(TEST_DIR)/gate-test.o
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang -I$(S) -I$(S)/audio -I$(PS5_PAYLOAD_SDK)/target/include \
		-D__PROSPERO__ -std=gnu11 -Wall -Wextra -Wshadow -Werror -g -O2 \
		$(CONSOLE_GATE_DEF) \
		-o $(CONSOLE_GATE) ../tools/audio-capture-gate.c $(TEST_DIR)/gate-test.o \
		$(S)/audio/gc_audio_capture.c $(S)/audio/gc_audio.c $(S)/audio/gc_audio_settings.c \
		$(S)/audio/gc_audio_dsp.c $(S)/gc_file_safety.c
	@printf '%s\n' '$(abspath $(CONSOLE_GATE))'

# AUDIO-02/03/04 rework gate: the same three test translation units, compiled
# for the console with their entry points renamed, and one runner that calls
# them in order. The gates' dependencies are their own fakes (owner reports,
# scripted PDUs, byte buffers), so this payload opens no radio, no capture
# service, no pad and no title; it writes /data/gc-audio-rework-gate.log and
# nothing else. Load it with tools/ps5loop.py load and pull the log over FTP.
AUDIO_REWORK_GATE := $(BUILD_DIR)/gc-audio-rework-gate.elf
AUDIO_RATE_SHA := $(shell sha256sum $(TS)/test_gc_audio_rate.c | cut -d' ' -f1)
AUDIO_RETRY_SHA := $(shell sha256sum $(TS)/test_gc_audio_avdtp_retry.c | cut -d' ' -f1)
AUDIO_AVCTP_SHA := $(shell sha256sum $(TS)/test_gc_audio_avctp.c | cut -d' ' -f1)
AUDIO_REWORK_DEF := -DGC_AUDIO_RATE_GATE_TESTSHA="\"$(AUDIO_RATE_SHA)\"" \
	-DGC_AUDIO_RETRY_GATE_TESTSHA="\"$(AUDIO_RETRY_SHA)\"" \
	-DGC_AUDIO_AVCTP_GATE_TESTSHA="\"$(AUDIO_AVCTP_SHA)\""
AUDIO_GATE_CFLAGS = -I$(S) -I$(S)/audio -I$(PS5_PAYLOAD_SDK)/target/include \
	-D__PROSPERO__ -std=gnu11 -Wall -Wextra -Wshadow -Werror -g -O2 $(AUDIO_REWORK_DEF)
console-test-audio-rework:
	@test -x "$(PS5_PAYLOAD_SDK)/bin/prospero-clang" || { echo "console-test-audio-rework: no payload SDK at PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK)" >&2; exit 1; }
	@mkdir -p $(BUILD_DIR)
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(AUDIO_GATE_CFLAGS) -Dmain=gc_audio_rate_gate_main \
		-c $(TS)/test_gc_audio_rate.c -o $(TEST_DIR)/rate-gate-test.o
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(AUDIO_GATE_CFLAGS) -Dmain=gc_audio_retry_gate_main \
		-c $(TS)/test_gc_audio_avdtp_retry.c -o $(TEST_DIR)/retry-gate-test.o
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(AUDIO_GATE_CFLAGS) -Dmain=gc_audio_avctp_gate_main \
		-c $(TS)/test_gc_audio_avctp.c -o $(TEST_DIR)/avctp-gate-test.o
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(AUDIO_GATE_CFLAGS) \
		-o $(AUDIO_REWORK_GATE) ../tools/audio-rework-gate.c \
		$(TEST_DIR)/rate-gate-test.o $(TEST_DIR)/retry-gate-test.o $(TEST_DIR)/avctp-gate-test.o \
		$(S)/audio/gc_audio_codec.c $(S)/audio/gc_audio_avdtp.c $(S)/audio/gc_audio_controls.c \
		$(S)/audio/gc_audio.c $(S)/audio/gc_audio_settings.c $(S)/audio/gc_audio_dsp.c \
		$(S)/gc_file_safety.c
	@printf '%s\n' '$(abspath $(AUDIO_REWORK_GATE))'
