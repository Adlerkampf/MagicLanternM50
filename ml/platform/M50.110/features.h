#define FEATURE_VRAM_RGBA

// Don't Click Me menu looks to be intended as a place
// for devs to put custom code in debug.c run_test(),
// and allowing triggering from a menu context.
#define FEATURE_DONT_CLICK_ME

#define FEATURE_SHOW_SHUTTER_COUNT

// working but incomplete, some allocators don't report
// anything yet as they're faked / not yet found
#define FEATURE_SHOW_FREE_MEMORY

#define CONFIG_TSKMON
#define FEATURE_SHOW_TASKS
#define FEATURE_SHOW_CPU_USAGE
#define FEATURE_SHOW_GUI_EVENTS

#define CONFIG_ADDITIONAL_VERSION
#define FEATURE_SCREENSHOT

// enable global draw
#define FEATURE_GLOBAL_DRAW
#define FEATURE_CROPMARKS

// Enable remapping ROM pages to RAM
#define CONFIG_SGI_HANDLERS
#define CONFIG_MMU_REMAP
#define CONFIG_MMU_REMAP_PAGES 2

//~kitor: add bootdisk menu for easier testing on foreign camera :)
#define FEATURE_BOOTFLAG_MENU

// We can't yet rely on image capture.  Cam crashes due to null pointer,
// I think?  If it fails to AF lock, for example.
#define CONFIG_IMAGE_CAPTURE_NOT_WORKING

#define CONFIG_CRASH_LOG
#define CONFIG_AUTOBACKUP_ROM

#undef CONFIG_PROP_REQUEST_CHANGE

// ---- RAW video (experimental, see raw_config.h and RAW_VIDEO.md) ----
// Needed by mlv_lite. All camera-specific unknowns live in raw_config.h.
#define CONFIG_RAW_LIVEVIEW
#define FEATURE_PICSTYLE

// The stock port can't hook EVF_STATE, so frames are detected by polling.
#define CONFIG_M50_POLLED_VSYNC

// Hook Canon's EDMAC set-address (0xE054A9AE). For port 3 (LiveView RAW)
// it gives the exact per-frame signal and lets CRX / mlv_lite redirect frames
// into their own buffers (zero copy).
#define CONFIG_M50_EDMAC_HOOK

// SD UHS-I 195 MHz clock tuning (sd_speed.c)
#ifndef M50_NO_SD195
#define CONFIG_M50_SD195
#endif
