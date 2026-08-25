#include "pcmmix_asset.h"

#include <fluidsynth.h>
#include <sndfile.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum {
    ASSET_FILE_MAX = 256 * 1024 * 1024,
    SFZ_FILE_MAX = 1024 * 1024,
    SFZ_PATH_MAX = 2048,
    RENDER_BLOCK = 1024
};

typedef struct sfz_region {
    char sample[SFZ_PATH_MAX];
    int lokey;
    int hikey;
    int lovel;
    int hivel;
    int keycenter;
    bool have_sample;
    bool have_keycenter;
} sfz_region;

typedef enum sfz_section {
    SFZ_NONE = 0,
    SFZ_CONTROL,
    SFZ_GLOBAL,
    SFZ_GROUP,
    SFZ_REGION
} sfz_section;

static void asset_error(char *err, size_t err_len, const char *format, ...)
{
    va_list arguments;

    if (err == NULL || err_len == 0u) return;
    va_start(arguments, format);
    (void)vsnprintf(err, err_len, format, arguments);
    va_end(arguments);
}

static bool bounded_regular(const char *path, size_t maximum, size_t *size)
{
    struct stat status;

    if (path == NULL || lstat(path, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        (uintmax_t)status.st_size > (uintmax_t)maximum)
        return false;
    if (size != NULL) *size = (size_t)status.st_size;
    return true;
}

static bool ascii_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static const char *extension(const char *path)
{
    const char *slash;
    const char *dot;

    if (path == NULL) return "";
    slash = strrchr(path, '/');
    dot = strrchr(path, '.');
    return dot != NULL && (slash == NULL || dot > slash) ? dot : "";
}

void pcmmix_asset_options_init(pcmmix_asset_options *options)
{
    if (options == NULL) return;
    *options = (pcmmix_asset_options){
        .target_rate = 44100u,
        .max_seconds = 30u,
        .sf2_bank = 0u,
        .sf2_hold_ms = 1200u,
        .sf2_release_ms = 500u,
        .note = 60u,
        .velocity = 100u,
        .sf2_program = 0u,
        .sf2_channel = 0u
    };
}

pcmmix_asset_format pcmmix_asset_detect(const char *path)
{
    unsigned char header[12] = {0};
    FILE *file;
    size_t count;
    const char *suffix;

    if (!bounded_regular(path, ASSET_FILE_MAX, NULL))
        return PCMMIX_ASSET_UNKNOWN;
    file = fopen(path, "rb");
    if (file == NULL) return PCMMIX_ASSET_UNKNOWN;
    count = fread(header, 1u, sizeof header, file);
    (void)fclose(file);
    if (count >= 12u && memcmp(header, "RIFF", 4u) == 0) {
        if (memcmp(header + 8u, "WAVE", 4u) == 0) return PCMMIX_ASSET_WAV;
        if (memcmp(header + 8u, "sfbk", 4u) == 0) return PCMMIX_ASSET_SF2;
    }
    if (count >= 4u && memcmp(header, "OggS", 4u) == 0)
        return PCMMIX_ASSET_OGG;
    suffix = extension(path);
    if (ascii_equal(suffix, ".sfz")) return PCMMIX_ASSET_SFZ;
    if (ascii_equal(suffix, ".sf2")) return PCMMIX_ASSET_SF2;
    if (ascii_equal(suffix, ".ogg") || ascii_equal(suffix, ".oga"))
        return PCMMIX_ASSET_OGG;
    if (ascii_equal(suffix, ".wav")) return PCMMIX_ASSET_WAV;
    return PCMMIX_ASSET_UNKNOWN;
}

const char *pcmmix_asset_format_name(pcmmix_asset_format format)
{
    switch (format) {
    case PCMMIX_ASSET_WAV: return "WAV";
    case PCMMIX_ASSET_OGG: return "OGG";
    case PCMMIX_ASSET_SFZ: return "SFZ";
    case PCMMIX_ASSET_SF2: return "SF2";
    default: return "unknown";
    }
}

static bool valid_options(const pcmmix_asset_options *options)
{
    uint64_t rendered_ms;

    if (options == NULL || options->target_rate < 8000u ||
        options->target_rate > 192000u || options->max_seconds == 0u ||
        options->max_seconds > 60u || options->note > 127u ||
        options->velocity == 0u || options->velocity > 127u ||
        options->sf2_program > 127u || options->sf2_channel > 15u ||
        options->sf2_hold_ms == 0u)
        return false;
    rendered_ms = (uint64_t)options->sf2_hold_ms +
                  (uint64_t)options->sf2_release_ms;
    return rendered_ms <= (uint64_t)options->max_seconds * UINT64_C(1000);
}

static int16_t interpolate(int16_t left, int16_t right, uint32_t numerator,
                           uint32_t denominator)
{
    int64_t value = (int64_t)left * (denominator - numerator) +
                    (int64_t)right * numerator;

    value += value >= 0 ? (int64_t)denominator / 2 :
                          -(int64_t)denominator / 2;
    value /= denominator;
    if (value > INT16_MAX) value = INT16_MAX;
    if (value < INT16_MIN) value = INT16_MIN;
    return (int16_t)value;
}

static int16_t *resample_mono(int16_t *input, size_t input_count,
                              uint32_t input_rate, uint32_t output_rate,
                              size_t *output_count)
{
    size_t count;
    int16_t *output;

    if (input_rate == output_rate) {
        *output_count = input_count;
        return input;
    }
    if (input_count > (SIZE_MAX - output_rate / 2u) / output_rate) {
        free(input);
        return NULL;
    }
    count = (input_count * output_rate + input_rate / 2u) / input_rate;
    if (count == 0u || count > SIZE_MAX / sizeof output[0]) {
        free(input);
        return NULL;
    }
    output = malloc(count * sizeof output[0]);
    if (output == NULL) {
        free(input);
        return NULL;
    }
    for (size_t index = 0u; index < count; ++index) {
        const uint64_t position = (uint64_t)index * input_rate;
        const size_t source = (size_t)(position / output_rate);
        const uint32_t fraction = (uint32_t)(position % output_rate);

        if (source >= input_count - 1u)
            output[index] = input[input_count - 1u];
        else
            output[index] = interpolate(input[source], input[source + 1u],
                                        fraction, output_rate);
    }
    free(input);
    *output_count = count;
    return output;
}

static int16_t *load_sndfile(const char *path,
                             const pcmmix_asset_options *options,
                             size_t *out_frames, char *err, size_t err_len)
{
    SF_INFO info = {0};
    SNDFILE *file;
    int16_t *interleaved = NULL;
    int16_t *mono = NULL;
    size_t frames;
    size_t values;
    sf_count_t decoded;

    if (!bounded_regular(path, ASSET_FILE_MAX, NULL)) {
        asset_error(err, err_len, "missing, linked, or oversized asset: %s",
                    path);
        return NULL;
    }
    file = sf_open(path, SFM_READ, &info);
    if (file == NULL) {
        asset_error(err, err_len, "cannot decode %s: %s", path, sf_strerror(NULL));
        return NULL;
    }
#define SNDFILE_FAIL(...)                                                     \
    do {                                                                      \
        asset_error(err, err_len, __VA_ARGS__);                               \
        free(interleaved);                                                    \
        free(mono);                                                           \
        (void)sf_close(file);                                                 \
        return NULL;                                                          \
    } while (0)
    if (info.frames <= 0 || info.samplerate < 8000 ||
        info.samplerate > 192000 || info.channels < 1 || info.channels > 8)
        SNDFILE_FAIL("unsupported stream geometry in %s", path);
    if ((uint64_t)info.frames >
        (uint64_t)(unsigned int)info.samplerate * options->max_seconds)
        SNDFILE_FAIL("asset exceeds %u seconds: %s", options->max_seconds,
                     path);
    frames = (size_t)info.frames;
    if (frames > SIZE_MAX / (size_t)info.channels)
        SNDFILE_FAIL("asset geometry overflows: %s", path);
    values = frames * (size_t)info.channels;
    if (values > SIZE_MAX / sizeof interleaved[0])
        SNDFILE_FAIL("asset allocation overflows: %s", path);
    interleaved = malloc(values * sizeof interleaved[0]);
    mono = malloc(frames * sizeof mono[0]);
    if (interleaved == NULL || mono == NULL)
        SNDFILE_FAIL("out of memory decoding %s", path);
    decoded = sf_readf_short(file, interleaved, info.frames);
    if (decoded != info.frames)
        SNDFILE_FAIL("short decoded stream in %s", path);
    if (sf_close(file) != 0) {
        file = NULL;
        free(interleaved);
        free(mono);
        asset_error(err, err_len, "cannot close decoded asset: %s", path);
        return NULL;
    }
    file = NULL;
    for (size_t frame = 0u; frame < frames; ++frame) {
        int64_t total = 0;
        for (int channel = 0; channel < info.channels; ++channel)
            total += interleaved[frame * (size_t)info.channels +
                                 (size_t)channel];
        total /= info.channels;
        if (total > INT16_MAX) total = INT16_MAX;
        if (total < INT16_MIN) total = INT16_MIN;
        mono[frame] = (int16_t)total;
    }
    free(interleaved);
    mono = resample_mono(mono, frames, (uint32_t)info.samplerate,
                         options->target_rate, out_frames);
    if (mono == NULL)
        asset_error(err, err_len, "cannot resample %s", path);
    return mono;
#undef SNDFILE_FAIL
}

static void sfz_region_init(sfz_region *region)
{
    *region = (sfz_region){
        .lokey = 0,
        .hikey = 127,
        .lovel = 0,
        .hivel = 127,
        .keycenter = 60
    };
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

static bool parse_number(const char *text, int minimum, int maximum, int *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    while (end != NULL && isspace((unsigned char)*end)) ++end;
    if (errno != 0 || end == text || end == NULL || *end != '\0' ||
        value < minimum || value > maximum)
        return false;
    *out = (int)value;
    return true;
}

static bool sfz_opcode(sfz_section section, sfz_region *group,
                       sfz_region *region, char default_path[SFZ_PATH_MAX],
                       const char *name, const char *value)
{
    sfz_region *target = section == SFZ_REGION ? region : group;
    int parsed;

    if (section == SFZ_CONTROL && strcmp(name, "default_path") == 0) {
        if (strlen(value) >= SFZ_PATH_MAX) return false;
        (void)snprintf(default_path, SFZ_PATH_MAX, "%s", value);
        return true;
    }
    if (section != SFZ_GROUP && section != SFZ_GLOBAL &&
        section != SFZ_REGION)
        return true;
    if (strcmp(name, "sample") == 0) {
        if (strlen(value) >= sizeof target->sample) return false;
        (void)snprintf(target->sample, sizeof target->sample, "%s", value);
        target->have_sample = true;
        return true;
    }
    if (strcmp(name, "key") == 0) {
        if (!parse_number(value, 0, 127, &parsed)) return false;
        target->lokey = parsed;
        target->hikey = parsed;
        target->keycenter = parsed;
        target->have_keycenter = true;
        return true;
    }
    if (strcmp(name, "lokey") == 0)
        return parse_number(value, 0, 127, &target->lokey);
    if (strcmp(name, "hikey") == 0)
        return parse_number(value, 0, 127, &target->hikey);
    if (strcmp(name, "lovel") == 0)
        return parse_number(value, 0, 127, &target->lovel);
    if (strcmp(name, "hivel") == 0)
        return parse_number(value, 0, 127, &target->hivel);
    if (strcmp(name, "pitch_keycenter") == 0) {
        if (!parse_number(value, 0, 127, &parsed)) return false;
        target->keycenter = parsed;
        target->have_keycenter = true;
    }
    return true;
}

static bool sfz_matches(const sfz_region *region,
                        const pcmmix_asset_options *options)
{
    return region->have_sample && region->lokey <= options->note &&
           region->hikey >= options->note &&
           region->lovel <= options->velocity &&
           region->hivel >= options->velocity;
}

static bool safe_relative_path(char *path)
{
    char *component = path;

    if (path[0] == '/' || path[0] == '\0' ||
        (isalpha((unsigned char)path[0]) && path[1] == ':'))
        return false;
    for (char *cursor = path; *cursor != '\0'; ++cursor)
        if (*cursor == '\\') *cursor = '/';
    while (*component != '\0') {
        char *slash = strchr(component, '/');
        if (slash != NULL) *slash = '\0';
        if (strcmp(component, "..") == 0 || component[0] == '\0') {
            if (slash != NULL) *slash = '/';
            return false;
        }
        if (slash == NULL) break;
        *slash = '/';
        component = slash + 1;
    }
    return true;
}

static bool sfz_find_sample(const char *path,
                            const pcmmix_asset_options *options,
                            char output[SFZ_PATH_MAX], uint8_t *root_note,
                            char *err, size_t err_len)
{
    FILE *file;
    char line[4096];
    char default_path[SFZ_PATH_MAX] = "";
    sfz_region group;
    sfz_region region;
    sfz_region chosen;
    sfz_section section = SFZ_NONE;
    bool have_region = false;
    bool found = false;

    if (!bounded_regular(path, SFZ_FILE_MAX, NULL)) {
        asset_error(err, err_len, "missing, linked, or oversized SFZ: %s", path);
        return false;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        asset_error(err, err_len, "cannot open SFZ: %s", path);
        return false;
    }
    sfz_region_init(&group);
    sfz_region_init(&region);
    sfz_region_init(&chosen);
    while (!found && fgets(line, sizeof line, file) != NULL) {
        char *comment = strstr(line, "//");
        char *cursor;

        if (comment != NULL) *comment = '\0';
        cursor = trim(line);
        if (*cursor == '\0') continue;
        if (*cursor == '<') {
            char *close = strchr(cursor, '>');
            if (close == NULL) {
                asset_error(err, err_len, "unterminated SFZ section in %s", path);
                (void)fclose(file);
                return false;
            }
            if (have_region && sfz_matches(&region, options)) {
                chosen = region;
                found = true;
                break;
            }
            *close = '\0';
            if (strcmp(cursor + 1, "control") == 0)
                section = SFZ_CONTROL;
            else if (strcmp(cursor + 1, "global") == 0) {
                section = SFZ_GLOBAL;
                sfz_region_init(&group);
            } else if (strcmp(cursor + 1, "group") == 0) {
                section = SFZ_GROUP;
            } else if (strcmp(cursor + 1, "region") == 0) {
                section = SFZ_REGION;
                region = group;
                have_region = true;
            } else {
                section = SFZ_NONE;
            }
            cursor = trim(close + 1);
        }
        while (!found && *cursor != '\0') {
            char *equals;
            char *name;
            char *value;
            char *next = NULL;

            while (isspace((unsigned char)*cursor)) ++cursor;
            if (*cursor == '\0') break;
            equals = strchr(cursor, '=');
            if (equals == NULL) break;
            *equals = '\0';
            name = trim(cursor);
            value = equals + 1;
            for (char *scan = value; *scan != '\0'; ++scan) {
                if (!isspace((unsigned char)*scan)) continue;
                char *candidate = scan;
                while (isspace((unsigned char)*candidate)) ++candidate;
                char *candidate_equals = strchr(candidate, '=');
                char *candidate_space = strpbrk(candidate, " \t\r\n");
                if (candidate_equals != NULL &&
                    (candidate_space == NULL || candidate_equals < candidate_space)) {
                    next = candidate;
                    *scan = '\0';
                    break;
                }
            }
            value = trim(value);
            if (*name == '\0' || *value == '\0' ||
                !sfz_opcode(section, &group, &region, default_path,
                            name, value)) {
                asset_error(err, err_len, "invalid SFZ opcode in %s", path);
                (void)fclose(file);
                return false;
            }
            cursor = next != NULL ? next : value + strlen(value);
        }
    }
    if (!found && have_region && sfz_matches(&region, options)) {
        chosen = region;
        found = true;
    }
    if (ferror(file) != 0 || fclose(file) != 0) {
        asset_error(err, err_len, "cannot read SFZ: %s", path);
        return false;
    }
    if (!found) {
        asset_error(err, err_len, "no SFZ region matches note %u velocity %u",
                    options->note, options->velocity);
        return false;
    }
    char relative[SFZ_PATH_MAX];
    int count = snprintf(relative, sizeof relative, "%s%s", default_path,
                         chosen.sample);
    if (count < 0 || (size_t)count >= sizeof relative ||
        !safe_relative_path(relative)) {
        asset_error(err, err_len, "unsafe SFZ sample path in %s", path);
        return false;
    }
    const char *slash = strrchr(path, '/');
    if (slash == NULL)
        count = snprintf(output, SFZ_PATH_MAX, "%s", relative);
    else
        count = snprintf(output, SFZ_PATH_MAX, "%.*s/%s",
                         (int)(slash - path), path, relative);
    if (count < 0 || count >= SFZ_PATH_MAX) {
        asset_error(err, err_len, "resolved SFZ path is too long: %s", path);
        return false;
    }
    *root_note = (uint8_t)(chosen.have_keycenter ? chosen.keycenter :
                           options->note);
    return true;
}

static int16_t *load_sfz(const char *path,
                         const pcmmix_asset_options *options,
                         size_t *out_frames, uint8_t *root_note,
                         char *err, size_t err_len)
{
    char sample[SFZ_PATH_MAX];

    if (!sfz_find_sample(path, options, sample, root_note, err, err_len))
        return NULL;
    pcmmix_asset_format format = pcmmix_asset_detect(sample);
    if (format != PCMMIX_ASSET_WAV && format != PCMMIX_ASSET_OGG) {
        asset_error(err, err_len, "SFZ sample is not WAV or OGG: %s", sample);
        return NULL;
    }
    return load_sndfile(sample, options, out_frames, err, err_len);
}

static int16_t *load_sf2(const char *path,
                         const pcmmix_asset_options *options,
                         size_t *out_frames, char *err, size_t err_len)
{
    fluid_settings_t *settings = NULL;
    fluid_synth_t *synth = NULL;
    int16_t *output = NULL;
    int16_t left[RENDER_BLOCK];
    int16_t right[RENDER_BLOCK];
    size_t hold_frames;
    size_t release_frames;
    size_t total_frames;
    int soundfont;

    if (!bounded_regular(path, ASSET_FILE_MAX, NULL)) {
        asset_error(err, err_len, "missing, linked, or oversized SF2: %s", path);
        return NULL;
    }
    hold_frames = ((size_t)options->target_rate * options->sf2_hold_ms +
                   999u) / 1000u;
    release_frames = ((size_t)options->target_rate * options->sf2_release_ms +
                      999u) / 1000u;
    if (hold_frames > SIZE_MAX - release_frames ||
        hold_frames + release_frames > SIZE_MAX / sizeof output[0]) {
        asset_error(err, err_len, "SF2 render size overflows: %s", path);
        return NULL;
    }
    total_frames = hold_frames + release_frames;
    output = malloc(total_frames * sizeof output[0]);
    settings = new_fluid_settings();
    if (output == NULL || settings == NULL) {
        asset_error(err, err_len, "out of memory preparing SF2: %s", path);
        goto fail;
    }
    (void)fluid_settings_setnum(settings, "synth.sample-rate",
                                (double)options->target_rate);
    (void)fluid_settings_setint(settings, "synth.chorus.active", 0);
    (void)fluid_settings_setint(settings, "synth.reverb.active", 0);
    (void)fluid_settings_setnum(settings, "synth.gain", 0.5);
    synth = new_fluid_synth(settings);
    if (synth == NULL) {
        asset_error(err, err_len, "cannot create SF2 renderer");
        goto fail;
    }
    soundfont = fluid_synth_sfload(synth, path, 1);
    if (soundfont == FLUID_FAILED ||
        fluid_synth_program_select(synth, options->sf2_channel, soundfont,
                                   options->sf2_bank,
                                   options->sf2_program) == FLUID_FAILED ||
        fluid_synth_noteon(synth, options->sf2_channel, options->note,
                           options->velocity) == FLUID_FAILED) {
        asset_error(err, err_len,
                    "SF2 bank %u program %u cannot play note %u: %s",
                    options->sf2_bank, options->sf2_program, options->note,
                    path);
        goto fail;
    }
    size_t position = 0u;
    while (position < hold_frames) {
        size_t take = hold_frames - position;
        if (take > RENDER_BLOCK) take = RENDER_BLOCK;
        if (fluid_synth_write_s16(synth, (int)take, left, 0, 1,
                                  right, 0, 1) == FLUID_FAILED) {
            asset_error(err, err_len, "SF2 render failed: %s", path);
            goto fail;
        }
        for (size_t index = 0u; index < take; ++index)
            output[position + index] = (int16_t)(((int32_t)left[index] +
                                                  right[index]) / 2);
        position += take;
    }
    (void)fluid_synth_noteoff(synth, options->sf2_channel, options->note);
    while (position < total_frames) {
        size_t take = total_frames - position;
        if (take > RENDER_BLOCK) take = RENDER_BLOCK;
        if (fluid_synth_write_s16(synth, (int)take, left, 0, 1,
                                  right, 0, 1) == FLUID_FAILED) {
            asset_error(err, err_len, "SF2 release render failed: %s", path);
            goto fail;
        }
        for (size_t index = 0u; index < take; ++index)
            output[position + index] = (int16_t)(((int32_t)left[index] +
                                                  right[index]) / 2);
        position += take;
    }
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
    *out_frames = total_frames;
    return output;

fail:
    if (synth != NULL) delete_fluid_synth(synth);
    if (settings != NULL) delete_fluid_settings(settings);
    free(output);
    return NULL;
}

int16_t *pcmmix_asset_load(const char *path,
                           const pcmmix_asset_options *requested,
                           size_t *out_frames, uint8_t *out_root_note,
                           char *err, size_t err_len)
{
    pcmmix_asset_options defaults;
    const pcmmix_asset_options *options = requested;
    pcmmix_asset_format format;
    int16_t *frames = NULL;

    if (out_frames != NULL) *out_frames = 0u;
    if (out_root_note != NULL) *out_root_note = 0u;
    if (err != NULL && err_len != 0u) err[0] = '\0';
    if (options == NULL) {
        pcmmix_asset_options_init(&defaults);
        options = &defaults;
    }
    if (path == NULL || out_frames == NULL || out_root_note == NULL ||
        !valid_options(options)) {
        asset_error(err, err_len, "invalid audio asset arguments");
        return NULL;
    }
    format = pcmmix_asset_detect(path);
    if (format == PCMMIX_ASSET_WAV || format == PCMMIX_ASSET_OGG) {
        frames = load_sndfile(path, options, out_frames, err, err_len);
        *out_root_note = options->note;
    } else if (format == PCMMIX_ASSET_SFZ) {
        frames = load_sfz(path, options, out_frames, out_root_note,
                          err, err_len);
    } else if (format == PCMMIX_ASSET_SF2) {
        frames = load_sf2(path, options, out_frames, err, err_len);
        *out_root_note = options->note;
    } else {
        asset_error(err, err_len, "unsupported audio asset: %s", path);
    }
    if (frames == NULL) {
        *out_frames = 0u;
        *out_root_note = 0u;
    }
    return frames;
}

void pcmmix_asset_free(int16_t *frames)
{
    free(frames);
}
