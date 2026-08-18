#ifndef PCMMIX_BANK_H
#define PCMMIX_BANK_H

/* Bounded, ownership-aware sound cue and variant registry for pcm-mixer. */

#include "pcm_mixer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCMMIX_BANK_VERSION_MAJOR 0
#define PCMMIX_BANK_VERSION_MINOR 2
#define PCMMIX_BANK_VERSION_PATCH 0

#if PCMMIX_VERSION_MAJOR == 0 && PCMMIX_VERSION_MINOR < 2
#error "pcmmix-bank 0.2 requires pcm-mixer 0.2 or newer"
#endif

#define PCMMIX_BANK_CUES_MAX 64u
#define PCMMIX_BANK_VARIANTS_MAX 8u

typedef struct pcmmix_bank_variant {
    pcmmix_sample sample;
    int16_t *owned_frames;
    float gain;
    float pitch;
    bool occupied;
} pcmmix_bank_variant;

typedef struct pcmmix_bank_cue {
    pcmmix_bank_variant variants[PCMMIX_BANK_VARIANTS_MAX];
    uint8_t variant_count;
    uint8_t last_variant;
} pcmmix_bank_cue;

/* Public for allocation without malloc; treat fields as private. */
typedef struct pcmmix_bank {
    pcmmix_bank_cue cues[PCMMIX_BANK_CUES_MAX];
    uint32_t random_state;
    uint32_t cue_count;
} pcmmix_bank;

/* cue_count must be 1..PCMMIX_BANK_CUES_MAX. Zero selects the maximum. */
bool pcmmix_bank_init(pcmmix_bank *bank, uint32_t cue_count, uint32_t seed);

/* Free every owned sample. Stop the mixer before clearing or replacing data. */
void pcmmix_bank_clear(pcmmix_bank *bank);
void pcmmix_bank_clear_cue(pcmmix_bank *bank, uint32_t cue);

/* Register caller-owned PCM. The memory must outlive the bank and mixer. */
bool pcmmix_bank_set_borrowed(pcmmix_bank *bank, uint32_t cue,
                              uint32_t variant, const pcmmix_sample *sample,
                              float gain, float pitch);

/* Transfer a malloc-compatible frame buffer to the bank on success. A buffer
 * already referenced by another slot in this bank is rejected; taking or
 * re-taking it into the same slot safely transfers ownership and updates that
 * slot's metadata. */
bool pcmmix_bank_take(pcmmix_bank *bank, uint32_t cue, uint32_t variant,
                      int16_t *frames, size_t frame_count, float gain,
                      float pitch);

/* Load one strict pcm-mixer WAV and install it as an owned variant. */
bool pcmmix_bank_load_wav(pcmmix_bank *bank, uint32_t cue,
                          uint32_t variant, const char *path, float gain,
                          float pitch, char *error, size_t error_size);

/* Transactional cue load: all paths replace the cue, or none do. */
bool pcmmix_bank_load_variants(pcmmix_bank *bank, uint32_t cue,
                               const char *const *paths, size_t path_count,
                               float gain, float pitch, char *error,
                               size_t error_size);

size_t pcmmix_bank_variant_count(const pcmmix_bank *bank, uint32_t cue);
const pcmmix_sample *pcmmix_bank_sample_at(const pcmmix_bank *bank,
                                           uint32_t cue, uint32_t variant);

/* Select an occupied variant, avoiding an immediate repeat when possible. */
int pcmmix_bank_choose(pcmmix_bank *bank, uint32_t cue);

/* Apply per-variant gain/pitch and return a normal pcm-mixer voice handle. */
int pcmmix_bank_play(pcmmix *mixer, pcmmix_bank *bank, uint32_t cue,
                     float gain, float pitch);
int pcmmix_bank_play_pan(pcmmix *mixer, pcmmix_bank *bank, uint32_t cue,
                         float gain, float pitch, float pan);
int pcmmix_bank_loop(pcmmix *mixer, pcmmix_bank *bank, uint32_t cue,
                     float gain, float pitch);
int pcmmix_bank_loop_pan(pcmmix *mixer, pcmmix_bank *bank, uint32_t cue,
                         float gain, float pitch, float pan);

#ifdef __cplusplus
}
#endif

#endif
