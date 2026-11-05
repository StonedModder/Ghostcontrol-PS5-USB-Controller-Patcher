# Legacy payload cross-build settings. The default app-service build is driven
# by build-openprospero.py; host tests do not require a console SDK.
PYTHON ?= python3
OPENPROSPERO_ROOT ?= $(abspath ../../sdk)
OPENPROSPERO_SDK ?= $(OPENPROSPERO_ROOT)/sdk
OPENPROSPERO_BIN ?= $(OPENPROSPERO_ROOT)/build/bin
OPENPROSPERO_CLANG ?= clang
CC := $(OPENPROSPERO_CLANG)
OP_RESOURCE_DIR = $(shell $(OPENPROSPERO_CLANG) -print-resource-dir)
CFLAGS += --target=x86_64-sie-ps5 -nostdinc -femulated-tls -fPIE \
    -isystem $(OP_RESOURCE_DIR)/include -isystem $(OPENPROSPERO_SDK)/target/include \
    -isystem $(OPENPROSPERO_ROOT)/include
LDFLAGS += -B$(OPENPROSPERO_BIN)
export OPENPROSPERO_SDK
export OPENPROSPERO_LINK_FLAGS := --profile native
