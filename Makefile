include $(APPDIR)/Make.defs

PROGNAME  = bytechecker
PRIORITY  = 1000
STACKSIZE = 2048
MODULE    = $(CONFIG_BYTECHECKER)

MAINSRC = bc_main.c

ifeq ($(CONFIG_BYTECHECKER),y)
  CFLAGS += -include bytechecker.h
endif

include $(APPDIR)/Application.mk
