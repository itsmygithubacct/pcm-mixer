/* Strict RIFF/WAVE loader for the mixer's fixed transport format.
 *
 * Deliberately strict: only internally consistent PCM, mono, 16-bit,
 * 44100 Hz assets are accepted. Chunks are parsed as a bounded stream so a
 * maximum-size file does not need a second whole-file allocation beside the
 * decoded sample buffer. */
#include "pcm_mixer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAV_RATE 44100u
#define WAV_CHANNELS 1u
#define WAV_BITS 16u

/* 64 MiB is over 12 minutes of mono 44.1 kHz audio. */
#define WAV_MAX_BYTES ((long)1 << 26)
#define WAV_READ_FRAMES 4096u

static uint16_t wav_u16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t wav_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool skip_bytes(FILE *file, size_t count)
{
    return fseek(file, (long)count, SEEK_CUR) == 0;
}

int16_t *pcmmix_wav_load(const char *path, size_t *out_frames, char *err,
                         size_t err_len)
{
    uint8_t riff_header[12];
    uint8_t chunk_header[8];
    int16_t *samples = NULL;
    FILE *file = NULL;
    long file_size;
    size_t bounded_size;
    size_t position;
    long data_offset = -1;
    uint32_t data_length = 0u;
    bool have_format = false;
    bool have_data = false;

    if (out_frames != NULL) *out_frames = 0u;
    if (err != NULL && err_len != 0u) err[0] = '\0';

#define WAV_FAIL(...)                                                         \
    do {                                                                      \
        if (err != NULL && err_len != 0u)                                     \
            (void)snprintf(err, err_len, __VA_ARGS__);                        \
        free(samples);                                                        \
        if (file != NULL) (void)fclose(file);                                 \
        return NULL;                                                          \
    } while (0)

    if (path == NULL || out_frames == NULL) WAV_FAIL("invalid arguments");
    file = fopen(path, "rb");
    if (file == NULL) WAV_FAIL("cannot open %s", path);
    if (fseek(file, 0, SEEK_END) != 0) WAV_FAIL("cannot seek %s", path);
    file_size = ftell(file);
    if (file_size < 44 || file_size > WAV_MAX_BYTES)
        WAV_FAIL("implausible size for %s", path);
    bounded_size = (size_t)file_size;
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(riff_header, 1u, sizeof riff_header, file) !=
            sizeof riff_header)
        WAV_FAIL("short RIFF header: %s", path);
    if (memcmp(riff_header, "RIFF", 4u) != 0 ||
        memcmp(riff_header + 8u, "WAVE", 4u) != 0)
        WAV_FAIL("not a RIFF/WAVE file: %s", path);
    if ((uint64_t)wav_u32(riff_header + 4u) + UINT64_C(8) !=
        (uint64_t)bounded_size)
        WAV_FAIL("RIFF size does not match file: %s", path);

    position = sizeof riff_header;
    while (position < bounded_size) {
        uint32_t length;
        size_t padded;
        size_t remaining;

        if (bounded_size - position < sizeof chunk_header)
            WAV_FAIL("trailing partial chunk header: %s", path);
        if (fread(chunk_header, 1u, sizeof chunk_header, file) !=
            sizeof chunk_header)
            WAV_FAIL("short chunk header: %s", path);
        position += sizeof chunk_header;
        length = wav_u32(chunk_header + 4u);
        remaining = bounded_size - position;
        if ((size_t)length > remaining)
            WAV_FAIL("chunk overruns file: %s", path);
        padded = (size_t)length;
        if ((length & 1u) != 0u) ++padded;
        if (padded > remaining)
            WAV_FAIL("chunk padding overruns file: %s", path);

        if (memcmp(chunk_header, "fmt ", 4u) == 0) {
            uint8_t format_bytes[16];
            uint16_t format;
            uint16_t channels;
            uint32_t rate;
            uint32_t byte_rate;
            uint16_t block_align;
            uint16_t bits;
            uint64_t expected_byte_rate;
            uint32_t expected_align;

            if (have_format) WAV_FAIL("duplicate fmt chunk: %s", path);
            if (length < sizeof format_bytes)
                WAV_FAIL("short fmt chunk: %s", path);
            if (fread(format_bytes, 1u, sizeof format_bytes, file) !=
                sizeof format_bytes)
                WAV_FAIL("short fmt body: %s", path);
            format = wav_u16(format_bytes);
            channels = wav_u16(format_bytes + 2u);
            rate = wav_u32(format_bytes + 4u);
            byte_rate = wav_u32(format_bytes + 8u);
            block_align = wav_u16(format_bytes + 12u);
            bits = wav_u16(format_bytes + 14u);
            if (format != 1u)
                WAV_FAIL("%s is not PCM (format %u)", path,
                         (unsigned int)format);
            if (channels != WAV_CHANNELS || rate != WAV_RATE ||
                bits != WAV_BITS)
                WAV_FAIL("%s is %uch/%ubit/%uHz, expected %uch/%ubit/%uHz",
                         path, (unsigned int)channels, (unsigned int)bits,
                         (unsigned int)rate, WAV_CHANNELS, WAV_BITS, WAV_RATE);
            expected_align = (uint32_t)channels * (uint32_t)bits / 8u;
            expected_byte_rate = (uint64_t)rate * expected_align;
            if (block_align != expected_align ||
                byte_rate != expected_byte_rate)
                WAV_FAIL("inconsistent PCM rate/alignment in %s", path);
            have_format = true;
            if (!skip_bytes(file, padded - sizeof format_bytes))
                WAV_FAIL("cannot seek fmt chunk: %s", path);
        } else {
            if (memcmp(chunk_header, "data", 4u) == 0) {
                if (have_data) WAV_FAIL("duplicate data chunk: %s", path);
                if ((length & 1u) != 0u)
                    WAV_FAIL("odd PCM data length: %s", path);
                data_offset = (long)position;
                data_length = length;
                have_data = true;
            }
            if (!skip_bytes(file, padded))
                WAV_FAIL("cannot seek chunk body: %s", path);
        }
        position += padded;
    }

    if (!have_format) WAV_FAIL("no fmt chunk: %s", path);
    if (!have_data) WAV_FAIL("no data chunk: %s", path);
    if (data_length < 2u) WAV_FAIL("empty data chunk: %s", path);

    const size_t frame_count = (size_t)data_length / 2u;
    samples = malloc(frame_count * sizeof samples[0]);
    if (samples == NULL) WAV_FAIL("out of memory decoding %s", path);
    if (fseek(file, data_offset, SEEK_SET) != 0)
        WAV_FAIL("cannot seek PCM data: %s", path);
    for (size_t decoded = 0u; decoded < frame_count;) {
        uint8_t bytes[WAV_READ_FRAMES * 2u];
        size_t take = frame_count - decoded;

        if (take > WAV_READ_FRAMES) take = WAV_READ_FRAMES;
        if (fread(bytes, 2u, take, file) != take)
            WAV_FAIL("short PCM data: %s", path);
        for (size_t index = 0u; index < take; ++index) {
            const uint16_t raw = wav_u16(bytes + index * 2u);

            samples[decoded + index] = raw <= (uint16_t)INT16_MAX ?
                (int16_t)raw : (int16_t)((int32_t)raw - 65536);
        }
        decoded += take;
    }
    if (fclose(file) != 0) {
        file = NULL;
        WAV_FAIL("cannot close %s", path);
    }
    file = NULL;
    *out_frames = frame_count;
    return samples;

#undef WAV_FAIL
}

void pcmmix_wav_free(int16_t *frames)
{
    free(frames);
}
