#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# External tools
#---------------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
MAKEROM 	?= makerom.exe
BANNERTOOL 	?= bannertool.exe
else
MAKEROM 	?= makerom
BANNERTOOL 	?= bannertool
endif

RSF_FILE	:= app/build-cia.rsf
BNR_IMAGE	:= app/banner.cgfx
BNR_AUDIO	:= app/audio.wav
ICON		:= app/icon.png

APP_TITLE	:= CTGP-7 Mod Manager
APP_AUTHOR	:= NitroShell and Bonkmaykr

TARGET		:= CTGP-7-Mod-Manager
BUILD		:= build
SOURCES		:= source
INCLUDES	:= include
ROMFS		:= romfs

#---------------------------------------------------------------------------------
# Options for code generation
#---------------------------------------------------------------------------------
ARCH	:= -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS	:= -g -Wall -Wextra -Os -mword-relocations \
		   -ffunction-sections -fdata-sections \
		   $(ARCH)

CFLAGS		+= $(INCLUDE) -D__3DS__
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++14 -Wno-psabi
ASFLAGS		:= -g $(ARCH)
LDFLAGS		:= -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections

LIBS	:= -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljson-c -lz -lcitro2d -lcitro3d -lctru -lm
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:= $(CURDIR)/$(TARGET)
export TOPDIR	:= $(CURDIR)
export VPATH	:= $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:= $(CURDIR)/$(BUILD)

CFILES		:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN     := $(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o)
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES         := $(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h)

export INCLUDE	:= $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
		           $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
		           -I$(CURDIR)/$(BUILD)
export LIBPATHS	:= $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXDEPS := $(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean

all: $(BUILD) $(DEPSDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(OUTPUT).cia $(DEPSDIR)
	@rm -f app/banner.bin app/icon.bin

else

.PHONY: all

all: $(OUTPUT).3dsx $(OUTPUT).cia

$(OUTPUT).3dsx	: $(OUTPUT).elf $(_3DSXDEPS)
$(OFILES_SOURCES) : $(HFILES)
$(OUTPUT).elf	: $(OFILES)

$(OUTPUT).cia	: $(OUTPUT).elf $(OUTPUT).smdh
	@$(BANNERTOOL) makebanner -ci "../app/banner.cgfx" -a "../app/audio.wav" -o "../app/banner.bin"
	@$(BANNERTOOL) makesmdh -i "../app/icon.png" -s "$(TARGET)" -l "$(APP_TITLE)" -p "$(APP_AUTHOR)" -o "../app/icon.bin" \
		--flags visible,ratingrequired --cero 153 --esrb 153 --usk 153 --pegigen 153 --pegiptr 153 --pegibbfc 153 --cob 153 --grb 153 --cgsrr 153
	@$(MAKEROM) -f cia -target t -exefslogo -o "$(OUTPUT).cia" -elf "$(OUTPUT).elf" -rsf "../app/build-cia.rsf" -banner "../app/banner.bin" -icon "../app/icon.bin" -DAPP_ROMFS=".."

%.bin.o	%_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

.PRECIOUS: %.shbin

%.shbin.o %_shbin.h : %.shbin
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

endif