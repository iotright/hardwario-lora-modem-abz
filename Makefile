# The base name of binary firmware files.
BASENAME ?= firmware

# The default speed (baudrate) of the AT UART interface. This value will be used
# unless the system configuration stored in EEPROM provides an alternative
# value.
DEFAULT_UART_BAUDRATE ?= 19200

# Select the regional parameter files that you wish to have included in the
# firmware. By default, all regions supported by the Type ABZ radio hardware
# (860-930 MHz) are included. Excluded regional parameters: CN470, CN779, EU433.
ENABLED_REGIONS ?= AS923 AU915 EU868 KR920 IN865 US915 RU864

# Activate the following region if no region has been selected by the
# application.
DEFAULT_ACTIVE_REGION ?= EU868

# Uncomment the following line to configure pin 14 (GPIOB15/SPI2_MOSI) as a
# factory reset pin. During normal operation, the pin should be pulled up or
# left floating. When continuously pulled down for more than five seconds and
# then pulled up, the modem resets itself to factory defaults.
#
# This functionality is only available in release builds. In debug builds, the
# same pin is used by the SWD debugging interface.
#
# Since the modem can also be reset through the AT command interface, this
# functionality is disabled by default to save a little bit of power.
#
# ENABLE_FACTORY_RESET_PIN = 1

# The default channel plan for the AS923 region. One of:
#  - CHANNEL_PLAN_GROUP_AS923_1
#  - CHANNEL_PLAN_GROUP_AS923_2
#  - CHANNEL_PLAN_GROUP_AS923_3
#  - CHANNEL_PLAN_GROUP_AS923_4
#  - CHANNEL_PLAN_GROUP_AS923_1_JP
AS923_DEFAULT_CHANNEL_PLAN ?= CHANNEL_PLAN_GROUP_AS923_1

# The default channel plan for the CN470 region. One of:
#  - CHANNEL_PLAN_20MHZ_TYPE_A
#  - CHANNEL_PLAN_20MHZ_TYPE_B
#  - CHANNEL_PLAN_26MHZ_TYPE_A
#  - CHANNEL_PLAN_26MHZ_TYPE_B
# CN470_DEFAULT_CHANNEL_PLAN ?= CHANNEL_PLAN_20MHZ_TYPE_A

# There is no protocol version negotiation between the node and the network
# server in the ABP activation mode. Thus, we need to configure the MAC protocol
# version to be used in this case manually here. Set the following variable to
# 0x01010100 (LoRaWAN 1.1.1) if your network is 1.1 compatible. Set the variable
# to 0x01000400 (LoRaWAN 1.0.4) if you are on a 1.0 network.
LORAMAC_ABP_VERSION ?= 0x01000400

# The version string to be returned by AT+VER. The version string is meant to be
# compatible with the version string returned by the original Murata Modem
# firmware. It must be of the form x.x.xx.
VERSION_COMPAT ?= 1.1.06

# The build date string to be returned by AT+VER. This build date matches the
# build date of Murata Modem in version configured through VERSION_COMPAT.
BUILD_DATE_COMPAT ?= Aug 24 2020 16:11:57

# The LoRaWAN network server may reconfigure the node's channel mask in the Join
# Accept message. If you want to prevent that from happening, e.g., if you work
# with an incorrectly configured LoRaWAN network server, uncomment the following
# variable. Use with caution. This feature is designed as a work-around for
# incorrectly configured LoRa networks. If you are unsure, leave the variable
# commented out.
# RESTORE_CHMASK_AFTER_JOIN = 1

# Select the USART port number which will receive debug messages when the
# firmware is built in debugging mode. You can select 1 or 2 here.
DEBUG_PORT ?= 1

# Select the GPIO pin connected to VDD_TCXO (pin 48). The modem will use this
# pin to control power to the TCXO IC. The TCXO IC generates clock for the
# SX1276 radio chip. The clock is turned off during sleep to conserve battery
# power. The following values are supported:
#
#   0 - Disable TCXO control. TCXO IC is assumed to be always on. This is the
#       case on Arduino MKRWAN1300 boards.
#
#   1 - PA12 (pin 1). This is the pin recommended in the Type ABZ datasheet and
#       also the default value. Hardwario LoRa devices use this pin.
#
#   2 - PB6 (pin 39). Arduino MKRWAN1310 boards use this pin.
TCXO_PIN ?= 1

# Specific target MKR1310, add a feature to deactivate the serial port based on
# PB12 GPIO. This is because Arduino team used same pins for serial line and SPI
# lines. As a consequence, Serial line activity prevent to use the SPI port and the
# onboarded SerialFlash. 
# The Arduino documentation recommands to maintain the modem in a reset mode but this
# will make the LoRaWAN session lost, so this approach is not working. With the ability
# to deactivate the serail line on request, this may solve the problem even if the right
# solution should be to redesign this board.
#   0 - Disable this specific feature
#   1 - Enable this specific feature with control line PB12 set to 0 (pull-up on the line)
MKR1310 ?= 1

################################################################################
# You shouldn't need to edit the text below under normal circumstances.        #
################################################################################

SRC_DIR := src
LIB_DIR := lib
CFG_DIR := cfg
OBJ_DIR := obj
OUT_DIR ?= out

# The current compilation type (either debug or release). Passed recursively
# across make invocations.
TYPE ?= debug

ELF ?= $(OUT_DIR)/$(TYPE)/$(BASENAME).elf
MAP ?= $(OUT_DIR)/$(TYPE)/$(BASENAME).map
BIN ?= $(OUT_DIR)/$(TYPE)/$(BASENAME).bin
HEX ?= $(OUT_DIR)/$(TYPE)/$(BASENAME).hex

################################################################################
# Source code files                                                            #
################################################################################

# Add all the application files to the list of directories to scan.
SRC_DIRS = $(SRC_DIR)
SRC_DIRS_DEBUG = $(SRC_DIR)/debug

# Include only the following selected sources from the STM HAL and everything
# from stm/src
stm_hal = \
	stm32l0xx_hal.c \
	stm32l0xx_hal_adc.c \
	stm32l0xx_hal_adc_ex.c \
	stm32l0xx_hal_cortex.c \
	stm32l0xx_hal_dma.c \
	stm32l0xx_hal_flash.c \
	stm32l0xx_hal_flash_ex.c \
	stm32l0xx_hal_gpio.c \
	stm32l0xx_hal_pwr.c \
	stm32l0xx_hal_pwr_ex.c \
	stm32l0xx_hal_rcc.c \
	stm32l0xx_hal_rcc_ex.c \
	stm32l0xx_hal_rtc.c \
	stm32l0xx_hal_rtc_ex.c \
	stm32l0xx_hal_spi.c \
	stm32l0xx_hal_uart.c \
	stm32l0xx_hal_uart_ex.c \
	stm32l0xx_ll_dma.c

stm_hal_debug = \
	stm32l0xx_ll_usart.c \
	stm32l0xx_ll_rcc.c

SRC_FILES += $(patsubst %.c,$(LIB_DIR)/stm/STM32L0xx_HAL_Driver/Src/%.c,$(stm_hal))
SRC_FILES_DEBUG = $(patsubst %.c,$(LIB_DIR)/stm/STM32L0xx_HAL_Driver/Src/%.c,$(stm_hal_debug))

SRC_DIRS += $(LIB_DIR)/stm/src

# Include all source code from rtt and LoRaWAN lib subdirectories
SRC_DIRS_DEBUG += $(LIB_DIR)/rtt
SRC_DIRS += $(LIB_DIR)/LoRaWAN/Utilities

# Include the core LoRa MAC stack with only the base regional files
SRC_DIRS += \
	$(LIB_DIR)/loramac-node/src/peripherals/soft-se \
	$(LIB_DIR)/loramac-node/src/radio/sx1276 \
	$(LIB_DIR)/loramac-node/src/mac
SRC_FILES += \
	$(LIB_DIR)/loramac-node/src/mac/region/Region.c \
	$(LIB_DIR)/loramac-node/src/mac/region/RegionCommon.c

# Activate the regional parameter files explicitly enabled by the developer in
# the variable ENABLED_REGIONS.
SRC_FILES += $(foreach reg,$(ENABLED_REGIONS),$(wildcard $(LIB_DIR)/loramac-node/src/mac/region/*$(reg)*.c))
CFLAGS += $(foreach reg,$(ENABLED_REGIONS),-DREGION_$(reg))

# The US915 regional file depends on RegionBaseUS which will not be matched by
# the wildcard pattern above, so we need to include it explicitly if the region
# is enabled.
ifneq (,$(findstring US,$(ENABLED_REGIONS)))
SRC_FILES += $(LIB_DIR)/loramac-node/src/mac/region/RegionBaseUS.c
endif

################################################################################
# ASM sources                                                                  #
################################################################################

ASM_SOURCES ?= $(LIB_DIR)/stm/src/startup_stm32l072xx.s

################################################################################
# Linker script                                                                #
################################################################################

LINKER_SCRIPT ?= $(CFG_DIR)/STM32L072CZEx_FLASH.ld

################################################################################
# Toolchain                                                                    #
################################################################################

TOOLCHAIN ?= arm-none-eabi-
CC = $(TOOLCHAIN)gcc
GDB = $(TOOLCHAIN)gdb
AS = $(TOOLCHAIN)gcc -x assembler-with-cpp
OBJCOPY = $(TOOLCHAIN)objcopy
SIZE = $(TOOLCHAIN)size

################################################################################
# Was verbose build mode requested?                                            #
################################################################################

ifeq ("$(BUILD_VERBOSE)","1")
Q :=
ECHO = @echo
else
MAKE += -s
Q := @
ECHO = @echo
endif

################################################################################
# Are we runnign a target that builds?                                         #
################################################################################

# A list of make targets for which version and dependency files should not be
# generated and included. That's generally any target that does not build
# firmware.
NOBUILD := clean .clean-obj .clean-out .clean-python flash gdbserver jlink ozone openocd

# We only need to generate dependency files if the make target is empty or if it
# is not one of the targets in NOBUILD
ifeq (,$(MAKECMDGOALS))
building=1
else
ifneq (,$(filter-out $(NOBUILD),$(MAKECMDGOALS)))
building=1
endif
endif

################################################################################
# Version and build date information                                           #
################################################################################

# Only generate version and build time information if we are actually running a
# target that builds something.
ifeq (1,$(building))

build_date := $(shell date "+%Y-%b-%d %H:%M:%S %Z")

# In order to properly re-build the firmware when the version string changes,
# e.g., as a result of a git tag begin added or removed, the output of git
# describe needs to be written to a file other project files can depend on, and
# the generated file only needs to be updated when the version string really
# changes. This is what the shell script below implements. The version string
# generated by git describe is written to a file in $(OBJ_DIR) which is then
# pulled in as a dependency.
#
# If the version tag is of the form "vX.Y.Z*" where X, Y, Z are numbers, strip
# the prefix "v". The prefix is added to the git tag name to make it more likely
# that the resulting string is unique, e.g., does not conflict with branch
# names. Also, it appears to be a convention on Github. However, the prefix "v"
# is not part of the version string and thus should not be included in the
# sources because then AT commands like AT$VER would (incorrectly) return it.

git_describe := git describe --abbrev=8 --always --dirty=' (modified)' 2>/dev/null | \
	sed -Ee 's/^v(([0-9]+\.){2}[0-9]+)/\1/'

tmp := $(shell \
	mkdir -p $(OBJ_DIR); \
	f=$(OBJ_DIR)/version; \
	cur=`$(git_describe)`; \
	[ -z "$$cur" ] && cur=`cat VERSION 2>/dev/null`; \
	[ -r $$f ] && prev=`cat $$f`; \
	[ -n "$$prev" -a "$$prev" = "$$cur" ] && exit 0; \
	echo "$$cur" > $$f)
version := $(strip $(shell cat $(OBJ_DIR)/version))

ifeq (,$(version))
$(error Could not detect firmware version)
endif

tmp := $(shell \
	mkdir -p $(OBJ_DIR); \
	f=$(OBJ_DIR)/lib_version; \
	cur=`(cd lib/loramac-node; $(git_describe))`; \
	[ -z "$$cur" ] && cur=`cat LIB_VERSION 2>/dev/null`; \
	[ -r $$f ] && prev=`cat $$f`; \
	[ -n "$$prev" -a "$$prev" = "$$cur" ] && exit 0; \
	echo "$$cur" > $$f)
lib_version := $(strip $(shell cat $(OBJ_DIR)/lib_version))

ifeq (,$(version))
$(error Could not detect LoRaMac-node version)
endif

endif

################################################################################
# Compiler flags for .c files                                                  #
################################################################################

CFLAGS += -std=c11
CFLAGS += -mcpu=cortex-m0plus
CFLAGS += -mthumb
CFLAGS += -mlittle-endian
CFLAGS += -Wall
CFLAGS += -pedantic
CFLAGS += -Wextra
CFLAGS += -Wmissing-include-dirs
CFLAGS += -Wswitch-default
CFLAGS += -Wno-old-style-declaration
CFLAGS += -ffunction-sections
CFLAGS += -fdata-sections

CFLAGS += -D'__weak=__attribute__((weak))'
CFLAGS += -D'__packed=__attribute__((__packed__))'
CFLAGS += -DSTM32L072xx
CFLAGS += -DHAL_IWDG_MODULE_ENABLED
CFLAGS += -DUSE_FULL_LL_DRIVER

CFLAGS += -DDEFAULT_UART_BAUDRATE=$(DEFAULT_UART_BAUDRATE)

CFLAGS += -DTCXO_PIN=$(TCXO_PIN)  -DMKR1310=$(MKR1310)

# Extra flags to be only applied when we compile the souce files from the lib
# subdirectory. Since that sub-directory contains third-party code, disable some
# of the warnings.
CFLAGS_LIBS += -Wno-unused-parameter
CFLAGS_LIBS += -Wno-switch-default
CFLAGS_LIBS += -Wno-int-conversion

CFLAGS_DEBUG += -g3
CFLAGS_DEBUG += -Og
CFLAGS_DEBUG += -DDEBUG
CFLAGS_DEBUG += -DDEBUG_PORT=$(DEBUG_PORT)

CFLAGS_RELEASE += -Os
CFLAGS_RELEASE += -DRELEASE

CFLAGS += -DSOFT_SE
CFLAGS += -DSECURE_ELEMENT_PRE_PROVISIONED
CFLAGS += -DLORAMAC_CLASSB_ENABLED

CFLAGS += -DENABLED_REGIONS='"$(ENABLED_REGIONS)"'
CFLAGS += -DDEFAULT_ACTIVE_REGION='"$(DEFAULT_ACTIVE_REGION)"'
CFLAGS += -DREGION_AS923_DEFAULT_CHANNEL_PLAN=$(AS923_DEFAULT_CHANNEL_PLAN)
CFLAGS += -DREGION_CN470_DEFAULT_CHANNEL_PLAN=$(CN470_DEFAULT_CHANNEL_PLAN)


ifdef ENABLE_FACTORY_RESET_PIN
CFLAGS += -DENABLE_FACTORY_RESET_PIN
endif

ifneq (,$(LORAMAC_ABP_VERSION))
CFLAGS += -DLORAMAC_ABP_VERSION=$(LORAMAC_ABP_VERSION)
endif

ifdef RESTORE_CHMASK_AFTER_JOIN
CFLAGS += -DRESTORE_CHMASK_AFTER_JOIN
endif

################################################################################
# Compiler flags for .s files                                                  #
################################################################################

ASFLAGS += -mcpu=cortex-m0plus
ASFLAGS += --specs=nano.specs
ASFLAGS += -mfloat-abi=soft
ASFLAGS += -mthumb
ASFLAGS += -mlittle-endian

ASFLAGS_DEBUG += -g3
ASFLAGS_DEBUG += -Og

ASFLAGS_RELEASE += -Os

################################################################################
# Linker flags                                                                 #
################################################################################

LDFLAGS += -mcpu=cortex-m0plus
LDFLAGS += -mthumb
LDFLAGS += -mlittle-endian
LDFLAGS += -T$(LINKER_SCRIPT)
LDFLAGS += -Wl,-lc
LDFLAGS += -Wl,-lm
LDFLAGS += -static
LDFLAGS += -Wl,-Map=$(MAP)
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,--print-memory-usage
LDFLAGS += -Wl,-u,__errno
LDFLAGS += --specs=nosys.specs

################################################################################
# Create a list of object files and their dependencies                         #
################################################################################

SRC_FILES += $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
SRC_FILES_DEBUG += $(foreach dir,$(SRC_DIRS_DEBUG),$(wildcard $(dir)/*.c))

OBJ_C = $(SRC_FILES:%.c=$(OBJ_DIR)/$(TYPE)/%.o)
OBJ_S = $(ASM_SOURCES:%.s=$(OBJ_DIR)/$(TYPE)/%.o)
OBJ = $(OBJ_C) $(OBJ_S)
DEP = $(OBJ:%.o=%.d)

################################################################################
# Build targets                                                                #
################################################################################

.PHONY: debug
debug: export TYPE=debug
debug: export CFLAGS=$(CFLAGS_DEBUG)
debug: export ASFLAGS=$(ASFLAGS_DEBUG)
debug: export SRC_FILES=$(SRC_FILES_DEBUG)
debug:
	$(Q)$(MAKE) install

.PHONY: release
release: export TYPE=release
release: export CFLAGS=$(CFLAGS_RELEASE)
release: export ASFLAGS=$(ASFLAGS_RELEASE)
release:
	$(Q)$(MAKE) install

.PHONY: install
install: $(BIN) $(HEX) $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Copying $(BIN) to ./$(BASENAME).bin..."
	$(Q)cp -f "$(BIN)" "$(BASENAME).bin"
	$(Q)$(ECHO) "Copying $(HEX) to ./$(BASENAME).hex..."
	$(Q)cp -f "$(HEX)" "$(BASENAME).hex"

.PHONY: python
python: $(MAKEFILE_LIST) python/VERSION
	cd python && python -m build

.PHONY: python/VERSION
python/VERSION: $(MAKEFILE_LIST)
	git describe | sed -e 's/.*\([0-9]\+\.[0-9]\+\.[0-9]\+\).*/\1/g' > $@

$(BIN): $(ELF) $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Creating $(BIN) from $(ELF)..."
	$(Q)$(OBJCOPY) -O binary "$(ELF)" "$(BIN)"

$(HEX): $(ELF) $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Creating $(HEX) from $(ELF)..."
	$(Q)$(OBJCOPY) -S -O ihex "$(ELF)" "$(HEX)"

$(ELF): $(OBJ) $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Linking object files into $(ELF)..."
	$(Q)mkdir -p "$(OUT_DIR)/$(TYPE)"
	$(Q)$(CC) $(LDFLAGS) $(OBJ) -o "$(ELF)"
	$(Q)$(MAKE) size

.PHONY: size
size: $(ELF) $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Size of sections:"
	$(Q)$(SIZE) "$(ELF)"

define compile
$(Q)$(ECHO) "Compiling: $<"
$(Q)mkdir -p "$(@D)"
$(Q)$(CC) -MD -MP -MT "$@ $(@:.o=.d)" -c $(CFLAGS) $(1) -isystem $(LIB_DIR) $< -o $@
endef

$(OBJ_DIR)/$(TYPE)/src/%.o: src/%.c $(MAKEFILE_LIST)
	$(call compile,\
		-I $(SRC_DIR) \
		-I $(SRC_DIR)/debug \
		-I $(CFG_DIR) \
		-isystem $(LIB_DIR)/loramac-node/src/mac \
		-isystem $(LIB_DIR)/loramac-node/src/mac/region \
		-isystem $(LIB_DIR)/loramac-node/src/radio \
		-isystem $(LIB_DIR)/LoRaWAN/Utilities \
		-isystem $(LIB_DIR)/stm/STM32L0xx_HAL_Driver/Inc \
		-isystem $(LIB_DIR)/stm/include \
	)

$(OBJ_DIR)/$(TYPE)/lib/LoRaWAN/%.o: lib/LoRaWAN/%.c $(MAKEFILE_LIST)
	$(call compile,\
		-I $(SRC_DIR) \
		-I $(CFG_DIR) \
		-isystem $(LIB_DIR)/stm/STM32L0xx_HAL_Driver/Inc \
		-isystem $(LIB_DIR)/stm/include \
	)

# Specialized targets for src/main.c and src/cmd.c. These two files depend on
# the version strings generated from the git repository and need a couple of
# extra CFLAGS.

$(OBJ_DIR)/$(TYPE)/src/cmd.o: CFLAGS+=-DVERSION='"$(version)"'
$(OBJ_DIR)/$(TYPE)/src/cmd.o: CFLAGS+=-DVERSION_COMPAT='"$(VERSION_COMPAT)"'
$(OBJ_DIR)/$(TYPE)/src/cmd.o: CFLAGS+=-DLIB_VERSION='"$(lib_version)"'
$(OBJ_DIR)/$(TYPE)/src/cmd.o: CFLAGS+=-DBUILD_DATE='"$(build_date)"'
$(OBJ_DIR)/$(TYPE)/src/cmd.o: CFLAGS+=-DBUILD_DATE_COMPAT='"$(BUILD_DATE_COMPAT)"'
$(OBJ_DIR)/$(TYPE)/src/cmd.o: $(MAKEFILE_LIST) $(OBJ_DIR)/version $(OBJ_DIR)/lib_version

$(OBJ_DIR)/$(TYPE)/src/main.o: CFLAGS+=-DVERSION='"$(version)"'
$(OBJ_DIR)/$(TYPE)/src/main.o: CFLAGS+=-DLIB_VERSION='"$(lib_version)"'
$(OBJ_DIR)/$(TYPE)/src/main.o: CFLAGS+=-DBUILD_DATE='"$(build_date)"'
$(OBJ_DIR)/$(TYPE)/src/main.o: $(MAKEFILE_LIST) $(OBJ_DIR)/version $(OBJ_DIR)/lib_version

$(OBJ_DIR)/$(TYPE)/lib/stm/%.o: lib/stm/%.c $(MAKEFILE_LIST)
	$(call compile,\
		-Wno-unused-parameter \
		-I $(LIB_DIR)/stm/STM32L0xx_HAL_Driver/Inc \
		-I $(LIB_DIR)/stm/include \
		-isystem $(CFG_DIR) \
	)

$(OBJ_DIR)/$(TYPE)/lib/rtt/%.o: lib/rtt/%.c $(MAKEFILE_LIST)
	$(call compile)

$(OBJ_DIR)/$(TYPE)/lib/loramac-node/%.o: lib/loramac-node/%.c $(MAKEFILE_LIST)
	$(call compile,\
		-Wno-int-conversion \
		-Wno-unused-parameter \
		-Wno-switch-default \
		-I $(LIB_DIR)/loramac-node/src/mac \
		-I $(LIB_DIR)/loramac-node/src/radio \
		-I $(LIB_DIR)/loramac-node/src/mac/region \
		-isystem $(LIB_DIR)/LoRaWAN/Utilities \
		-isystem $(LIB_DIR)/stm/STM32L0xx_HAL_Driver/Inc \
		-isystem $(LIB_DIR)/stm/include \
		-isystem $(SRC_DIR) \
		-isystem $(CFG_DIR) \
	)

$(OBJ_DIR)/$(TYPE)/cfg/%.o: cfg/%.c $(MAKEFILE_LIST)
	$(call compile,-isystem $(LIB_DIR)/stm/STM32L0xx_HAL_Driver/Inc)

$(OBJ_DIR)/$(TYPE)/%.o: %.s $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Compiling: $<"
	$(Q)mkdir -p $(@D)
	$(Q)$(CC) -c $(ASFLAGS) $< -o $@

VERSION: $(MAKEFILE_LIST) $(OBJ_DIR)/version
	$(Q)cp $(OBJ_DIR)/version VERSION

LIB_VERSION: $(MAKEFILE_LIST) $(OBJ_DIR)/lib_version
	$(Q)cp $(OBJ_DIR)/lib_version LIB_VERSION

################################################################################
# Clean targets                                                                #
################################################################################

.PHONY: clean
clean: $(MAKEFILE_LIST)
	$(Q)$(MAKE) .clean-obj
	$(Q)$(MAKE) .clean-out
	$(Q)$(MAKE) .clean-python

.PHONY: .clean-obj
.clean-obj: $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Deleting object files..."
	$(Q)rm -rf "$(OBJ_DIR)"

.PHONY: .clean-out
.clean-out: $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Deleting output files..."
	$(Q)rm -rf "$(OUT_DIR)"

.PHONY: .clean-python
.clean-python: $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Deleting Python build artifacts..."
	$(Q)rm -rf "python/VERSION"
	$(Q)rm -rf "python/build"
	$(Q)rm -rf "python/dist"
	$(Q)rm -rf "python/lora_modem_abz.egg-info"
	$(Q)rm -rf "python/__pycache__"
	$(Q)rm -rf "python/.mypy_cache"

################################################################################
# Debugging targets                                                            #
################################################################################

.PHONY: flash
flash: $(MAKEFILE_LIST)
ifeq ($(OS),Windows_NT)
	JLink -device stm32l072cz -CommanderScript tools/jlink/flash.jlink
else
	JLinkExe -device stm32l072cz -CommanderScript tools/jlink/flash.jlink
endif

.PHONY: gdbserver
gdbserver: $(MAKEFILE_LIST)
ifeq ($(OS),Windows_NT)
	JLinkGDBServerCL -singlerun -device stm32l072cz -if swd -speed 4000 -localhostonly -reset
else
	JLinkGDBServer -singlerun -device stm32l072cz -if swd -speed 4000 -localhostonly -reset
endif

.PHONY: jlink
jlink: $(MAKEFILE_LIST)
	$(Q)$(MAKE) jlink-flash
	$(Q)$(MAKE) jlink-gdbserver

.PHONY: ozone
ozone: debug $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Launching Ozone debugger..."
	$(Q)Ozone tools/ozone/ozone.jdebug

.PHONY: openocd
openocd: $(MAKEFILE_LIST)
	$(Q)$(ECHO) "Launching OpenOCD..."
	$(Q)openocd -f interface/stlink.cfg -c "transport select hla_swd" \
		-f target/stm32l0_dual_bank.cfg

################################################################################
# Initialize git submodules                                                    #
################################################################################

$(LIB_DIR)/loramac-node/LICENSE:
	@git submodule update --init lib/loramac-node

################################################################################
# Include dependencies                                                         #
################################################################################

ifeq (1,$(building))
-include $(DEP)
endif
