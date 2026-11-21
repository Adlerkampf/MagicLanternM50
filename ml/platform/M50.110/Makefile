MODEL = M50
FW_VERSION = 110

TOP_DIR = ../..

AUTOEXEC_BASE = 0x800000
FIR_BASE = 0x800120

#CANON_FIR  = CCF19110.FIR
#UPDATE_NAME_FIR = D_M50110.FIR
FIRMWARE_ID     = 0x00000412
MAIN_FIRMWARE_ADDR = 0xE0040000

# user_mem_start from cstart struct. Align to 0x10 up
RESTARTSTART = 0xdc870

# Cortex A9, binaries loaded as Thumb
CFLAGS_PLATFORM = -mthumb -mlong-calls
PLATFORM_ARCH = armv7-a

ML_BOOT_OBJ = boot-d678.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/function_overrides.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/raw_vsync.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/bin_stub.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/sd_speed.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/crx_rec.o
ML_SRC_EXTRA_OBJS += $(BUILD_DIR)/sensor_crop.o

# RAW video: compile-only check with dummy addresses (refuses to run RAW).
#   make M50_RAW_PLACEHOLDERS=1
ifeq ($(M50_RAW_PLACEHOLDERS),1)
CFLAGS_PLATFORM += -DCONFIG_RAW_PLACEHOLDER_ADDRS
endif
# Real values found on your camera can be passed without editing files, e.g.
#   make M50_RAW_DEFS="-DM50_RAW_LV_EDMAC_CHANNEL_ADDR=0xD00xxxxx"
CFLAGS_PLATFORM += $(M50_RAW_DEFS)

include ../Makefile
.DEFAULT_GOAL := $(BUILD_DIR)/cam_complete
