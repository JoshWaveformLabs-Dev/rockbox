/*
 * Waveform OS — shared strip header
 *
 * Single source of truth for the m1–m9 strip block.
 * Consumed by every Waveform target config (sdlapp.h, ipodvideo.h, ...).
 *
 * MUST be included AFTER device-shape defines so the #undefs land last.
 *
 * No #ifndef guard: these are intentional unconditional un/defines.
 * Including twice is a no-op.
 *
 * Telemetry (WAVEFORM_TELEMETRY) is NOT here — telemetry is per-config
 * (engineering vs production) and lives in the consumer file.
 */

#undef HAVE_ROCKBOX_PLUGINS    /* m1: all plugins stripped */
#define HAVE_TEST_PLUGINS      /* D5: engineering test suite (keep permanently) */
#undef HAVE_VOICE_THREAD       /* m2: voice/talk UI stripped */
#undef HAVE_TAGCACHE           /* m4: tagcache/database stripped */
#undef HAVE_RECORDING          /* m6: recording stripped */
#undef HAVE_FMRADIO            /* m7: FM radio stripped (sdlapp gate) */
#undef CONFIG_TUNER            /* m7: FM radio stripped (hardware gate; config.h:752 falls back to 0) */
#undef HAVE_RDS_CAP            /* m7: RDS stripped with the tuner */
#undef CONFIG_RDS              /* m7: RDS stripped with the tuner */
#define WAVEFORM_NO_CROSSFADE  /* m8: crossfade stripped (consumed by config.h:1082) */
#define WAVEFORM_STRIP_CODECS  /* m9: codec set trimmed to MP3/AAC/FLAC/ALAC/Opus */
#define WAVEFORM_WFLIB         /* stage 3: compact library index reader */
