#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	ovll
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
APP_VERSION	:=	2.0.4

# Path to nx-ovlreloader
RELOADER_DIR := external/nx-ovlreloader

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# Balanced performance compiler flags
CFLAGS := -g -Wall -Os -ffunction-sections -fdata-sections \
          -fomit-frame-pointer -fno-stack-protector \
          -fno-strict-aliasing -falign-functions=16 \
          $(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__ -DVERSION=\"v$(APP_VERSION)\" -DNDEBUG

BUILD_LOADER_PLUS_DIRECTIVE := 0
CFLAGS += -DBUILD_LOADER_PLUS_DIRECTIVE=$(BUILD_LOADER_PLUS_DIRECTIVE)

#CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -fvisibility-inlines-hidden

ASFLAGS	:=	-g $(ARCH)

# Enhanced linker flags for performance
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
			-Wl,-wrap,exit -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections -Wl,--strip-all \
			-flto -fuse-linker-plugin

LIBS	:= -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# Parallel build support
#---------------------------------------------------------------------------------
MAKEFLAGS += -j$(shell nproc 2>/dev/null || echo 4)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all dist reloader

#---------------------------------------------------------------------------------
all: $(BUILD) reloader
	@echo "================================================"
	@echo "Packaging complete build..."
	@echo "================================================"
	@rm -rf out/
	@mkdir -p out/atmosphere/contents/420000000007E51A/flags
	@touch out/atmosphere/contents/420000000007E51A/flags/boot2.flag
	@cp $(CURDIR)/toolbox.json out/atmosphere/contents/420000000007E51A/toolbox.json
	@cp $(CURDIR)/$(TARGET).nsp out/atmosphere/contents/420000000007E51A/exefs.nsp
	
	@echo "Copying nx-ovlreloader components..."
	@cp -r $(RELOADER_DIR)/out/switch out/
	@cp -r $(RELOADER_DIR)/out/atmosphere/contents/420000000007E51B out/atmosphere/contents/
	
	@echo ""
	@echo "================================================"
	@echo "Complete package created in out/"
	@echo "================================================"

$(BUILD):
	@echo "Building nx-ovlloader..."
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

reloader:
	@echo "================================================"
	@echo "Building nx-ovlreloader..."
	@echo "================================================"
	@$(MAKE) --no-print-directory -C $(RELOADER_DIR) all

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
ifeq ($(strip $(APP_JSON)),)
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif
	@rm -rf out/
	@rm -f $(TARGET).zip
	@echo "Cleaning nx-ovlreloader..."
	@$(MAKE) --no-print-directory -C $(RELOADER_DIR) clean

#---------------------------------------------------------------------------------
dist: all
	@echo "================================================"
	@echo "Creating distribution package..."
	@echo "================================================"
	@rm -f nx-ovlloader.zip
	@cd out; zip -r -X ../nx-ovlloader.zip ./* -x "*.DS_Store" -x "__MACOSX" -x "._*"; cd ../
	@echo "Distribution created: nx-ovlloader.zip"
	@echo "================================================"

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
ifeq ($(strip $(APP_JSON)),)

all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

else

all	:	$(OUTPUT).nsp

$(OUTPUT).nsp	:	$(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso	:	$(OUTPUT).elf

endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
