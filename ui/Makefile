# Ghostcontrol Toolbox 2.0 — native UI app on the blackbearreloaded kit.
# OpenProspero is not the driver. The UI kit links the app; GhosttoothAPI
# is a separate ELF loaded on the console loader.
#
# Upstream clones. GC_VENDOR_DIR relocates them for a checkout that cannot
# delete files; the upstream build removes scratch files as it goes.
VENDOR      ?= $(if $(GC_VENDOR_DIR),$(GC_VENDOR_DIR),$(CURDIR)/vendor)
BOILERPLATE := $(VENDOR)/ps5-native-app-boilerplate
UI_KIT      := $(if $(GC_UI_KIT),$(GC_UI_KIT),$(VENDOR)/ps5-homebrew-ui)
TOOLS       := ../tools
APP_NAME    ?= Ghostcontrol Toolbox
PS5_HOST    ?= 192.168.69.8
FTP_PORT    ?= 2120
TITLE_ID    ?= PPSA62838

USE_CCACHE       ?= $(if $(shell command -v ccache 2>/dev/null),1,0)
export USE_CCACHE

TEST_DIR         ?= $(HOME)/gc-usability-integrated/ui
HOST_CC          ?= gcc
HOST_CXX         ?= g++
HOST_TEST_CFLAGS ?= -D_DEFAULT_SOURCE -std=c11 -Wall -Wextra -Wshadow -Werror
ROUND_TRIP_CFLAGS ?= -std=c11 -Wall -Wextra -Wshadow -Werror

.DEFAULT_GOAL := app
.PHONY: app deps compose companion-asset prune-dist title-id test test-usability test-shell deploy deploy-dry-run clean print-title-id pkg

deps:
	@GC_VENDOR_DIR=$(VENDOR) $(TOOLS)/fetch-deps.sh
	@$(MAKE) -C $(UI_KIT) deps

# Stable identity: PPSA62838 is the published product title ID. The self
# updater requires one fixed ID so an installed copy recognises newer
# releases. Override with TITLE_ID= on the command line only for throwaway
# dev installs that must not collide with the product title.
title-id:
	@mkdir -p "$(BUILD_ROOT)/identity/sce_sys"
	@cp -a sce_sys/. "$(BUILD_ROOT)/identity/sce_sys/"
	@python3 $(TOOLS)/new-title-id.py "$(BUILD_ROOT)/identity/sce_sys/param.json" \
		--name "$(APP_NAME)" --record .title-id \
		$(if $(TITLE_ID),--title-id $(TITLE_ID),) > /dev/null
	@printf '==> [title-id] %s\n' "$$(cat .title-id)"

print-title-id:
	@cat .title-id 2>/dev/null || { echo "no title id yet; run make title-id" >&2; exit 1; }

BUILD_ROOT ?= $(if $(GC_BUILD_ROOT),$(GC_BUILD_ROOT),$(CURDIR)/.build/root)
GC_APP_SERVICE_PORT ?= 39073
GHOSTTOOTH_ELF ?= $(CURDIR)/../GhosttoothAPI/GhosttoothAPI.elf
# APP-SVC-04 package gate. Notice files to ship beside the companion
# (space-separated paths), and for a release the pinned reviewed SHA-256.
# GC_RELEASE=1 makes every verification step strict: known clean source
# revision, at least one notice, and a mandatory externally reviewed hash.
GHOSTTOOTH_NOTICES ?=
GHOSTTOOTH_SHA256 ?=
GC_RELEASE ?=
COMPANION_ASSETS := $(BUILD_ROOT)/companion/ghostcontrol
MANIFEST_TOOL := python3 $(TOOLS)/companion-manifest.py
MANIFEST_STRICT := $(if $(GC_RELEASE),--release $(if $(GHOSTTOOTH_SHA256),--expect-sha256 $(GHOSTTOOTH_SHA256),),)

companion-asset:
	@test -f "$(GHOSTTOOTH_ELF)" || { \
		echo "missing GhosttoothAPI ELF: $(GHOSTTOOTH_ELF)" >&2; \
		echo "build it first: make -C ../GhosttoothAPI" >&2; exit 2; }
	@mkdir -p "$(COMPANION_ASSETS)"
	@cp "$(GHOSTTOOTH_ELF)" "$(COMPANION_ASSETS)/GhosttoothAPI.elf"
	@sed -n 's/^#define GC_PRODUCT_VERSION "\(.*\)"$$/\1/p' ../core/src/gc_product_version.h \
		> "$(COMPANION_ASSETS)/version.txt"
	@test -s "$(COMPANION_ASSETS)/version.txt" || { echo "empty product version" >&2; exit 2; }
	@printf '==> [GhosttoothAPI] %s bytes\n' "$$(wc -c < "$(COMPANION_ASSETS)/GhosttoothAPI.elf")"
	@$(MANIFEST_TOOL) generate --elf "$(COMPANION_ASSETS)/GhosttoothAPI.elf" --source-root .. \
		$(foreach n,$(GHOSTTOOTH_NOTICES),--notice $(n))
	@$(MANIFEST_TOOL) verify --dir "$(COMPANION_ASSETS)" $(MANIFEST_STRICT)

compose: companion-asset title-id
	@test -d $(UI_KIT)/src/ui || { \
		echo "missing $(UI_KIT); run 'make deps' first" >&2; exit 2; }
	@GC_APP_SCE_SYS="$(BUILD_ROOT)/identity/sce_sys" GC_COMPANION_ASSETS="$(COMPANION_ASSETS)" \
		bash $(TOOLS)/compose-kit-root.sh $(UI_KIT) $(CURDIR) $(BUILD_ROOT)
	@$(MANIFEST_TOOL) verify-tree --root $(BUILD_ROOT)/assets $(MANIFEST_STRICT)

prune-dist: compose
	@test ! -d $(BUILD_ROOT)/dist || rm -rf $(BUILD_ROOT)/dist/*

app: companion-asset title-id compose prune-dist
	APP_INCLUDE_PATHS="src/ghostcontrol" APP_DEFINITIONS="GC_NATIVE_PAD_PS5=1 GC_CLIENT_PORT=$(GC_APP_SERVICE_PORT)" USE_CCACHE=$(USE_CCACHE) \
		$(MAKE) -C $(BUILD_ROOT) app
	@python3 $(TOOLS)/zip-open-modes.py "$(BUILD_ROOT)/dist/$$(cat .title-id).zip"
	@printf '==> [app] %s/dist/%s\n' "$(BUILD_ROOT)" "$$(cat .title-id)"
	@$(MANIFEST_TOOL) verify-tree --root $(BUILD_ROOT)/dist/$$(cat .title-id) $(MANIFEST_STRICT)
	@$(MAKE) verify-companion-dist

# Narrow verification-only entry point: no app/deps/native build prerequisites.
COMPANION_DIST ?= $(BUILD_ROOT)/dist
.PHONY: verify-companion-dist
verify-companion-dist:
	@$(MANIFEST_TOOL) verify-dist --root "$(COMPANION_DIST)" $(MANIFEST_STRICT)

.PHONY: pkg
pkg: app


# Host tests only: the socket path needs a console with a service on it.
test: test-completion test-usability test-qr-assets test-support-cards test-profile-features
	@mkdir -p $(TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -Isrc -o $(TEST_DIR)/gc-client-test \
		tests/test_gc_client.c src/gc_client.c
	$(TEST_DIR)/gc-client-test
	# Links the service's real IPC server against this client, so the
	# handshake is proven without a console.
	$(HOST_CC) $(ROUND_TRIP_CFLAGS) -Isrc -I../core/src -pthread \
		-o $(TEST_DIR)/gc-round-trip-test \
		tests/test_service_round_trip.c src/gc_client.c ../core/src/gc_ipc.c
	$(TEST_DIR)/gc-round-trip-test
	$(HOST_CC) $(ROUND_TRIP_CFLAGS) -DGC_IPC_BIND_LAN=1 -Isrc -I../core/src -pthread \
		-o $(TEST_DIR)/gc-lan-round-trip-test \
		tests/test_service_round_trip.c src/gc_client.c ../core/src/gc_ipc.c
	$(TEST_DIR)/gc-lan-round-trip-test
	# The per-call timeout, against the same real server: a command slower
	# than the default is cut off at the default and still succeeds on a
	# longer budget, and a LOG_TAIL-class reply fits the app's poll buffer.
	$(HOST_CC) $(ROUND_TRIP_CFLAGS) -Isrc -I../core/src -pthread \
		-o $(TEST_DIR)/gc-timeout-test \
		tests/test_service_timeout.c src/gc_client.c ../core/src/gc_ipc.c
	$(TEST_DIR)/gc-timeout-test
	# Command queue and toast wording on the real ServiceSnapshot, without a
	# console. gc_client.c is C, so it is compiled as C and linked into the
	# C++ test rather than compiled as C++.
	$(HOST_CC) $(HOST_TEST_CFLAGS) -Isrc -c src/gc_client.c \
		-o $(TEST_DIR)/gc_client_test.o
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -pthread \
		-o $(TEST_DIR)/snapshot-queue-test \
		tests/test_snapshot_queue.cpp src/service_snapshot.cpp \
		$(TEST_DIR)/gc_client_test.o
	$(TEST_DIR)/snapshot-queue-test
	# The viewer's ported gyro math and its part map, and a real draw through
	# the kit's mesh primitive. draw_list.cpp and triangulate.cpp are the only
	# kit translation units the CPU-only polygon path needs; the assertion
	# trips if the asset is not beside the logo where the app loads it.
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -I$(UI_KIT)/src \
		-o $(TEST_DIR)/controller3d-test \
		tests/test_controller3d.cpp src/controller3d.cpp \
		$(UI_KIT)/src/gfx/draw_list.cpp $(UI_KIT)/src/gfx/triangulate.cpp \
		$(UI_KIT)/src/gfx/font.cpp
	$(TEST_DIR)/controller3d-test
	python3 $(TOOLS)/test_new_title_id.py
	# APP-SVC-04 package gate, cross-checked against the app's runtime check.
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc \
		-o $(TEST_DIR)/check-companion-manifest tests/check_companion_manifest.cpp \
		src/service_launcher.cpp src/sha256.cpp
	python3 $(TOOLS)/test_companion_manifest.py --checker $(TEST_DIR)/check-companion-manifest
	python3 $(TOOLS)/test_companion_manifest_boundaries.py --checker $(TEST_DIR)/check-companion-manifest

# Tests exercise production seams; native backend mocks are explicitly NOT
# hardware evidence. Keep build artifacts on WSL ext4, not the drvfs checkout.
test-shell:
	@mkdir -p $(TEST_DIR)
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -I$(UI_KIT)/src \
		-o $(TEST_DIR)/row-actions-test tests/test_row_actions.cpp
	$(TEST_DIR)/row-actions-test
	$(HOST_CXX) -std=c++20 -O2 -DGC_ROW_BASELINE -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -Isrc -I$(UI_KIT)/src \
		-o $(TEST_DIR)/real-row-test tests/test_row_actions.cpp
	$(TEST_DIR)/real-row-test
	$(HOST_CC) $(HOST_TEST_CFLAGS) -Isrc -c src/gc_client.c -o $(TEST_DIR)/overlay-client.o
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -pthread \
		-o $(TEST_DIR)/overlay-dispatch-test tests/test_overlay_dispatch.cpp src/service_snapshot.cpp \
		$(TEST_DIR)/overlay-client.o
	$(TEST_DIR)/overlay-dispatch-test
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc \
		-o $(TEST_DIR)/sha256-test tests/test_sha256.cpp src/sha256.cpp
	$(TEST_DIR)/sha256-test
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc \
		-o $(TEST_DIR)/service-launcher-test tests/test_service_launcher.cpp src/service_launcher.cpp src/sha256.cpp
	$(TEST_DIR)/service-launcher-test
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc \
		-o $(TEST_DIR)/service-submit-boundaries-test tests/test_service_submit_boundaries.cpp src/service_launcher.cpp src/sha256.cpp
	$(TEST_DIR)/service-submit-boundaries-test
	# APP-SVC-02 lifecycle owner: strict STATUS identity, probe-before-send,
	# one committed automatic attempt, refused duplicates, cancel without EXIT.
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -pthread \
		-o $(TEST_DIR)/service-lifecycle-test tests/test_service_lifecycle.cpp \
		src/service_lifecycle.cpp src/service_launcher.cpp src/sha256.cpp
	$(TEST_DIR)/service-lifecycle-test
	# Synthetic conditional contracts, actual ordinary flock, late/cancel release.
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -pthread \
		-o $(TEST_DIR)/service-lifecycle-contracts-test tests/test_service_lifecycle_contracts.cpp \
		src/service_lifecycle.cpp src/service_launcher.cpp src/sha256.cpp
	$(TEST_DIR)/service-lifecycle-contracts-test
	# Fortified libc inlines (for example __poll_chk) would bypass --wrap,
	# so this socket-seam test is built without _FORTIFY_SOURCE.
	$(HOST_CXX) -std=c++20 -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wextra -Wshadow -Werror \
		-Isrc -I$(UI_KIT)/src \
		-Wl,--wrap=socket,--wrap=setsockopt,--wrap=getsockopt,--wrap=fcntl,--wrap=connect,--wrap=getsockname,--wrap=send,--wrap=shutdown,--wrap=poll,--wrap=recv,--wrap=close \
		-o $(TEST_DIR)/service-launcher-platform-test tests/test_service_launcher_platform.cpp \
		src/service_launcher_platform.cpp
	$(TEST_DIR)/service-launcher-platform-test
	$(HOST_CXX) -std=c++20 -O2 -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections \
		-Isrc -I$(UI_KIT)/src -o $(TEST_DIR)/service-autostart-test \
		tests/test_service_autostart.cpp src/service_launcher.cpp src/service_lifecycle.cpp src/sha256.cpp src/screens/*.cpp src/task_screen.cpp src/task_workspace.cpp src/task_pages_*.cpp \
		src/service_snapshot.cpp src/overlay.cpp src/ghostscape.cpp src/app_service_preferences.cpp src/app_service_preferences_worker.cpp \
		$(TEST_DIR)/overlay-client.o -Wl,--wrap=gc_client_send_cancelable,--wrap=gc_client_last_connect_errno,--wrap=gc_client_port \
		$(UI_KIT)/src/ui/theme.cpp $(UI_KIT)/src/ui/components/color_picker.cpp \
		$(UI_KIT)/src/ui/components/keyboard.cpp $(UI_KIT)/src/ui/components/component.cpp \
		$(UI_KIT)/src/ui/components/key_binder.cpp $(UI_KIT)/src/ui/components/range_slider.cpp \
		$(UI_KIT)/src/ui/components/text_field.cpp $(UI_KIT)/src/ui/components/wheel_picker.cpp \
		$(UI_KIT)/src/ui/components/dialog.cpp src/beta_disclaimer.cpp
	$(TEST_DIR)/service-autostart-test
	# Launch beta disclaimer: preference codec + real ToolboxUi arm/Continue/
	# Don't-show-again/next-launch. CPU-side kit objects only (no GL harness).
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc \
		-o $(TEST_DIR)/beta-disclaimer-pref-test tests/test_beta_disclaimer.cpp src/beta_disclaimer.cpp
	GC_BETA_DISCLAIMER_TEST_ROOT=$$(mktemp -d) $(TEST_DIR)/beta-disclaimer-pref-test
	$(HOST_CC) $(HOST_TEST_CFLAGS) -Isrc -c src/gc_client.c -o $(TEST_DIR)/beta-gc-client.o
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Werror -Isrc -I$(UI_KIT)/src -pthread \
		-o $(TEST_DIR)/beta-disclaimer-ui-test tests/test_beta_disclaimer_ui.cpp \
		src/toolbox_ui.cpp src/task_screen.cpp src/task_workspace.cpp src/task_pages_*.cpp \
		src/screens/*.cpp src/service_snapshot.cpp src/overlay.cpp src/ghostscape.cpp \
		src/app_service_preferences.cpp src/app_service_preferences_worker.cpp \
		src/service_launcher.cpp src/service_lifecycle.cpp src/sha256.cpp \
		src/beta_disclaimer.cpp $(TEST_DIR)/beta-gc-client.o \
		$(UI_KIT)/src/ui/*.cpp $(UI_KIT)/src/ui/components/*.cpp \
		$(UI_KIT)/src/core/*.cpp \
		$(UI_KIT)/src/gfx/draw_list.cpp $(UI_KIT)/src/gfx/triangulate.cpp \
		$(UI_KIT)/src/gfx/font.cpp -lm
	GC_BETA_DISCLAIMER_UI_ROOT=$$(mktemp -d) \
	GC_BINDING_TEST_FONT=$(UI_KIT)/assets/fonts/inter-regular.huifont \
		$(TEST_DIR)/beta-disclaimer-ui-test

test-usability: test-shell
	@mkdir -p $(TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -Isrc -c src/gc_client.c -o $(TEST_DIR)/gc_client.o
	$(HOST_CC) $(ROUND_TRIP_CFLAGS) -I../core/src -pthread -c ../core/src/gc_ipc.c -o $(TEST_DIR)/gc_ipc.o
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I../core/src -c ../core/src/gc_app_pad.c -o $(TEST_DIR)/gc_app_pad.o
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -I../core/src -pthread \
		-o $(TEST_DIR)/snapshot-actions-test tests/test_snapshot_actions.cpp src/service_snapshot.cpp \
		$(TEST_DIR)/gc_client.o $(TEST_DIR)/gc_ipc.o
	$(TEST_DIR)/snapshot-actions-test
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -I../core/src -pthread \
		-o $(TEST_DIR)/native-mailbox-test tests/test_native_mailbox.cpp src/service_snapshot.cpp \
		$(TEST_DIR)/gc_client.o $(TEST_DIR)/gc_ipc.o
	$(TEST_DIR)/native-mailbox-test
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -I../core/src \
		-o $(TEST_DIR)/native-pad-test tests/test_native_pad_bridge.cpp src/native_pad_bridge.cpp $(TEST_DIR)/gc_app_pad.o
	$(TEST_DIR)/native-pad-test
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc \
		-o $(TEST_DIR)/screen-contracts-test tests/test_screen_contracts.cpp src/screens/*.cpp \
		src/service_snapshot.cpp tests/test_ui_host_log.cpp $(TEST_DIR)/gc_client.o -pthread
	$(TEST_DIR)/screen-contracts-test

# Focused real preference/page/capability/feedback regressions, both modes.
# A fresh root preserves prior receipts and makes repeated make test usable.
COMPLETION_TEST_ROOT ?= $(TEST_DIR)/completion-$(shell date +%Y%m%d-%H%M%S-%N)
COMPLETION_EVIDENCE ?= $(COMPLETION_TEST_ROOT)/evidence
.PHONY: test-completion
test-completion:
	python3 $(TOOLS)/test_service_endpoint_defaults.py
	python3 $(TOOLS)/test_native_candidate_options.py
	python3 $(TOOLS)/test-ui-completion.py --build-root "$(COMPLETION_TEST_ROOT)" \
		--evidence "$(COMPLETION_EVIDENCE)"

# Shipped support codes: every PNG must decode back to exactly the payload the
# card prints. This is the same decoder the generator verified against an
# independently produced code, so a wrong asset cannot ship quietly.
.PHONY: test-qr-assets
test-qr-assets:
	python3 $(TOOLS)/make-qr-assets.py --verify-only

# The companion's own profile parser, compiled as C for the test below, so a
# mapping this build produces is judged by the service code that will consume it
# rather than by a second opinion of our own.
PROFILE_CHECK_OBJS := $(TEST_DIR)/gc-profile.o $(TEST_DIR)/gc-ini.o
$(TEST_DIR)/gc-profile.o: ../core/src/gc_profile.c
	@mkdir -p $(TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I../core/src -c $< -o $@
$(TEST_DIR)/gc-ini.o: ../core/src/gc_ini.c
	@mkdir -p $(TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -I../core/src -c $< -o $@

# Held-modifier (layer) mappings offered by the profile picker, validated by the
# service's parser. Profile import/export remains service-owned: the narrow
# source contract locks the shipped page to PROFILE_IMPORT/PROFILE_EXPORT and
# rejects the former title-sandbox layout directory.
.PHONY: test-profile-features
test-profile-features: $(PROFILE_CHECK_OBJS)
	python3 tests/test_profile_service_path.py
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -Isrc -I../core/src -I$(UI_KIT)/src \
		-o $(TEST_DIR)/profile-chords-test tests/test_profile_chords.cpp \
		$(PROFILE_CHECK_OBJS)
	$(TEST_DIR)/profile-chords-test

# The community and funding cards, on the production painter with real fonts.
# Builds the snapshot harness objects once and links the host assertions to them.
.PHONY: test-support-cards
test-support-cards:
	python3 $(TOOLS)/test-support-cards.py \
		--build-root $(TEST_DIR)/support-cards-$(shell date +%Y%m%d-%H%M%S-%N)

# Deploy the boilerplate dist folder. GhosttoothAPI is a separate ELF.
deploy: app
	python3 $(TOOLS)/deploy-app.py --host "$(PS5_HOST)" --port "$(FTP_PORT)" \
		--dist $(BUILD_ROOT)/dist/$$(cat .title-id) \
		--title-id $$(cat .title-id)

deploy-dry-run: app
	python3 $(TOOLS)/deploy-app.py --host "$(PS5_HOST)" --port "$(FTP_PORT)" \
		--dist $(BUILD_ROOT)/dist/$$(cat .title-id) \
		--title-id $$(cat .title-id) --dry-run

clean:
	rm -rf .build $(BUILD_ROOT)

# Reproduce the launch-dialog soft lock through the real mapping chain
# (PadSample -> InputTracker -> ToolboxUi), including the intercepted bit.
.PHONY: test-beta-dialog-chain
test-beta-dialog-chain:
	@mkdir -p $(TEST_DIR)
	$(HOST_CC) $(HOST_TEST_CFLAGS) -Isrc -c src/gc_client.c -o $(TEST_DIR)/chain-gc-client.o
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -Werror -Isrc -I$(UI_KIT)/src -pthread \
		-o $(TEST_DIR)/beta-dialog-chain-test tests/test_beta_dialog_chain.cpp \
		src/toolbox_ui.cpp src/task_screen.cpp src/task_workspace.cpp src/task_pages_*.cpp \
		src/screens/*.cpp src/service_snapshot.cpp src/overlay.cpp src/ghostscape.cpp \
		src/app_service_preferences.cpp src/app_service_preferences_worker.cpp \
		src/service_launcher.cpp src/service_lifecycle.cpp src/sha256.cpp \
		src/beta_disclaimer.cpp $(TEST_DIR)/chain-gc-client.o \
		$(UI_KIT)/src/ui/*.cpp $(UI_KIT)/src/ui/components/*.cpp \
		$(UI_KIT)/src/core/*.cpp \
		$(UI_KIT)/src/gfx/draw_list.cpp $(UI_KIT)/src/gfx/triangulate.cpp \
		$(UI_KIT)/src/gfx/font.cpp -lm
	GC_BETA_DIALOG_CHAIN_ROOT=$$(mktemp -d) $(TEST_DIR)/beta-dialog-chain-test
