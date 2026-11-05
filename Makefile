# Ghostcontrol Toolbox 2.0
#
# Two artifacts, deployed differently:
#
#   core/  the controller service, a payload ELF sent to the ELF loader
#   ui/    the toolbox, a native homebrew app installed as a title
#
# Neither one patches the Shell, so neither depends on a firmware offset
# table or a bundle fingerprint.

.PHONY: all core ui deps clean deploy-core deploy-ui

all: ui

# The app build also builds and packages its controller service.
.PHONY: pkg
pkg: ui

deps:
	$(MAKE) -C ui deps

core:
	$(MAKE) -C core

ui:
	$(MAKE) -C ui


deploy-core:
	$(MAKE) -C core deploy

deploy-ui:
	$(MAKE) -C ui deploy

clean:
	$(MAKE) -C core clean
	$(MAKE) -C ui clean
