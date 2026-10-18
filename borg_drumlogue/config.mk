PROJECT      := borg
PROJECT_TYPE := synth

CSRC  = header.c

CXXSRC = unit.cc
CXXSRC += borg.cc

# borg_blob.S embeds libborg.so; build it before running make.
ASMXSRC = borg_blob.S

UINCDIR =

ULIBDIR =
ULIBS   = -lc -lpthread

UDEFS += -DUNIT_TARGET_PLATFORM_DRUMLOGUE

UDEFS += -D_GNU_SOURCE

UXXDEFS += -DUNIT_TARGET_PLATFORM_DRUMLOGUE
UXXDEFS += -D_GNU_SOURCE

# Optional overrides for fixed paths, used by native tests.
-include $(EXTRA_MK)
