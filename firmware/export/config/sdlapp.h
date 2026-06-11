/*
 * This config file is for the SDL application
 */

/* We don't run on hardware directly */
#define CONFIG_PLATFORM (PLATFORM_HOSTED|PLATFORM_SDL)
#define HAVE_FPU

/* For Rolo and boot loader */
#define MODEL_NUMBER 100

#define MODEL_NAME   "Rockbox"

#define USB_NONE




/* define this if you have a colour LCD */
#define HAVE_LCD_COLOR

/* define this if you want album art for this target */
#define HAVE_ALBUMART

/* define this to enable bitmap scaling */
#define HAVE_BMP_SCALING

/* define this to enable JPEG decoding */
#define HAVE_JPEG

/* define this if you have access to the quickscreen */
#define HAVE_QUICKSCREEN

/* define this if you would like tagcache to build on this target */
#define HAVE_TAGCACHE

/* LCD dimensions
 *
 * overriden by configure for application builds */
#ifndef LCD_WIDTH
#define LCD_WIDTH  320
#endif

#ifndef LCD_HEIGHT
#define LCD_HEIGHT 480
#endif

#define LCD_DEPTH  24
#define LCD_PIXELFORMAT RGB888

/* define this to indicate your device's keypad */
#define HAVE_TOUCHSCREEN
#define HAVE_BUTTON_DATA

/* define this if you have a real-time clock */
#define CONFIG_RTC RTC_HOSTED

/* The number of bytes reserved for loadable codecs */
#define CODEC_SIZE 0x100000

/* The number of bytes reserved for loadable plugins */
#define PLUGIN_BUFFER_SIZE 0x80000
/* m1–m9 strip block: shared with all Waveform targets */
#include "waveform_strip.h"

/* Waveform OS post-m9 Stage 1 instrumentation. Engineering builds only.
 * Production builds: comment out the WAVEFORM_TELEMETRY line — every hook
 * is #ifdef-gated and falls through to zero-cost.
 * __PCTOOL__ guard (S7-D4r): host tools (warble) compile firmware files
 * like buflib_mempool.c against this config but do not link
 * wf_telemetry.c — telemetry must stay off there or the link fails. */
#ifndef __PCTOOL__
#define WAVEFORM_TELEMETRY
#endif
#ifdef WAVEFORM_TELEMETRY
#define WAVEFORM_TELEMETRY_BUFLIB
#define WAVEFORM_TELEMETRY_PCMBUF
#define WAVEFORM_TELEMETRY_CODEC
#define WAVEFORM_TELEMETRY_STORAGE
#define WAVEFORM_TELEMETRY_STACK
#define WAVEFORM_TELEMETRY_DISPLAY
#endif

#define AB_REPEAT_ENABLE




#define HAVE_SCROLLWHEEL
#define CONFIG_KEYPAD SDL_PAD

/* Use SDL audio/pcm in a SDL app build */
#define HAVE_SDL
#define HAVE_SDL_AUDIO

#define HAVE_SW_TONE_CONTROLS 

/* Define this to the CPU frequency */
/*
#define CPU_FREQ 48000000
*/

#define CONFIG_LCD LCD_COWOND2

/* Define this if a programmable hotkey is mapped */
#define HAVE_HOTKEY

#define BOOTDIR "/.rockbox"

/* No special storage */
#define CONFIG_STORAGE STORAGE_HOSTFS
#define HAVE_STORAGE_FLUSH

/* Override CODECS_DIR (default ROCKBOX_LIBRARY_PATH "/rockbox/codecs" =
 * "/usr/local/lib/rockbox/codecs") so handle_special_dirs maps it to
 * "$HOME/.config/rockbox.org/codecs". Required because the sim runs from
 * its build tree, not from a system-wide install. */
#define CODECS_DIR ROCKBOX_DIR "/codecs"
