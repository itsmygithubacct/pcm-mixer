#include "pcmmix_bank.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
    SAMPLE_COUNT = 5,
    BLOCK_FRAMES = 1024,
    BLOCK_COUNT = 4096,
    SOURCE_FRAMES = 4096,
    BANK_ITERATIONS = 5000000
};

typedef enum workload {
    WORK_IDLE_SOFT,
    WORK_UNITY_HARD,
    WORK_RESAMPLE_HARD,
    WORK_UNITY_SOFT,
    WORK_16_VOICES_HARD
} workload;

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;

    return a > b ? 1 : a < b ? -1 : 0;
}

static uint64_t run_mix(workload kind, volatile int64_t *checksum)
{
    static int16_t source[SOURCE_FRAMES];
    int16_t output[BLOCK_FRAMES];
    pcmmix_sample sample = {source, SOURCE_FRAMES};
    pcmmix_options options;
    pcmmix mixer;
    uint64_t started;
    int voice_count = kind == WORK_16_VOICES_HARD ? 16 :
                      kind == WORK_IDLE_SOFT ? 0 : 1;
    float pitch = kind == WORK_RESAMPLE_HARD ? 1.37f : 1.0f;

    for (size_t index = 0u; index < SOURCE_FRAMES; ++index) {
        const int value = (int)((index * 1103515245u + 12345u) >> 20);
        source[index] = (int16_t)((value & 2047) - 1024);
    }
    pcmmix_options_init(&options);
    options.offline = true;
    options.max_voices = 32u;
    options.soft_clip = kind == WORK_IDLE_SOFT || kind == WORK_UNITY_SOFT;
    if (!pcmmix_start(&mixer, &options)) return 0u;
    for (int voice = 0; voice < voice_count; ++voice) {
        const float volume = voice_count > 1 ? 0.03125f : 1.0f;

        if (pcmmix_loop(&mixer, &sample, volume, pitch) <= 0) return 0u;
    }
    started = monotonic_nanoseconds();
    for (int block = 0; block < BLOCK_COUNT; ++block) {
        pcmmix_mix_block(&mixer, output, BLOCK_FRAMES);
        *checksum += output[(unsigned int)block % BLOCK_FRAMES];
    }
    started = monotonic_nanoseconds() - started;
    pcmmix_stop(&mixer);
    return started;
}

static double median_mix(workload kind, volatile int64_t *checksum)
{
    uint64_t samples[SAMPLE_COUNT];

    for (size_t sample = 0u; sample < SAMPLE_COUNT; ++sample)
        samples[sample] = run_mix(kind, checksum);
    qsort(samples, SAMPLE_COUNT, sizeof samples[0], compare_u64);
    return (double)samples[SAMPLE_COUNT / 2] /
           ((double)BLOCK_FRAMES * (double)BLOCK_COUNT);
}

static double median_bank(volatile int64_t *checksum)
{
    static int16_t frame = 1;
    const pcmmix_sample clip = {&frame, 1u};
    uint64_t samples[SAMPLE_COUNT];

    for (size_t sample = 0u; sample < SAMPLE_COUNT; ++sample) {
        pcmmix_bank bank;

        if (!pcmmix_bank_init(&bank, 1u, 123u)) return 0.0;
        for (uint32_t variant = 0u; variant < PCMMIX_BANK_VARIANTS_MAX;
             ++variant) {
            if (!pcmmix_bank_set_borrowed(&bank, 0u, variant, &clip,
                                          1.0f, 1.0f)) return 0.0;
        }
        const uint64_t started = monotonic_nanoseconds();
        for (int iteration = 0; iteration < BANK_ITERATIONS; ++iteration)
            *checksum += pcmmix_bank_choose(&bank, 0u);
        samples[sample] = monotonic_nanoseconds() - started;
        pcmmix_bank_clear(&bank);
    }
    qsort(samples, SAMPLE_COUNT, sizeof samples[0], compare_u64);
    return (double)samples[SAMPLE_COUNT / 2] / (double)BANK_ITERATIONS;
}

int main(void)
{
    volatile int64_t checksum = 0;

    (void)printf("idle_soft_ns_per_frame=%.3f\n",
                 median_mix(WORK_IDLE_SOFT, &checksum));
    (void)printf("unity_hard_ns_per_frame=%.3f\n",
                 median_mix(WORK_UNITY_HARD, &checksum));
    (void)printf("resample_hard_ns_per_frame=%.3f\n",
                 median_mix(WORK_RESAMPLE_HARD, &checksum));
    (void)printf("unity_soft_ns_per_frame=%.3f\n",
                 median_mix(WORK_UNITY_SOFT, &checksum));
    (void)printf("voices16_hard_ns_per_frame=%.3f\n",
                 median_mix(WORK_16_VOICES_HARD, &checksum));
    (void)printf("bank_choose_ns_per_call=%.3f\n",
                 median_bank(&checksum));
    (void)printf("checksum=%" PRId64 "\n", checksum);
    return checksum != 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
