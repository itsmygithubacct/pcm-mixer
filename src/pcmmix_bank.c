#include "pcmmix_bank.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
static void set_error(char *error, size_t error_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#endif

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0u) return;
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static bool cue_valid(const pcmmix_bank *bank, uint32_t cue)
{
    return bank != NULL && cue < bank->cue_count &&
           bank->cue_count <= PCMMIX_BANK_CUES_MAX;
}

static bool slot_valid(const pcmmix_bank *bank, uint32_t cue,
                       uint32_t variant)
{
    return cue_valid(bank, cue) && variant < PCMMIX_BANK_VARIANTS_MAX;
}

static float normalized_gain(float gain)
{
    return isnan(gain) || gain < 0.0f ? 0.0f : gain;
}

static float normalized_pitch(float pitch)
{
    return !isnan(pitch) && pitch > 0.0f ? pitch : 1.0f;
}

static void release_variant(pcmmix_bank_variant *variant)
{
    if (variant == NULL) return;
    pcmmix_wav_free(variant->owned_frames);
    *variant = (pcmmix_bank_variant){0};
}

static void refresh_cue(pcmmix_bank_cue *cue)
{
    size_t index;
    uint8_t count = 0u;

    for (index = 0u; index < PCMMIX_BANK_VARIANTS_MAX; ++index) {
        if (cue->variants[index].occupied) count = (uint8_t)(index + 1u);
    }
    cue->variant_count = count;
    if (cue->last_variant >= count ||
        (count != 0u && !cue->variants[cue->last_variant].occupied))
        cue->last_variant = UINT8_MAX;
}

bool pcmmix_bank_init(pcmmix_bank *bank, uint32_t cue_count, uint32_t seed)
{
    if (bank == NULL || cue_count > PCMMIX_BANK_CUES_MAX) return false;
    (void)memset(bank, 0, sizeof *bank);
    bank->cue_count = cue_count == 0u ? PCMMIX_BANK_CUES_MAX : cue_count;
    bank->random_state = seed != 0u ? seed : 0x6d2b79f5u;
    for (uint32_t cue = 0u; cue < bank->cue_count; ++cue)
        bank->cues[cue].last_variant = UINT8_MAX;
    return true;
}

void pcmmix_bank_clear_cue(pcmmix_bank *bank, uint32_t cue)
{
    pcmmix_bank_cue *selected;

    if (!cue_valid(bank, cue)) return;
    selected = &bank->cues[cue];
    for (size_t variant = 0u; variant < PCMMIX_BANK_VARIANTS_MAX; ++variant)
        release_variant(&selected->variants[variant]);
    selected->variant_count = 0u;
    selected->last_variant = UINT8_MAX;
}

void pcmmix_bank_clear(pcmmix_bank *bank)
{
    uint32_t cue_count;

    if (bank == NULL) return;
    cue_count = bank->cue_count <= PCMMIX_BANK_CUES_MAX ?
                bank->cue_count : PCMMIX_BANK_CUES_MAX;
    for (uint32_t cue = 0u; cue < cue_count; ++cue)
        pcmmix_bank_clear_cue(bank, cue);
    bank->cue_count = 0u;
    bank->random_state = 0u;
}

static bool set_variant(pcmmix_bank *bank, uint32_t cue, uint32_t variant,
                        const pcmmix_sample *sample, int16_t *owned_frames,
                        float gain, float pitch)
{
    pcmmix_bank_variant replacement;

    if (!slot_valid(bank, cue, variant) || sample == NULL ||
        sample->frames == NULL || sample->frame_count == 0u) return false;
    replacement = (pcmmix_bank_variant){
        .sample = *sample,
        .owned_frames = owned_frames,
        .gain = normalized_gain(gain),
        .pitch = normalized_pitch(pitch),
        .occupied = true
    };
    release_variant(&bank->cues[cue].variants[variant]);
    bank->cues[cue].variants[variant] = replacement;
    refresh_cue(&bank->cues[cue]);
    return true;
}

static pcmmix_bank_variant *find_owner(pcmmix_bank *bank,
                                       const int16_t *frames)
{
    for (uint32_t cue = 0u; cue < bank->cue_count; ++cue) {
        for (uint32_t variant = 0u; variant < PCMMIX_BANK_VARIANTS_MAX;
             ++variant) {
            pcmmix_bank_variant *candidate =
                &bank->cues[cue].variants[variant];

            if (candidate->occupied && candidate->owned_frames == frames)
                return candidate;
        }
    }
    return NULL;
}

static bool referenced_by_other_slot(const pcmmix_bank *bank,
                                     const pcmmix_bank_variant *target,
                                     const int16_t *frames)
{
    for (uint32_t cue = 0u; cue < bank->cue_count; ++cue) {
        for (uint32_t variant = 0u; variant < PCMMIX_BANK_VARIANTS_MAX;
             ++variant) {
            const pcmmix_bank_variant *candidate =
                &bank->cues[cue].variants[variant];

            if (candidate != target && candidate->occupied &&
                candidate->sample.frames == frames)
                return true;
        }
    }
    return false;
}

bool pcmmix_bank_set_borrowed(pcmmix_bank *bank, uint32_t cue,
                              uint32_t variant, const pcmmix_sample *sample,
                              float gain, float pitch)
{
    if (!slot_valid(bank, cue, variant) || sample == NULL ||
        sample->frames == NULL || sample->frame_count == 0u ||
        find_owner(bank, sample->frames) != NULL)
        return false;
    return set_variant(bank, cue, variant, sample, NULL, gain, pitch);
}

bool pcmmix_bank_take(pcmmix_bank *bank, uint32_t cue, uint32_t variant,
                      int16_t *frames, size_t frame_count, float gain,
                      float pitch)
{
    const pcmmix_sample sample = {frames, frame_count};
    pcmmix_bank_variant *owner;
    pcmmix_bank_variant *target;

    if (!slot_valid(bank, cue, variant) || frames == NULL || frame_count == 0u)
        return false;
    target = &bank->cues[cue].variants[variant];
    if (referenced_by_other_slot(bank, target, frames)) return false;
    owner = find_owner(bank, frames);
    if (owner != NULL) {
        if (owner != target) return false;
        target->sample = sample;
        target->gain = normalized_gain(gain);
        target->pitch = normalized_pitch(pitch);
        target->occupied = true;
        refresh_cue(&bank->cues[cue]);
        return true;
    }
    if (target->occupied && target->sample.frames == frames) {
        target->sample = sample;
        target->owned_frames = frames;
        target->gain = normalized_gain(gain);
        target->pitch = normalized_pitch(pitch);
        refresh_cue(&bank->cues[cue]);
        return true;
    }

    return set_variant(bank, cue, variant, &sample, frames, gain, pitch);
}

bool pcmmix_bank_load_wav(pcmmix_bank *bank, uint32_t cue,
                          uint32_t variant, const char *path, float gain,
                          float pitch, char *error, size_t error_size)
{
    int16_t *frames;
    size_t frame_count = 0u;

    if (error != NULL && error_size != 0u) error[0] = '\0';
    if (!slot_valid(bank, cue, variant) || path == NULL || path[0] == '\0') {
        set_error(error, error_size, "invalid bank slot or WAV path");
        return false;
    }
    frames = pcmmix_wav_load(path, &frame_count, error, error_size);
    if (frames == NULL) return false;
    if (!pcmmix_bank_take(bank, cue, variant, frames, frame_count,
                          gain, pitch)) {
        pcmmix_wav_free(frames);
        set_error(error, error_size, "could not install WAV in bank");
        return false;
    }
    return true;
}

bool pcmmix_bank_load_variants(pcmmix_bank *bank, uint32_t cue,
                               const char *const *paths, size_t path_count,
                               float gain, float pitch, char *error,
                               size_t error_size)
{
    pcmmix_bank_variant loaded[PCMMIX_BANK_VARIANTS_MAX] = {0};
    size_t index;

    if (error != NULL && error_size != 0u) error[0] = '\0';
    if (!cue_valid(bank, cue) || paths == NULL || path_count == 0u ||
        path_count > PCMMIX_BANK_VARIANTS_MAX) {
        set_error(error, error_size, "invalid cue or variant count");
        return false;
    }
    for (index = 0u; index < path_count; ++index) {
        size_t frame_count = 0u;
        char wav_error[256] = "";
        int16_t *frames;

        if (paths[index] == NULL || paths[index][0] == '\0') {
            set_error(error, error_size, "variant %zu has no WAV path", index);
            break;
        }
        frames = pcmmix_wav_load(paths[index], &frame_count, wav_error,
                                 sizeof wav_error);
        if (frames == NULL) {
            set_error(error, error_size, "variant %zu: %s", index, wav_error);
            break;
        }
        loaded[index] = (pcmmix_bank_variant){
            .sample = {frames, frame_count},
            .owned_frames = frames,
            .gain = normalized_gain(gain),
            .pitch = normalized_pitch(pitch),
            .occupied = true
        };
    }
    if (index != path_count) {
        for (size_t cleanup = 0u; cleanup < path_count; ++cleanup)
            release_variant(&loaded[cleanup]);
        return false;
    }

    pcmmix_bank_clear_cue(bank, cue);
    for (index = 0u; index < path_count; ++index)
        bank->cues[cue].variants[index] = loaded[index];
    bank->cues[cue].variant_count = (uint8_t)path_count;
    bank->cues[cue].last_variant = UINT8_MAX;
    return true;
}

size_t pcmmix_bank_variant_count(const pcmmix_bank *bank, uint32_t cue)
{
    size_t count = 0u;

    if (!cue_valid(bank, cue)) return 0u;
    for (size_t index = 0u; index < PCMMIX_BANK_VARIANTS_MAX; ++index)
        if (bank->cues[cue].variants[index].occupied) ++count;
    return count;
}

const pcmmix_sample *pcmmix_bank_sample_at(const pcmmix_bank *bank,
                                           uint32_t cue, uint32_t variant)
{
    if (!slot_valid(bank, cue, variant) ||
        !bank->cues[cue].variants[variant].occupied) return NULL;
    return &bank->cues[cue].variants[variant].sample;
}

static uint32_t next_random(pcmmix_bank *bank)
{
    uint32_t value = bank->random_state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    bank->random_state = value != 0u ? value : 0x6d2b79f5u;
    return bank->random_state;
}

int pcmmix_bank_choose(pcmmix_bank *bank, uint32_t cue)
{
    uint8_t occupied[PCMMIX_BANK_VARIANTS_MAX];
    size_t count = 0u;
    size_t selection;
    pcmmix_bank_cue *selected;

    if (!cue_valid(bank, cue)) return -1;
    selected = &bank->cues[cue];
    for (uint8_t index = 0u; index < PCMMIX_BANK_VARIANTS_MAX; ++index)
        if (selected->variants[index].occupied) occupied[count++] = index;
    if (count == 0u) return -1;
    selection = (size_t)(next_random(bank) % (uint32_t)count);
    if (count > 1u && occupied[selection] == selected->last_variant) {
        ++selection;
        if (selection == count) selection = 0u;
    }
    selected->last_variant = occupied[selection];
    return (int)occupied[selection];
}

static int play_selected(pcmmix *mixer, pcmmix_bank *bank, uint32_t cue,
                         float gain, float pitch, bool looping)
{
    const pcmmix_bank_variant *selected;
    int variant = pcmmix_bank_choose(bank, cue);

    if (variant < 0) return -1;
    selected = &bank->cues[cue].variants[(uint32_t)variant];
    gain = normalized_gain(gain) * selected->gain;
    pitch = normalized_pitch(pitch) * selected->pitch;
    return looping ? pcmmix_loop(mixer, &selected->sample, gain, pitch) :
                     pcmmix_play(mixer, &selected->sample, gain, pitch);
}

int pcmmix_bank_play(pcmmix *mixer, pcmmix_bank *bank, uint32_t cue,
                     float gain, float pitch)
{
    return play_selected(mixer, bank, cue, gain, pitch, false);
}

int pcmmix_bank_loop(pcmmix *mixer, pcmmix_bank *bank, uint32_t cue,
                     float gain, float pitch)
{
    return play_selected(mixer, bank, cue, gain, pitch, true);
}
