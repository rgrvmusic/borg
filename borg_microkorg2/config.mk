##############################################################################
# Project Configuration
#

PROJECT      := borg_microkorg2
PROJECT_TYPE := osc

##############################################################################
# Sources
#
# header.c is C and the rest is C++, as in every unit here.
#
# borg.cc is the switch plus the panel paint. The switch is borg_drumlogue's,
# carried to this platform unchanged in shape: execve(/proc/self/exe) under an
# LD_PRELOAD, same pid, because the microKORG2 kernel permits execve and refuses
# fork/clone/posix_spawn exactly as the drumlogue's does. What is new is that the
# object being preloaded is not only a marker: it watches the daemon's reads of
# /dev/ttymxc2, so the unit can see PLAY and FUNCTION 3 and open a popup.
#
# The trigger is a note, any C (pitch class 0), read from the oscillator runtime context
# in unit_render. A microKORG2 osc unit gets no note callback -- the interface is
# init/reset/render, parameters and tempo -- but render is handed pitch[8] and a
# trigger bit array, so the note is visible there and only there. A parameter
# could not be the trigger: the host pushes a program's saved values in after
# unit_init, so a parameter shows what was set, never what was asked.
#

CSRC  = header.c

CXXSRC = unit.cc
CXXSRC += borg.cc

# The object execve preloads, carried inside the unit. shim_blob.S is an .incbin
# of libborg.so, which build-mk2.sh compiles with the platform's own cross
# compiler immediately before make runs -- the same glibc 2.24 microkorgd links
# against, which is the only way an LD_PRELOAD into it is not a guess. Same
# choice, same reason, as borg_blob.S on the drumlogue.
ASMXSRC = shim_blob.S

##############################################################################
# Include Paths
#

UINCDIR =

##############################################################################
# Libraries
#
# -lpthread because the switch reads and resets the calling thread's signal
# mask, and this glibc keeps pthread_sigmask in libpthread. No thread is created.
#

ULIBDIR =
ULIBS   = -lc -lpthread

##############################################################################
# Macros
#

UDEFS += -DUNIT_TARGET_PLATFORM_MICROKORG2
UDEFS += -D_GNU_SOURCE

UXXDEFS += -DUNIT_TARGET_PLATFORM_MICROKORG2
UXXDEFS += -D_GNU_SOURCE
