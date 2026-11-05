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
TITLE_ID    ?=

USE_CCACHE       ?= $(if $(shell command -v ccache 2>/dev/null),1,0)
export USE_CCACHE


.DEFAULT_GOAL := app
.PHONY: app deps compose companion-asset prune-dist title-id deploy deploy-dry-run clean print-title-id pkg

deps:
	@GC_VENDOR_DIR=$(VENDOR) $(TOOLS)/fetch-deps.sh
	@$(MAKE) -C $(UI_KIT) deps

# A new identity per build. Pass TITLE_ID=PPSAnnnnn to pin one instead.
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
.PHONY: 
.PHONY: 
PROFILE_CHECK_OBJS := $(TEST_DIR)/gc-profile.o $(TEST_DIR)/gc-ini.o
.PHONY: 
.PHONY: 
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
