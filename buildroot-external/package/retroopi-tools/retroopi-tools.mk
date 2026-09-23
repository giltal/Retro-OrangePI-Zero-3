################################################################################
#
# retroopi-tools
#
# Small on-target helpers that live in <project>/tools.
#
################################################################################

RETROOPI_TOOLS_VERSION = local
RETROOPI_TOOLS_SITE = $(BR2_EXTERNAL_RETROOPI_PATH)/../tools
RETROOPI_TOOLS_SITE_METHOD = local
RETROOPI_TOOLS_LICENSE = GPL-2.0

RETROOPI_TOOLS_PROGS = inject-input seed-credit volumed

define RETROOPI_TOOLS_BUILD_CMDS
	$(foreach p,$(RETROOPI_TOOLS_PROGS), \
		$(TARGET_CC) $(TARGET_CFLAGS) -O2 -Wall -o $(@D)/$(p) $(@D)/$(p).c ; \
	)
endef

define RETROOPI_TOOLS_INSTALL_TARGET_CMDS
	$(foreach p,$(RETROOPI_TOOLS_PROGS), \
		$(INSTALL) -D -m 0755 $(@D)/$(p) $(TARGET_DIR)/usr/bin/$(p) ; \
	)
endef

$(eval $(generic-package))
