/* Optional, control-thread audio asset ingestion for pcm-mixer.
 *
 * This API is intentionally separate from the dependency-free realtime mixer.
 * It uses libsndfile for WAV/OGG decoding and FluidSynth for bounded SF2
 * rendering. SFZ files are resolved to one matching region and then decoded
 * through the same libsndfile path. All returned audio is owned, signed
 * 16-bit mono PCM at the requested rate and is suitable for pcmmix_sample.
 */
#ifndef PCMMIX_ASSET_H
#define PCMMIX_ASSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pcmmix_asset_format {
    PCMMIX_ASSET_UNKNOWN = 0,
    PCMMIX_ASSET_WAV,
    PCMMIX_ASSET_OGG,
    PCMMIX_ASSET_SFZ,
    PCMMIX_ASSET_SF2
} pcmmix_asset_format;

typedef struct pcmmix_asset_options {
    uint32_t target_rate;       /* 8000..192000; default 44100 */
    uint32_t max_seconds;       /* 1..60; default 30 */
    uint16_t sf2_bank;          /* default 0 */
    uint16_t sf2_hold_ms;       /* 1..max_seconds; default 1200 */
    uint16_t sf2_release_ms;    /* 0..max_seconds; default 500 */
    uint8_t note;               /* SFZ/SF2 selection and root; default 60 */
    uint8_t velocity;           /* SFZ/SF2 selection; default 100 */
    uint8_t sf2_program;        /* 0..127; default 0 */
    uint8_t sf2_channel;        /* 0..15; default 0 */
} pcmmix_asset_options;

/* Fill options with bounded defaults suitable for one-shot instruments. */
void pcmmix_asset_options_init(pcmmix_asset_options *options);

/* Detect WAV, OGG, SFZ, or SF2 from magic and a conservative extension
 * fallback. UNKNOWN is returned for missing, unreadable, or unsupported data. */
pcmmix_asset_format pcmmix_asset_detect(const char *path);
const char *pcmmix_asset_format_name(pcmmix_asset_format format);

/* Load one playable asset. For SFZ, the first region matching note and
 * velocity is selected; key/lokey/hikey/lovel/hivel/pitch_keycenter,
 * default_path, and relative WAV/OGG sample paths are supported. For SF2,
 * bank/program/channel/note select a preset that is rendered for the bounded
 * hold and release duration. File access, allocation, decoding, and synthesis
 * make this a control-thread API: never call it from an audio callback.
 *
 * Returns a malloc'd mono buffer, released by pcmmix_asset_free(). On success
 * out_frames and out_root_note are populated. On failure they are cleared and
 * a concise diagnostic is written to err when supplied. */
int16_t *pcmmix_asset_load(const char *path,
                           const pcmmix_asset_options *options,
                           size_t *out_frames, uint8_t *out_root_note,
                           char *err, size_t err_len);

void pcmmix_asset_free(int16_t *frames);

#ifdef __cplusplus
}
#endif

#endif /* PCMMIX_ASSET_H */
