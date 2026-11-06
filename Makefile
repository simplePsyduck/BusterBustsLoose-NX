#---------------------------------------------------------------------------------
# Nintendo Switch homebrew (.nro): devkitA64 + libnx + the switch-sdl2 portlib.
# Based on the standard libnx application template.
#
#   (dkp-)pacman -S switch-dev switch-sdl2
#   make -f Makefile.switch                 -> BusterBustsLoose.nro
#   make -f Makefile.switch NXLINK=1        -> same, with stdout/stderr sent to `nxlink -s`
#   make -f Makefile.switch clean
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := BusterBustsLoose
BUILD       := build-switch
# every .c in these folders is built (src/ = frontend + game patches, src/snes/ = LakeSnes core)
SOURCES     := src src/snes
# (not third_party/SDL2-*: that is the Windows SDL2; the console uses the portlib's)
INCLUDES    := src src/snes third_party/stb

APP_TITLE   := Buster Busts Loose!
APP_AUTHOR  := darhkfox
APP_VERSION := 1.0-switch-poc
ICON        := switch/icon.jpg

SWITCH_PORTLIBS := $(DEVKITPRO)/portlibs/switch
LIBDIRS     := $(SWITCH_PORTLIBS) $(LIBNX)

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
DEFINES     := -D__SWITCH__
ifneq ($(strip $(NXLINK)),)
DEFINES     += -DBBL_NXLINK
endif

# the emulation core needs optimising; -O2 matches the Android build
CFLAGS      := -g -Wall -O2 -ffunction-sections -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
               $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) $(shell $(SWITCH_PORTLIBS)/bin/sdl2-config --cflags)
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS     := -g $(ARCH)
LDFLAGS      = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# sdl2-config lists SDL2 and what it needs (EGL, GLESv2, glapi, drm_nouveau)
LIBS        := $(shell $(SWITCH_PORTLIBS)/bin/sdl2-config --libs) -lnx -lm

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD       := $(CC)
export OFILES   := $(CFILES:.c=.o)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile.switch

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all    : $(OUTPUT).nro
$(OUTPUT).nro  : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf  : $(OFILES)

-include $(DEPENDS)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
