#include "pcm_mixer.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum { SAMPLE_COUNT = 7, DATA_BYTES = 32 * 1024 * 1024 };

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value & UINT16_C(0xff));
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & UINT32_C(0xff));
    bytes[1] = (uint8_t)((value >> 8) & UINT32_C(0xff));
    bytes[2] = (uint8_t)((value >> 16) & UINT32_C(0xff));
    bytes[3] = (uint8_t)((value >> 24) & UINT32_C(0xff));
}

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

int main(void)
{
    uint8_t header[44] = {0};
    char path[512];
    uint64_t samples[SAMPLE_COUNT];
    uint64_t checksum = 0u;
    const char *temporary_directory = getenv("TMPDIR");
    int path_length;
    int descriptor;
    bool prepared;

    if (temporary_directory == NULL || temporary_directory[0] == '\0')
        temporary_directory = "/tmp";
    path_length = snprintf(path, sizeof path,
                           "%s/pcmmix-wav-benchmark-XXXXXX",
                           temporary_directory);
    if (path_length < 0 || (size_t)path_length >= sizeof path)
        return EXIT_FAILURE;
    descriptor = mkstemp(path);
    if (descriptor < 0) return EXIT_FAILURE;
    (void)memcpy(header, "RIFF", 4u);
    put_u32(header + 4u, DATA_BYTES + 36u);
    (void)memcpy(header + 8u, "WAVEfmt ", 8u);
    put_u32(header + 16u, 16u);
    put_u16(header + 20u, 1u);
    put_u16(header + 22u, 1u);
    put_u32(header + 24u, 44100u);
    put_u32(header + 28u, 88200u);
    put_u16(header + 32u, 2u);
    put_u16(header + 34u, 16u);
    (void)memcpy(header + 36u, "data", 4u);
    put_u32(header + 40u, DATA_BYTES);
    prepared = ftruncate(descriptor,
                         (off_t)(sizeof header + DATA_BYTES)) == 0 &&
               pwrite(descriptor, header, sizeof header, 0) ==
                   (ssize_t)sizeof header;
    if (close(descriptor) != 0) prepared = false;
    if (!prepared) {
        (void)unlink(path);
        return EXIT_FAILURE;
    }
    for (size_t sample = 0u; sample < SAMPLE_COUNT; ++sample) {
        char error[256];
        size_t frames = 0u;
        const uint64_t started = monotonic_nanoseconds();
        int16_t *decoded = pcmmix_wav_load(path, &frames, error, sizeof error);

        samples[sample] = monotonic_nanoseconds() - started;
        if (decoded == NULL || frames != DATA_BYTES / 2u) {
            (void)fprintf(stderr, "%s\n", error);
            pcmmix_wav_free(decoded);
            (void)unlink(path);
            return EXIT_FAILURE;
        }
        checksum += frames + (uint16_t)decoded[frames - 1u];
        pcmmix_wav_free(decoded);
    }
    if (unlink(path) != 0) return EXIT_FAILURE;
    qsort(samples, SAMPLE_COUNT, sizeof samples[0], compare_u64);
    (void)printf("wav_ns_per_byte=%.3f\n",
                 (double)samples[SAMPLE_COUNT / 2] / (double)DATA_BYTES);
    (void)printf("checksum=%" PRIu64 "\n", checksum);
    return checksum != 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
