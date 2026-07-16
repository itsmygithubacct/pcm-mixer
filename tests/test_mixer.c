/* pcm-mixer test suite. Everything runs against an offline mixer or a
 * captured pipe; no test ever spawns a real audio player or makes noise. */
#include "pcm_mixer.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(                                                    \
                stderr,                                                       \
                "%s:%d: check failed: %s\n",                                  \
                __FILE__,                                                     \
                __LINE__,                                                     \
                #condition);                                                  \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define CHECK_NEAR(value, expected, tolerance)                                \
    do {                                                                      \
        long check_delta = (long)(value) - (long)(expected);                  \
        if (check_delta < 0) check_delta = -check_delta;                      \
        if (check_delta > (long)(tolerance)) {                                \
            (void)fprintf(                                                    \
                stderr,                                                       \
                "%s:%d: %s = %ld, expected %ld +/- %ld\n",                    \
                __FILE__,                                                     \
                __LINE__,                                                     \
                #value,                                                       \
                (long)(value),                                                \
                (long)(expected),                                             \
                (long)(tolerance));                                           \
            return false;                                                     \
        }                                                                     \
    } while (false)

static void
sleep_ms(long milliseconds)
{
    struct timespec ts = { milliseconds / 1000,
                           (milliseconds % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static void
fill_const(int16_t *dst, size_t count, int16_t value)
{
    for (size_t i = 0; i < count; i++) dst[i] = value;
}

static bool
start_offline(pcmmix *mixer, uint32_t sample_rate, uint32_t max_voices,
              bool soft_clip)
{
    pcmmix_options options;
    pcmmix_options_init(&options);
    options.offline = true;
    options.sample_rate = sample_rate;
    options.max_voices = max_voices;
    options.soft_clip = soft_clip;
    return pcmmix_start(mixer, &options);
}

/* --- WAV fixture helpers ---------------------------------------------- */

static void
put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void
put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* Build a WAV image in buf. with_junk inserts a LIST chunk and an
 * odd-length chunk (with pad byte) before fmt/data. Returns bytes used. */
static size_t
make_wav(uint8_t *buf, const int16_t *frames, size_t frame_count,
         uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits,
         bool with_junk)
{
    size_t at = 12;
    if (with_junk) {
        memcpy(buf + at, "LIST", 4);
        put_u32(buf + at + 4, 4u);
        memcpy(buf + at + 8, "INFO", 4);
        at += 12;
        memcpy(buf + at, "odd ", 4);
        put_u32(buf + at + 4, 3u); /* odd length: parser must pad */
        buf[at + 8] = 0x41;
        buf[at + 9] = 0x42;
        buf[at + 10] = 0x43;
        buf[at + 11] = 0x00; /* pad byte */
        at += 12;
    }
    memcpy(buf + at, "fmt ", 4);
    put_u32(buf + at + 4, 16u);
    put_u16(buf + at + 8, format);
    put_u16(buf + at + 10, channels);
    put_u32(buf + at + 12, rate);
    put_u32(buf + at + 16, rate * channels * bits / 8u);
    put_u16(buf + at + 20, (uint16_t)(channels * bits / 8u));
    put_u16(buf + at + 22, bits);
    at += 24;
    memcpy(buf + at, "data", 4);
    put_u32(buf + at + 4, (uint32_t)(frame_count * 2u));
    at += 8;
    for (size_t i = 0; i < frame_count; i++) {
        put_u16(buf + at, (uint16_t)frames[i]);
        at += 2;
    }
    memcpy(buf, "RIFF", 4);
    put_u32(buf + 4, (uint32_t)(at - 8));
    memcpy(buf + 8, "WAVE", 4);
    return at;
}

static bool
write_file(const char *path, const uint8_t *bytes, size_t count)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = fwrite(bytes, 1, count, fp) == count;
    return fclose(fp) == 0 && ok;
}

static const char *
temp_path(char *buf, size_t buf_len, const char *name)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    (void)snprintf(buf, buf_len, "%s/pcmmix-test-%ld-%s", dir,
                   (long)getpid(), name);
    return buf;
}

/* --- WAV loader -------------------------------------------------------- */

static bool
test_wav_round_trip(void)
{
    enum { FRAMES = 64 };
    int16_t frames[FRAMES];
    for (size_t i = 0; i < FRAMES; i++)
        frames[i] = (int16_t)((int)i * 250 - 8000);

    uint8_t image[1024];
    size_t image_len =
        make_wav(image, frames, FRAMES, 1u, 1u, 44100u, 16u, true);

    char path[512];
    temp_path(path, sizeof path, "roundtrip.wav");
    CHECK(write_file(path, image, image_len));

    char err[256] = "";
    size_t loaded_count = 0;
    int16_t *loaded = pcmmix_wav_load(path, &loaded_count, err, sizeof err);
    CHECK(loaded != NULL);
    CHECK(err[0] == '\0');
    CHECK(loaded_count == FRAMES);
    CHECK(memcmp(loaded, frames, sizeof frames) == 0);
    pcmmix_wav_free(loaded);
    CHECK(unlink(path) == 0);
    return true;
}

static bool
expect_wav_failure(const uint8_t *image, size_t image_len,
                   const char *needle)
{
    char path[512];
    temp_path(path, sizeof path, "reject.wav");
    if (!write_file(path, image, image_len)) return false;

    char err[256] = "";
    size_t count = 999;
    int16_t *loaded = pcmmix_wav_load(path, &count, err, sizeof err);
    (void)unlink(path);
    if (loaded) {
        pcmmix_wav_free(loaded);
        (void)fprintf(stderr, "load unexpectedly succeeded (%s)\n", needle);
        return false;
    }
    if (count != 0 || strstr(err, needle) == NULL) {
        (void)fprintf(stderr, "error \"%s\" does not mention \"%s\"\n", err,
                      needle);
        return false;
    }
    return true;
}

static bool
test_wav_rejects_malformed(void)
{
    int16_t frames[8] = {0};
    uint8_t image[512];
    size_t image_len;

    /* Truncated header. */
    image_len = make_wav(image, frames, 8, 1u, 1u, 44100u, 16u, false);
    CHECK(expect_wav_failure(image, 20, "implausible"));

    /* Not a RIFF file. */
    memset(image, 'X', 64);
    CHECK(expect_wav_failure(image, 64, "RIFF"));

    /* Stereo. */
    image_len = make_wav(image, frames, 8, 1u, 2u, 44100u, 16u, false);
    CHECK(expect_wav_failure(image, image_len, "expected"));

    /* Wrong rate. */
    image_len = make_wav(image, frames, 8, 1u, 1u, 22050u, 16u, false);
    CHECK(expect_wav_failure(image, image_len, "expected"));

    /* Non-PCM format code. */
    image_len = make_wav(image, frames, 8, 3u, 1u, 44100u, 16u, false);
    CHECK(expect_wav_failure(image, image_len, "not PCM"));

    /* Chunk length overruns the file. */
    image_len = make_wav(image, frames, 8, 1u, 1u, 44100u, 16u, false);
    put_u32(image + 40, 0x00ffffffu); /* data chunk length field */
    CHECK(expect_wav_failure(image, image_len, "overruns"));

    /* fmt chunk but no data chunk. */
    image_len = make_wav(image, frames, 8, 1u, 1u, 44100u, 16u, false);
    memcpy(image + 36, "misc", 4); /* rename the data chunk id */
    CHECK(expect_wav_failure(image, image_len, "no data chunk"));

    /* Nonexistent file. */
    char err[256] = "";
    size_t count = 999;
    CHECK(pcmmix_wav_load("/nonexistent/nowhere.wav", &count, err,
                          sizeof err) == NULL);
    CHECK(count == 0);
    CHECK(strstr(err, "cannot open") != NULL);
    return true;
}

/* --- resampler --------------------------------------------------------- */

static bool
test_resampler_pitch_and_interpolation(void)
{
    pcmmix mixer;
    CHECK(start_offline(&mixer, 8000u, 4u, false));

    int16_t ramp[100];
    for (size_t i = 0; i < 100; i++) ramp[i] = (int16_t)(i * 100);
    pcmmix_sample sample = { ramp, 100 };
    int16_t out[220];

    /* pitch 2.0 halves the duration: 100 source frames in 50 output
     * frames, sample values striding by two. */
    int voice = pcmmix_play(&mixer, &sample, 1.0f, 2.0f);
    CHECK(voice > 0);
    pcmmix_mix_block(&mixer, out, 100);
    CHECK_NEAR(out[0], 0, 2);
    CHECK_NEAR(out[10], 2000, 2);
    CHECK_NEAR(out[49], 9800, 2);
    CHECK(out[60] == 0); /* finished at half the duration */
    CHECK(!pcmmix_voice_active(&mixer, voice));

    /* pitch 0.5 doubles the duration and interpolates midpoints. */
    voice = pcmmix_play(&mixer, &sample, 1.0f, 0.5f);
    CHECK(voice > 0);
    pcmmix_mix_block(&mixer, out, 220);
    CHECK_NEAR(out[0], 0, 2);
    CHECK_NEAR(out[1], 50, 2);   /* midpoint of 0 and 100 */
    CHECK_NEAR(out[2], 100, 2);
    CHECK_NEAR(out[3], 150, 2);  /* midpoint of 100 and 200 */
    CHECK_NEAR(out[100], 5000, 2);
    CHECK(out[210] == 0);

    pcmmix_stop(&mixer);
    return true;
}

/* --- mix_block --------------------------------------------------------- */

static bool
test_single_voice_reproduces_sample(void)
{
    pcmmix mixer;
    CHECK(start_offline(&mixer, 8000u, 4u, false));

    int16_t tone[50];
    fill_const(tone, 50, 12000);
    pcmmix_sample sample = { tone, 50 };
    CHECK(pcmmix_play(&mixer, &sample, 1.0f, 1.0f) > 0);

    int16_t out[80];
    pcmmix_mix_block(&mixer, out, 80);
    CHECK_NEAR(out[0], 12000, 2);
    CHECK_NEAR(out[25], 12000, 2);
    CHECK_NEAR(out[49], 12000, 2);
    CHECK(out[55] == 0); /* one-shot ended */
    CHECK(out[79] == 0);

    pcmmix_stop(&mixer);
    return true;
}

static bool
test_voice_sum_and_clipping(void)
{
    pcmmix mixer;
    CHECK(start_offline(&mixer, 8000u, 4u, false));

    int16_t a[40], b[40], loud[40];
    fill_const(a, 40, 8000);
    fill_const(b, 40, 4000);
    fill_const(loud, 40, 30000);
    pcmmix_sample sample_a = { a, 40 };
    pcmmix_sample sample_b = { b, 40 };
    pcmmix_sample sample_loud = { loud, 40 };

    /* Two voices sum linearly. */
    CHECK(pcmmix_play(&mixer, &sample_a, 1.0f, 1.0f) > 0);
    CHECK(pcmmix_play(&mixer, &sample_b, 1.0f, 1.0f) > 0);
    int16_t out[40];
    pcmmix_mix_block(&mixer, out, 40);
    CHECK_NEAR(out[10], 12000, 2);

    /* Hard clip: two near-full-scale voices pin at the rails. */
    CHECK(pcmmix_play(&mixer, &sample_loud, 1.0f, 1.0f) > 0);
    CHECK(pcmmix_play(&mixer, &sample_loud, 1.0f, 1.0f) > 0);
    pcmmix_mix_block(&mixer, out, 40);
    CHECK(out[10] == 32767);
    pcmmix_stop(&mixer);

    /* Soft clip: tanh saturation instead of a flat ceiling. */
    CHECK(start_offline(&mixer, 8000u, 4u, true));
    CHECK(pcmmix_play(&mixer, &sample_loud, 1.0f, 1.0f) > 0);
    pcmmix_mix_block(&mixer, out, 40);
    long expected = lrintf(tanhf(30000.0f / 32768.0f) * 32767.0f);
    CHECK_NEAR(out[10], expected, 4);
    CHECK(out[10] < 30000); /* saturated well below the input level */
    pcmmix_stop(&mixer);
    return true;
}

static bool
test_loop_wrap_and_live_voice_set(void)
{
    pcmmix mixer;
    CHECK(start_offline(&mixer, 8000u, 4u, false));

    /* A looping ramp must wrap seamlessly: frame N maps back to frame 0. */
    int16_t ramp[20];
    for (size_t i = 0; i < 20; i++) ramp[i] = (int16_t)(i * 300);
    pcmmix_sample sample = { ramp, 20 };
    int voice = pcmmix_loop(&mixer, &sample, 1.0f, 1.0f);
    CHECK(voice > 0);

    int16_t out[45];
    pcmmix_mix_block(&mixer, out, 45);
    CHECK_NEAR(out[19], 5700, 2);
    CHECK_NEAR(out[20], 0, 2); /* wrapped */
    CHECK_NEAR(out[21], 300, 2);
    CHECK_NEAR(out[40], 0, 2); /* wrapped again */
    CHECK(pcmmix_voice_active(&mixer, voice));

    /* Live volume change applies to the very next block. */
    int16_t drone[20];
    fill_const(drone, 20, 8000);
    pcmmix_sample drone_sample = { drone, 20 };
    int drone_voice = pcmmix_loop(&mixer, &drone_sample, 1.0f, 1.0f);
    CHECK(drone_voice > 0);
    CHECK(pcmmix_voice_stop(&mixer, voice)); /* isolate the drone */

    pcmmix_mix_block(&mixer, out, 20);
    CHECK_NEAR(out[10], 8000, 2);
    CHECK(pcmmix_voice_set(&mixer, drone_voice, 0.5f, 1.0f));
    pcmmix_mix_block(&mixer, out, 20);
    CHECK_NEAR(out[10], 4000, 2);

    /* Stopping makes the handle stale. */
    CHECK(pcmmix_voice_stop(&mixer, drone_voice));
    pcmmix_mix_block(&mixer, out, 20);
    CHECK(out[10] == 0);
    CHECK(!pcmmix_voice_stop(&mixer, drone_voice));
    CHECK(!pcmmix_voice_set(&mixer, drone_voice, 1.0f, 1.0f));

    pcmmix_stop(&mixer);
    return true;
}

static bool
test_handle_generations_and_stealing(void)
{
    pcmmix mixer;
    CHECK(start_offline(&mixer, 8000u, 2u, false));

    int16_t a[100], b[100], c[100];
    fill_const(a, 100, 1000);
    fill_const(b, 100, 2000);
    fill_const(c, 100, 3000);
    pcmmix_sample sample_a = { a, 100 };
    pcmmix_sample sample_b = { b, 100 };
    pcmmix_sample sample_c = { c, 100 };
    int16_t out[100];

    /* A stale handle must be rejected once its slot is recycled. */
    int first = pcmmix_play(&mixer, &sample_a, 1.0f, 1.0f);
    CHECK(first > 0);
    pcmmix_mix_block(&mixer, out, 100);
    pcmmix_mix_block(&mixer, out, 10); /* runs voice past its end */
    CHECK(!pcmmix_voice_active(&mixer, first));
    int second = pcmmix_play(&mixer, &sample_b, 1.0f, 1.0f);
    CHECK(second > 0);
    CHECK(second != first);
    CHECK(!pcmmix_voice_set(&mixer, first, 1.0f, 1.0f)); /* same slot, old gen */
    CHECK(pcmmix_voice_set(&mixer, second, 1.0f, 1.0f));
    CHECK(pcmmix_voice_stop(&mixer, second));

    /* Stealing picks the most-finished voice when every slot is busy. */
    int voice_a = pcmmix_play(&mixer, &sample_a, 1.0f, 1.0f);
    CHECK(voice_a > 0);
    pcmmix_mix_block(&mixer, out, 80); /* voice A now 80% done */
    int voice_b = pcmmix_play(&mixer, &sample_b, 1.0f, 1.0f);
    CHECK(voice_b > 0);
    pcmmix_mix_block(&mixer, out, 10); /* A: 90%, B: 10% */
    int voice_c = pcmmix_play(&mixer, &sample_c, 1.0f, 1.0f);
    CHECK(voice_c > 0);
    CHECK(!pcmmix_voice_active(&mixer, voice_a)); /* A was stolen */
    CHECK(pcmmix_voice_active(&mixer, voice_b));
    CHECK(pcmmix_voice_active(&mixer, voice_c));
    pcmmix_mix_block(&mixer, out, 10);
    CHECK_NEAR(out[0], 5000, 2); /* B + C, no A */

    pcmmix_stop(&mixer);
    return true;
}

/* --- music ------------------------------------------------------------- */

static bool
test_music_crossfade(void)
{
    pcmmix mixer;
    CHECK(start_offline(&mixer, 8000u, 4u, false));

    static int16_t track_a[16000], track_b[16000];
    fill_const(track_a, 16000, 16000);
    fill_const(track_b, 16000, 8000);
    pcmmix_sample music_a = { track_a, 16000 };
    pcmmix_sample music_b = { track_b, 16000 };
    static int16_t out[4200];

    /* Fade in: 0.5 s fade at 8 kHz is 4000 frames to full volume. */
    pcmmix_music_play(&mixer, &music_a, 1.0f, true);
    CHECK(pcmmix_music_current(&mixer) == track_a);
    pcmmix_mix_block(&mixer, out, 4100);
    CHECK_NEAR(out[1999], 8000, 80);  /* half way up */
    CHECK_NEAR(out[4050], 16000, 80); /* at full volume */

    /* Crossfade: A ramps 1 -> 0 while B ramps 0 -> 1 over 4000 frames. */
    pcmmix_music_play(&mixer, &music_b, 1.0f, true);
    CHECK(pcmmix_music_current(&mixer) == track_b);
    pcmmix_mix_block(&mixer, out, 2000);
    CHECK_NEAR(out[1999], 12000, 80); /* 0.5*A + 0.5*B */
    pcmmix_mix_block(&mixer, out, 2200);
    CHECK_NEAR(out[2100], 8000, 80); /* only B remains */
    CHECK(pcmmix_music_current(&mixer) == track_b);

    /* Retargeting the current track does not restart it. */
    pcmmix_music_play(&mixer, &music_b, 0.5f, true);
    pcmmix_mix_block(&mixer, out, 4200);
    CHECK_NEAR(out[4100], 4000, 80);

    pcmmix_stop(&mixer);
    return true;
}

static bool
test_music_stop_fade(void)
{
    pcmmix mixer;
    CHECK(start_offline(&mixer, 8000u, 4u, false));

    static int16_t track[16000];
    fill_const(track, 16000, 16000);
    pcmmix_sample music = { track, 16000 };
    static int16_t out[4100];

    pcmmix_music_play(&mixer, &music, 1.0f, true);
    pcmmix_mix_block(&mixer, out, 4100); /* reach full volume */

    /* 0.25 s fade at 8 kHz is 2000 frames to silence. */
    pcmmix_music_stop(&mixer, 0.25f);
    CHECK(pcmmix_music_current(&mixer) == NULL);
    pcmmix_mix_block(&mixer, out, 2100);
    CHECK_NEAR(out[999], 8000, 80); /* half way down */
    CHECK(out[2050] == 0);

    /* fade_seconds <= 0 stops immediately. */
    pcmmix_music_play(&mixer, &music, 1.0f, true);
    pcmmix_mix_block(&mixer, out, 100);
    pcmmix_music_stop(&mixer, 0.0f);
    pcmmix_mix_block(&mixer, out, 100);
    CHECK(out[0] == 0);
    CHECK(out[99] == 0);

    pcmmix_stop(&mixer);
    return true;
}

/* --- generator hook ----------------------------------------------------- */

static void
quarter_scale_generator(float *dst, size_t frames, void *user)
{
    int *calls = user;
    (*calls)++;
    for (size_t i = 0; i < frames; i++) dst[i] = 0.25f;
}

static bool
test_generator_mixed_under_voices(void)
{
    pcmmix mixer;
    CHECK(start_offline(&mixer, 8000u, 4u, false));

    int calls = 0;
    pcmmix_set_generator(&mixer, quarter_scale_generator, &calls);

    int16_t out[40];
    pcmmix_mix_block(&mixer, out, 40);
    CHECK(calls > 0);
    CHECK_NEAR(out[10], 8192, 2); /* 0.25 of full scale */

    /* Generator output sums with voices. */
    int16_t tone[40];
    fill_const(tone, 40, 8192);
    pcmmix_sample sample = { tone, 40 };
    CHECK(pcmmix_loop(&mixer, &sample, 1.0f, 1.0f) > 0);
    pcmmix_mix_block(&mixer, out, 40);
    CHECK_NEAR(out[10], 16384, 3);

    /* Disabling mutes everything, including the generator. */
    pcmmix_set_enabled(&mixer, false);
    CHECK(!pcmmix_is_enabled(&mixer));
    CHECK(pcmmix_play(&mixer, &sample, 1.0f, 1.0f) == -1);
    pcmmix_mix_block(&mixer, out, 40);
    CHECK(out[10] == 0);

    /* Re-enabling restores the generator; the voices were cleared. */
    pcmmix_set_enabled(&mixer, true);
    pcmmix_mix_block(&mixer, out, 40);
    CHECK_NEAR(out[10], 8192, 2);

    /* Clearing the hook silences it. */
    pcmmix_set_generator(&mixer, NULL, NULL);
    pcmmix_mix_block(&mixer, out, 40);
    CHECK(out[10] == 0);

    pcmmix_stop(&mixer);
    return true;
}

/* --- transport ---------------------------------------------------------- */

static bool
test_sink_capture(void)
{
    char capture[512], command[600];
    temp_path(capture, sizeof capture, "capture.pcm");
    CHECK(snprintf(command, sizeof command, "exec cat > '%s'", capture) <
          (int)sizeof command);
    const char *sink_argv[] = { "/bin/sh", "-c", command, NULL };

    pcmmix_options options;
    pcmmix_options_init(&options);
    options.sink_argv = sink_argv;
    options.soft_clip = false;

    pcmmix mixer;
    CHECK(pcmmix_start(&mixer, &options));
    CHECK(pcmmix_is_running(&mixer));
    const char *backend = pcmmix_backend_name(&mixer);
    CHECK(backend != NULL);
    CHECK(strcmp(backend, "sh") == 0);

    static int16_t tone[4410];
    fill_const(tone, 4410, 12000);
    pcmmix_sample sample = { tone, 4410 };
    int voice = pcmmix_play(&mixer, &sample, 1.0f, 1.0f);
    CHECK(voice > 0);

    /* cat drains faster than real time, so the voice finishes quickly. */
    for (int i = 0; i < 1000 && pcmmix_voice_active(&mixer, voice); i++)
        sleep_ms(5);
    CHECK(!pcmmix_voice_active(&mixer, voice));
    sleep_ms(50); /* one more block of trailing silence */
    pcmmix_stop(&mixer);

    /* The captured s16le stream must contain the tone and some of the
     * silence that is streamed continuously around it. */
    FILE *fp = fopen(capture, "rb");
    CHECK(fp != NULL);
    long tone_frames = 0, zero_frames = 0;
    int16_t chunk[8192];
    size_t got;
    while ((got = fread(chunk, sizeof chunk[0],
                        sizeof chunk / sizeof chunk[0], fp)) > 0) {
        for (size_t i = 0; i < got; i++) {
            if (chunk[i] >= 11998 && chunk[i] <= 12002) tone_frames++;
            else if (chunk[i] == 0) zero_frames++;
        }
    }
    CHECK(fclose(fp) == 0);
    CHECK(unlink(capture) == 0);
    CHECK(tone_frames >= 4000); /* nearly all 4410 tone frames captured */
    CHECK(zero_frames >= 64);   /* silence was streamed too */
    return true;
}

static bool
test_sink_death_degrades(void)
{
    /* A sink that never reads and exits shortly after the liveness probe:
     * the mixer must degrade to a silent no-op state, not fail the host. */
    const char *sink_argv[] = { "/bin/sh", "-c", "exec sleep 0.3", NULL };
    pcmmix_options options;
    pcmmix_options_init(&options);
    options.sink_argv = sink_argv;

    pcmmix mixer;
    CHECK(pcmmix_start(&mixer, &options));
    CHECK(pcmmix_is_running(&mixer));
    CHECK(pcmmix_backend_name(&mixer) != NULL);

    static int16_t tone[512];
    fill_const(tone, 512, 6000);
    pcmmix_sample sample = { tone, 512 };
    (void)pcmmix_play(&mixer, &sample, 1.0f, 1.0f);

    for (int i = 0; i < 600 && pcmmix_is_running(&mixer); i++) sleep_ms(10);
    CHECK(!pcmmix_is_running(&mixer));
    CHECK(pcmmix_backend_name(&mixer) == NULL);
    CHECK(!pcmmix_is_enabled(&mixer));
    CHECK(pcmmix_play(&mixer, &sample, 1.0f, 1.0f) == -1);
    CHECK(pcmmix_loop(&mixer, &sample, 1.0f, 1.0f) == -1);
    pcmmix_music_play(&mixer, &sample, 1.0f, true); /* safe no-op */

    pcmmix_stop(&mixer);
    pcmmix_stop(&mixer); /* idempotent */
    return true;
}

static bool
test_failed_start_is_safe(void)
{
    /* A sink that exits inside the 80 ms liveness probe: start must fail
     * cleanly and every later call must be a safe no-op. */
    const char *sink_argv[] = { "/bin/sh", "-c", "exit 1", NULL };
    pcmmix_options options;
    pcmmix_options_init(&options);
    options.sink_argv = sink_argv;

    pcmmix mixer;
    CHECK(!pcmmix_start(&mixer, &options));
    CHECK(!pcmmix_is_running(&mixer));
    CHECK(pcmmix_backend_name(&mixer) == NULL);
    CHECK(!pcmmix_is_enabled(&mixer));

    static int16_t tone[64];
    fill_const(tone, 64, 6000);
    pcmmix_sample sample = { tone, 64 };
    CHECK(pcmmix_play(&mixer, &sample, 1.0f, 1.0f) == -1);
    CHECK(!pcmmix_voice_set(&mixer, 123, 1.0f, 1.0f));
    pcmmix_music_stop(&mixer, 1.0f);
    pcmmix_stop(&mixer);

    /* Out-of-range options are rejected up front. */
    pcmmix_options_init(&options);
    options.max_voices = PCMMIX_VOICES_MAX + 1u;
    options.offline = true;
    CHECK(!pcmmix_start(&mixer, &options));
    return true;
}

/* --- harness ------------------------------------------------------------ */

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"WAV round trip with extra chunks", test_wav_round_trip},
        {"WAV rejects malformed files", test_wav_rejects_malformed},
        {"resampler pitch and interpolation",
         test_resampler_pitch_and_interpolation},
        {"single voice reproduces sample", test_single_voice_reproduces_sample},
        {"voice sum and clipping", test_voice_sum_and_clipping},
        {"loop wrap and live voice_set", test_loop_wrap_and_live_voice_set},
        {"handle generations and stealing",
         test_handle_generations_and_stealing},
        {"music crossfade", test_music_crossfade},
        {"music stop fade", test_music_stop_fade},
        {"generator mixed under voices", test_generator_mixed_under_voices},
        {"sink capture integration", test_sink_capture},
        {"sink death degrades gracefully", test_sink_death_degrades},
        {"failed start is safe", test_failed_start_is_safe}
    };
    size_t passed = 0u;
    size_t index;

    for (index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
