#include "pcmmix_bank.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",             \
                          __FILE__, __LINE__, #condition);                     \
            return false;                                                     \
        }                                                                     \
    } while (false)

static void put_u16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
}

static bool write_wav(const char *path, int16_t value)
{
    unsigned char bytes[52] = {0};
    FILE *file;

    (void)memcpy(bytes, "RIFF", 4u);
    put_u32(bytes + 4u, 44u);
    (void)memcpy(bytes + 8u, "WAVEfmt ", 8u);
    put_u32(bytes + 16u, 16u);
    put_u16(bytes + 20u, 1u);
    put_u16(bytes + 22u, 1u);
    put_u32(bytes + 24u, 44100u);
    put_u32(bytes + 28u, 88200u);
    put_u16(bytes + 32u, 2u);
    put_u16(bytes + 34u, 16u);
    (void)memcpy(bytes + 36u, "data", 4u);
    put_u32(bytes + 40u, 8u);
    for (size_t index = 44u; index < sizeof bytes; index += 2u)
        put_u16(bytes + index, (uint16_t)value);
    file = fopen(path, "wb");
    if (file == NULL) return false;
    if (fwrite(bytes, 1u, sizeof bytes, file) != sizeof bytes) {
        (void)fclose(file);
        return false;
    }
    return fclose(file) == 0;
}

static bool test_registration_selection_and_playback(void)
{
    int16_t tone[16];
    pcmmix_sample sample;
    pcmmix_options options;
    pcmmix_bank bank;
    pcmmix mixer;
    int16_t output[16];
    int previous = -1;

    for (size_t index = 0u; index < 16u; ++index) tone[index] = 10000;
    sample = (pcmmix_sample){tone, 16u};
    CHECK(pcmmix_bank_init(&bank, 4u, 123u));
    CHECK(pcmmix_bank_set_borrowed(&bank, 0u, 0u, &sample, 0.5f, 1.0f));
    CHECK(pcmmix_bank_variant_count(&bank, 0u) == 1u);
    CHECK(pcmmix_bank_sample_at(&bank, 0u, 0u) != NULL);
    CHECK(pcmmix_bank_sample_at(&bank, 0u, 1u) == NULL);

    CHECK(pcmmix_bank_set_borrowed(&bank, 1u, 0u, &sample, 1.0f, 1.0f));
    CHECK(pcmmix_bank_set_borrowed(&bank, 1u, 2u, &sample, 1.0f, 1.0f));
    CHECK(pcmmix_bank_variant_count(&bank, 1u) == 2u);
    for (size_t index = 0u; index < 20u; ++index) {
        const int selected = pcmmix_bank_choose(&bank, 1u);
        CHECK(selected == 0 || selected == 2);
        CHECK(selected != previous);
        previous = selected;
    }

    pcmmix_options_init(&options);
    options.offline = true;
    options.soft_clip = false;
    CHECK(pcmmix_start(&mixer, &options));
    CHECK(pcmmix_bank_play(&mixer, &bank, 0u, 0.5f, 1.0f) > 0);
    pcmmix_mix_block(&mixer, output, 16u);
    CHECK(output[4] >= 2498 && output[4] <= 2502);
    CHECK(pcmmix_bank_loop(&mixer, &bank, 0u, 1.0f, 1.0f) > 0);
    pcmmix_stop(&mixer);
    pcmmix_bank_clear(&bank);
    return true;
}

static bool test_owned_and_transactional_loading(void)
{
    pcmmix_bank bank;
    int16_t *owned;
    char first_path[256];
    char second_path[256];
    char missing_path[256];
    const char *good_paths[2];
    const char *bad_paths[2];
    const pcmmix_sample *before;
    char error[256] = "";

    CHECK(pcmmix_bank_init(&bank, 4u, 1u));
    owned = malloc(12u * sizeof *owned);
    CHECK(owned != NULL);
    for (size_t index = 0u; index < 12u; ++index) owned[index] = 3000;
    CHECK(pcmmix_bank_take(&bank, 2u, 0u, owned, 12u, 1.0f, 1.0f));

    (void)snprintf(first_path, sizeof first_path, "/tmp/pcmmix-bank-%ld-a.wav",
                   (long)getpid());
    (void)snprintf(second_path, sizeof second_path, "/tmp/pcmmix-bank-%ld-b.wav",
                   (long)getpid());
    (void)snprintf(missing_path, sizeof missing_path,
                   "/tmp/pcmmix-bank-%ld-missing.wav", (long)getpid());
    CHECK(write_wav(first_path, 4000));
    CHECK(write_wav(second_path, 8000));
    CHECK(pcmmix_bank_load_wav(&bank, 3u, 0u, first_path, 1.0f, 1.0f,
                               error, sizeof error));
    before = pcmmix_bank_sample_at(&bank, 3u, 0u);
    CHECK(before != NULL && before->frames[0] == 4000);

    bad_paths[0] = second_path;
    bad_paths[1] = missing_path;
    CHECK(!pcmmix_bank_load_variants(&bank, 3u, bad_paths, 2u, 1.0f, 1.0f,
                                     error, sizeof error));
    CHECK(strstr(error, "variant 1") != NULL);
    CHECK(pcmmix_bank_sample_at(&bank, 3u, 0u) == before);
    CHECK(pcmmix_bank_variant_count(&bank, 3u) == 1u);

    good_paths[0] = first_path;
    good_paths[1] = second_path;
    CHECK(pcmmix_bank_load_variants(&bank, 3u, good_paths, 2u, 0.75f, 1.0f,
                                    error, sizeof error));
    CHECK(pcmmix_bank_variant_count(&bank, 3u) == 2u);
    CHECK(pcmmix_bank_sample_at(&bank, 3u, 1u)->frames[0] == 8000);

    pcmmix_bank_clear(&bank);
    CHECK(unlink(first_path) == 0);
    CHECK(unlink(second_path) == 0);
    return true;
}

static bool test_invalid_inputs(void)
{
    pcmmix_bank bank;
    pcmmix_sample empty = {0};
    int16_t *frames = malloc(sizeof *frames);

    CHECK(frames != NULL);
    CHECK(!pcmmix_bank_init(NULL, 1u, 0u));
    CHECK(!pcmmix_bank_init(&bank, PCMMIX_BANK_CUES_MAX + 1u, 0u));
    CHECK(pcmmix_bank_init(&bank, 1u, 0u));
    CHECK(!pcmmix_bank_set_borrowed(&bank, 1u, 0u, &empty, 1.0f, 1.0f));
    CHECK(!pcmmix_bank_take(&bank, 0u, PCMMIX_BANK_VARIANTS_MAX, frames, 1u,
                            1.0f, 1.0f));
    free(frames); /* ownership transfers only on success */
    CHECK(pcmmix_bank_choose(&bank, 0u) == -1);
    pcmmix_bank_clear(&bank);
    return true;
}

int main(void)
{
    CHECK(test_registration_selection_and_playback());
    CHECK(test_owned_and_transactional_loading());
    CHECK(test_invalid_inputs());
    (void)puts("ok: pcmmix-bank");
    return 0;
}
