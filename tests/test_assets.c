#include "pcmmix_asset.h"

#include <sndfile.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",             \
                          __FILE__, __LINE__, #condition);                     \
            return false;                                                     \
        }                                                                     \
    } while (false)

static bool write_audio(const char *path, int format, int sample_rate,
                        int channels, size_t frames)
{
    SF_INFO info = {
        .samplerate = sample_rate,
        .channels = channels,
        .format = format
    };
    SNDFILE *file;
    int16_t *samples;
    bool ok;

    if (frames > SIZE_MAX / (size_t)channels ||
        frames * (size_t)channels > SIZE_MAX / sizeof samples[0])
        return false;
    samples = malloc(frames * (size_t)channels * sizeof samples[0]);
    if (samples == NULL) return false;
    for (size_t frame = 0u; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            const int value = frame % 32u < 16u ? 12000 : -12000;
            samples[frame * (size_t)channels + (size_t)channel] =
                (int16_t)(channel == 0 ? value : value / 2);
        }
    }
    file = sf_open(path, SFM_WRITE, &info);
    if (file == NULL) {
        free(samples);
        return false;
    }
    ok = sf_writef_short(file, samples, (sf_count_t)frames) ==
         (sf_count_t)frames;
    ok = sf_close(file) == 0 && ok;
    free(samples);
    return ok;
}

static bool write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    const size_t length = strlen(text);
    bool ok;

    if (file == NULL) return false;
    ok = fwrite(text, 1u, length, file) == length;
    return fclose(file) == 0 && ok;
}

static bool any_signal(const int16_t *samples, size_t frames)
{
    for (size_t index = 0u; index < frames; ++index)
        if (samples[index] != 0) return true;
    return false;
}

static bool test_detection_and_defaults(const char *directory)
{
    char wav_path[512];
    char disguised_path[512];
    pcmmix_asset_options options;

    (void)snprintf(wav_path, sizeof wav_path, "%s/tone.wav", directory);
    (void)snprintf(disguised_path, sizeof disguised_path, "%s/tone.bin",
                   directory);
    CHECK(write_audio(wav_path, SF_FORMAT_WAV | SF_FORMAT_PCM_16,
                      22050, 2, 2205u));
    CHECK(link(wav_path, disguised_path) == 0);
    CHECK(pcmmix_asset_detect(wav_path) == PCMMIX_ASSET_WAV);
    CHECK(pcmmix_asset_detect(disguised_path) == PCMMIX_ASSET_WAV);
    CHECK(strcmp(pcmmix_asset_format_name(PCMMIX_ASSET_SF2), "SF2") == 0);
    pcmmix_asset_options_init(&options);
    CHECK(options.target_rate == 44100u);
    CHECK(options.note == 60u);
    CHECK(unlink(disguised_path) == 0);
    return true;
}

static bool test_wav_and_ogg_decode(const char *directory)
{
    char wav_path[512];
    char ogg_path[512];
    char error[256] = "";
    pcmmix_asset_options options;
    int16_t *samples;
    size_t frames = 0u;
    uint8_t root = 0u;

    (void)snprintf(wav_path, sizeof wav_path, "%s/tone.wav", directory);
    (void)snprintf(ogg_path, sizeof ogg_path, "%s/tone.ogg", directory);
    CHECK(write_audio(ogg_path, SF_FORMAT_OGG | SF_FORMAT_VORBIS,
                      44100, 1, 1024u));
    pcmmix_asset_options_init(&options);
    options.note = 48u;
    samples = pcmmix_asset_load(wav_path, &options, &frames, &root,
                                error, sizeof error);
    CHECK(samples != NULL);
    CHECK(frames == 4410u);
    CHECK(root == 48u);
    CHECK(any_signal(samples, frames));
    pcmmix_asset_free(samples);

    samples = pcmmix_asset_load(ogg_path, &options, &frames, &root,
                                error, sizeof error);
    CHECK(samples != NULL);
    CHECK(frames == 1024u);
    CHECK(root == 48u);
    CHECK(any_signal(samples, frames));
    pcmmix_asset_free(samples);
    return true;
}

static bool test_sfz_selection_and_path_policy(const char *directory)
{
    char samples_directory[512];
    char ogg_path[512];
    char sfz_path[512];
    char unsafe_path[512];
    char error[256] = "";
    pcmmix_asset_options options;
    int16_t *samples;
    size_t frames = 0u;
    uint8_t root = 0u;

    (void)snprintf(samples_directory, sizeof samples_directory, "%s/samples",
                   directory);
    (void)snprintf(ogg_path, sizeof ogg_path, "%s/samples/tone.ogg",
                   directory);
    (void)snprintf(sfz_path, sizeof sfz_path, "%s/instrument.sfz", directory);
    (void)snprintf(unsafe_path, sizeof unsafe_path, "%s/unsafe.sfz", directory);
    CHECK(mkdir(samples_directory, 0700) == 0);
    CHECK(write_audio(ogg_path, SF_FORMAT_OGG | SF_FORMAT_VORBIS,
                      32000, 1, 640u));
    CHECK(write_text(sfz_path,
                     "<control> default_path=samples\\\n"
                     "<group> lovel=1 hivel=100\n"
                     "<region> sample=tone.ogg lokey=36 hikey=72 "
                     "pitch_keycenter=55\n"));
    pcmmix_asset_options_init(&options);
    options.note = 60u;
    options.velocity = 90u;
    samples = pcmmix_asset_load(sfz_path, &options, &frames, &root,
                                error, sizeof error);
    CHECK(samples != NULL);
    CHECK(frames == 882u);
    CHECK(root == 55u);
    CHECK(any_signal(samples, frames));
    pcmmix_asset_free(samples);

    CHECK(write_text(unsafe_path,
                     "<region> sample=../outside.wav key=60\n"));
    samples = pcmmix_asset_load(unsafe_path, &options, &frames, &root,
                                error, sizeof error);
    CHECK(samples == NULL);
    CHECK(strstr(error, "unsafe SFZ sample path") != NULL);
    CHECK(frames == 0u && root == 0u);
    return true;
}

static bool test_sf2_render_if_available(void)
{
    const char *path = getenv("PCMMIX_TEST_SF2");
    struct stat status;
    pcmmix_asset_options options;
    char error[256] = "";
    int16_t *samples;
    size_t frames = 0u;
    uint8_t root = 0u;

    if (path == NULL || path[0] == '\0')
        path = "/usr/share/sounds/sf2/FluidR3_GM.sf2";
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
        (void)printf("ok - SF2 render (skipped: no fixture)\n");
        return true;
    }
    pcmmix_asset_options_init(&options);
    options.note = 60u;
    options.sf2_hold_ms = 80u;
    options.sf2_release_ms = 40u;
    samples = pcmmix_asset_load(path, &options, &frames, &root,
                                error, sizeof error);
    CHECK(samples != NULL);
    CHECK(frames == 5292u);
    CHECK(root == 60u);
    CHECK(any_signal(samples, frames));
    pcmmix_asset_free(samples);
    (void)printf("ok - SF2 render\n");
    return true;
}

int main(void)
{
    char directory[] = "/tmp/pcmmix-assets-XXXXXX";
    char path[512];

    CHECK(mkdtemp(directory) != NULL);
    CHECK(test_detection_and_defaults(directory));
    (void)printf("ok - detection and defaults\n");
    CHECK(test_wav_and_ogg_decode(directory));
    (void)printf("ok - WAV and OGG decode\n");
    CHECK(test_sfz_selection_and_path_policy(directory));
    (void)printf("ok - SFZ selection and path policy\n");
    CHECK(test_sf2_render_if_available());

    (void)snprintf(path, sizeof path, "%s/samples/tone.ogg", directory);
    CHECK(unlink(path) == 0);
    (void)snprintf(path, sizeof path, "%s/samples", directory);
    CHECK(rmdir(path) == 0);
    const char *files[] = {"tone.wav", "tone.ogg", "instrument.sfz",
                           "unsafe.sfz"};
    for (size_t index = 0u; index < sizeof files / sizeof files[0]; ++index) {
        (void)snprintf(path, sizeof path, "%s/%s", directory, files[index]);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(directory) == 0);
    (void)printf("4 audio asset tests passed\n");
    return 0;
}
