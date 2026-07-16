/* Strict RIFF/WAVE loader for the mixer's fixed transport format.
 *
 * Deliberately strict: only PCM, mono, 16-bit, 44100 Hz is accepted. An
 * asset bank that does not match is a bug in whatever generated it, and
 * silently resampling or downmixing would hide that. Callers treat a NULL
 * return as "degrade to silence and report the reason", never as
 * "synthesise something instead". */
#include "pcm_mixer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAV_RATE 44100u
#define WAV_CHANNELS 1u
#define WAV_BITS 16u

/* Guards a corrupt or hostile header from asking for a huge allocation:
 * 64 MiB is over 12 minutes of mono 44.1 kHz audio. */
#define WAV_MAX_BYTES ((long)1 << 26)

static uint16_t wav_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t wav_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int16_t *pcmmix_wav_load(const char *path, size_t *out_frames, char *err,
                         size_t err_len)
{
    if (out_frames) *out_frames = 0;
    if (err && err_len) err[0] = '\0';

#define WAV_FAIL(...)                                                        \
    do {                                                                     \
        if (err && err_len) snprintf(err, err_len, __VA_ARGS__);             \
        free(bytes);                                                         \
        if (fp) fclose(fp);                                                  \
        return NULL;                                                         \
    } while (0)

    uint8_t *bytes = NULL;
    FILE *fp = NULL;
    if (!path || !out_frames) WAV_FAIL("invalid arguments");

    fp = fopen(path, "rb");
    if (!fp) WAV_FAIL("cannot open %s", path);

    if (fseek(fp, 0, SEEK_END) != 0) WAV_FAIL("cannot seek %s", path);
    long size = ftell(fp);
    if (size < 44 || size > WAV_MAX_BYTES)
        WAV_FAIL("implausible size for %s", path);
    rewind(fp);

    bytes = malloc((size_t)size);
    if (!bytes) WAV_FAIL("out of memory reading %s", path);
    if (fread(bytes, 1, (size_t)size, fp) != (size_t)size)
        WAV_FAIL("short read on %s", path);
    fclose(fp);
    fp = NULL;

    if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0)
        WAV_FAIL("not a RIFF/WAVE file: %s", path);

    /* Walk chunks rather than assuming a canonical 44-byte header: writers
     * are free to emit LIST/fact chunks before data. */
    bool have_fmt = false;
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    size_t file_size = (size_t)size;
    size_t pos = 12;

    while (pos + 8 <= file_size) {
        const uint8_t *id = bytes + pos;
        uint32_t len = wav_u32(bytes + pos + 4);
        size_t body = pos + 8;
        if (len > file_size - body) /* overflow-safe bound */
            WAV_FAIL("chunk overruns file: %s", path);

        if (memcmp(id, "fmt ", 4) == 0) {
            if (len < 16) WAV_FAIL("short fmt chunk: %s", path);
            uint16_t format = wav_u16(bytes + body);
            uint16_t channels = wav_u16(bytes + body + 2);
            uint32_t rate = wav_u32(bytes + body + 4);
            uint16_t bits = wav_u16(bytes + body + 14);
            if (format != 1)
                WAV_FAIL("%s is not PCM (format %u)", path,
                         (unsigned)format);
            if (channels != WAV_CHANNELS || rate != WAV_RATE ||
                bits != WAV_BITS)
                WAV_FAIL("%s is %uch/%ubit/%uHz, expected %uch/%ubit/%uHz",
                         path, (unsigned)channels, (unsigned)bits,
                         (unsigned)rate, WAV_CHANNELS, WAV_BITS, WAV_RATE);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            data = bytes + body;
            data_len = len;
        }

        pos = body + len + (len & 1u); /* chunks are word-aligned */
    }

    if (!have_fmt) WAV_FAIL("no fmt chunk: %s", path);
    if (!data) WAV_FAIL("no data chunk: %s", path);
    if (data_len < 2) WAV_FAIL("empty data chunk: %s", path);

    size_t frames = data_len / 2u; /* a trailing odd byte is ignored */
    int16_t *samples = malloc(frames * sizeof *samples);
    if (!samples) WAV_FAIL("out of memory decoding %s", path);
    for (size_t i = 0; i < frames; i++)
        samples[i] = (int16_t)wav_u16(data + i * 2u);

    free(bytes);
    *out_frames = frames;
    return samples;

#undef WAV_FAIL
}

void pcmmix_wav_free(int16_t *frames)
{
    free(frames);
}
