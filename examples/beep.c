/* beep: synthesize a short envelope-swept tone and play it through the
 * default sink probe.
 *
 * NOTE: unlike the test suite, this example produces audible output on the
 * default audio device. The tiny synth lives here in the example; the
 * library itself only mixes PCM it is handed. */
#include "pcm_mixer.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BEEP_RATE 44100u

static int16_t *synth_beep(size_t *out_frames);
static void sleep_ms(long milliseconds);

static int16_t *
synth_beep(size_t *out_frames)
{
    const float duration = 0.45f;
    const size_t frames = (size_t)((float)BEEP_RATE * duration);
    int16_t *buf = malloc(frames * sizeof *buf);
    if (!buf) return NULL;

    float phase = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        float t = (float)i / (float)BEEP_RATE;
        float sweep = 880.0f + 520.0f * (t / duration); /* rising sweep */
        float env = expf(-t / (duration * 0.30f));      /* pluck decay */
        if (i < 64) env *= (float)i / 64.0f;            /* de-click */
        phase += 6.2831853f * sweep / (float)BEEP_RATE;
        buf[i] = (int16_t)lrintf(sinf(phase) * env * 0.35f * 32767.0f);
    }
    *out_frames = frames;
    return buf;
}

static void
sleep_ms(long milliseconds)
{
    struct timespec ts = { milliseconds / 1000,
                           (milliseconds % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

int
main(void)
{
    pcmmix mixer;
    pcmmix_options options;
    pcmmix_options_init(&options);

    if (!pcmmix_start(&mixer, &options)) {
        (void)fprintf(stderr,
                      "beep: no audio sink found "
                      "(tried pacat, pw-play, aplay, play)\n");
        return 1;
    }
    (void)printf("backend: %s\n", pcmmix_backend_name(&mixer));

    size_t frames = 0;
    int16_t *tone = synth_beep(&frames);
    if (!tone) {
        pcmmix_stop(&mixer);
        return 1;
    }
    pcmmix_sample sample = { tone, frames };

    int voice = pcmmix_play(&mixer, &sample, 0.9f, 1.0f);
    while (voice > 0 && pcmmix_voice_active(&mixer, voice)) sleep_ms(10);
    sleep_ms(150); /* let the sink drain its own buffer */

    pcmmix_stop(&mixer);
    free(tone);
    return 0;
}
