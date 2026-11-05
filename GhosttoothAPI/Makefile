# GhosttoothAPI — separate companion ELF. Not an OpenProspero module.
# Load this on the console ELF loader (port 9021). The native app talks to it
# on GC_APP_SERVICE_PORT and can also push this file if it is packaged.
PS5_HOST ?= ps5
PS5_PORT ?= 9021
.DEFAULT_GOAL := all

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

VARIANT ?= resident
ifeq ($(filter $(VARIANT),resident diagnostic resident-no-capture),)
    $(error VARIANT must be resident, resident-no-capture, or diagnostic)
endif
ifeq ($(VARIANT),diagnostic)
ELF := GhosttoothAPI-diagnostic.elf
else ifeq ($(VARIANT),resident-no-capture)
ELF := GhosttoothAPI-resident-no-capture.elf
else
ELF := GhosttoothAPI.elf
endif
PORT ?= 39073
ROOT := ..
SRC := $(ROOT)/core/src
PORTS ?= $(PS5_PAYLOAD_SDK)/target/user/homebrew

SOURCES := \
	$(SRC)/gc_app_service.c \
	$(SRC)/gc_runtime_commands.c \
	$(SRC)/gc_app_pad.c \
	$(SRC)/gc_config.c \
	$(SRC)/gc_controller_commands.c \
	$(SRC)/gc_profile.c \
	$(SRC)/gc_ini.c \
	$(SRC)/gc_ipc.c \
	$(SRC)/gc_log_tail.c \
	$(SRC)/gc_service_output.c \
	$(SRC)/gc_service_features.c \
	$(SRC)/gc_console_id.c \
	$(SRC)/gc_setup.c \
	$(SRC)/gc_hid.c \
	$(SRC)/gc_rawmap.c \
	$(SRC)/gc_bluetooth.c \
	$(SRC)/gc_bt.c \
	$(SRC)/gc_bt_hci.c \
	$(SRC)/bt/host.c \
	$(SRC)/bt/le.c \
	$(SRC)/bt/generic.c \
	$(SRC)/bt/profiles.c \
	$(SRC)/bt/hci_usb.c \
	$(SRC)/bt/hci_rx.c \
	$(SRC)/bt/usb_desc.c \
	$(SRC)/bt/evstream.c \
	$(SRC)/bt/smp_crypto.c \
	$(SRC)/bt/crc32.c \
	$(SRC)/bt/util.c \
	$(SRC)/bt/log.c \
	$(SRC)/gc_usb.c \
	$(SRC)/controller_ds4.c \
	$(SRC)/controller_mamba.c \
	$(SRC)/controller_nintendo.c \
	$(SRC)/usb_helpers.c \
	$(SRC)/bt/ps5_vpad.c \
	$(SRC)/gc_service_audio.c \
	$(SRC)/audio/gc_audio.c \
	$(SRC)/audio/gc_audio_settings.c \
	$(SRC)/audio/gc_audio_dsp.c \
	$(SRC)/audio/gc_audio_capture.c \
	$(SRC)/audio/gc_audio_capture_sce.c \
	$(SRC)/gc_service_capture.c \
	$(SRC)/audio/gc_audio_avdtp.c \
	$(SRC)/audio/gc_audio_codec.c \
	$(SRC)/audio/gc_audio_controls.c \
	$(SRC)/gc_file_safety.c \
	$(SRC)/gc_service_inject.c

# Explicit additive diagnostic closure: the real ordinary service modules only.
# Full resident sources above remain the default and are never replaced.
ifeq ($(VARIANT),diagnostic)
SOURCES := $(addprefix $(SRC)/,$(addsuffix .c,gc_app_service gc_runtime_commands gc_app_pad gc_config gc_controller_commands gc_profile gc_ini gc_ipc gc_log_tail gc_service_audio gc_service_capture audio/gc_audio audio/gc_audio_capture audio/gc_audio_capture_sce audio/gc_audio_settings audio/gc_audio_dsp gc_file_safety))
VARIANT_CFLAGS := -DGC_APP_SERVICE_DIAGNOSTIC=1
endif
ifeq ($(VARIANT),resident-no-capture)
VARIANT_CFLAGS := -DGC_APP_SERVICE_NO_AUDIO_CAPTURE=1
endif

CFLAGS := -std=c11 -D_DEFAULT_SOURCE= -Wall -Wextra -Wshadow -Werror -O2 -fPIC \
	-fno-stack-protector -ffunction-sections -fdata-sections \
	-D__PROSPERO__ $(VARIANT_CFLAGS) -DGC_APP_SERVICE_PORT=$(PORT) -DGC_IPC_BIND_LAN=1 \
	-I$(SRC) -I$(SRC)/bt -I$(PORTS)/include

LDFLAGS := -Wl,--gc-sections,--build-id=sha1 -L$(PS5_PAYLOAD_SDK)/target/lib -L$(PORTS)/lib -lScePad -lSceUserService -lSceNotification -lkernel -lkernel_sys -ldl -ljson-c -lpthread -lm

# Resident routing has a separate host-only gate. Never confuse it with
# `test` below, which is a live console deployment, not a host test.

all: $(ELF)

$(ELF): $(SOURCES) $(wildcard $(SRC)/*.h $(SRC)/bt/*.h $(SRC)/audio/*.h) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

clean:
	rm -f $(ELF)

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
