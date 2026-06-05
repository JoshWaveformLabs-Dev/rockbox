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

/*
 * Plugin gating — Stage 6.3.
 *   Production:  HAVE_ROCKBOX_PLUGINS undef'd (m1 strip).
 *   Engineering: pass -DWAVEFORM_ENGINEERING_BUILD via EXTRA_DEFINES
 *                AND set HAVE_ROCKBOX_PLUGINS=1 as a Make var (tools/root.make
 *                gates plugins.make on the Make-side var, not the C define).
 * Engineering builds enable HAVE_ROCKBOX_PLUGINS so test_codec / test_disk
 * / test_mem are reachable on real hardware. HAVE_TEST_PLUGINS stays
 * permanently defined so the .c files compile in either mode.
 */
#ifdef WAVEFORM_ENGINEERING_BUILD
#define HAVE_ROCKBOX_PLUGINS   /* engineering build: test_codec / test_disk / test_mem on */
#else
#undef HAVE_ROCKBOX_PLUGINS    /* m1: all plugins stripped (production invariant) */
#endif
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
