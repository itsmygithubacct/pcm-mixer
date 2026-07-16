/* pcm-mixer: a small C11 software mixer for terminal games.
 *
 * Mixes 16-bit signed mono PCM voices (with linear-interpolation
 * resampling), a two-slot crossfading music layer, and an optional live
 * generator callback, then streams raw s16le down a pipe to a forked
 * command-line audio sink (pacat, pw-play, aplay, or sox play). The pipe is
 * shrunk so its capacity is the audio latency and the sink's blocking reads
 * clock the mixer thread. No sink, or the sink dying mid-run, degrades to a
 * silent, safe no-op state instead of failing the host application.
 *
 * Requires POSIX and pthreads; no other dependencies.
 */
#ifndef PCM_MIXER_H
#define PCM_MIXER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard capacity limits baked into the mixer object. */
#define PCMMIX_VOICES_MAX 32u
#define PCMMIX_MUSIC_SLOTS 2u

/* A caller-owned PCM clip: 16-bit signed mono frames at the mixer sample
 * rate. The mixer never copies or frees the data; it must stay valid until
 * no voice references it (the voice finished or was stopped, the music
 * layer faded it out, or pcmmix_stop() returned). */
typedef struct pcmmix_sample {
    const int16_t *frames;
    size_t frame_count;
} pcmmix_sample;

/* Block generator hook for live synthesis (engine drones, procedural
 * music). dst holds `frames` float samples, zero-filled on entry; write
 * samples in [-1, 1] (nominal) and they are added to the master mix.
 *
 * The callback runs on the mixer thread while the mixer lock is held: it
 * must be fast and lock-free, must not block, and must not call back into
 * the pcmmix API. */
typedef void (*pcmmix_generator_fn)(float *dst, size_t frames, void *user);

typedef struct pcmmix_options {
    uint32_t sample_rate; /* frames per second, 4000..192000; default 44100 */
    uint32_t max_voices;  /* 1..PCMMIX_VOICES_MAX; default 16 */
    uint32_t latency_ms;  /* transport latency target, 5..1000; default 24 */
    /* NULL-terminated argv for a custom sink executable that reads s16le
     * mono PCM from stdin until EOF. NULL selects the built-in probe
     * (pacat, pw-play, aplay, play). Read only during pcmmix_start(). */
    const char *const *sink_argv;
    bool soft_clip; /* tanh master-bus saturation; default true */
    /* Start without a sink or mixer thread; the caller pulls audio with
     * pcmmix_mix_block() (unit tests, offline rendering). Default false. */
    bool offline;
} pcmmix_options;

/* --- internal state: treat every field below as private ------------- */

typedef struct pcmmix_voice {
    const int16_t *data;
    size_t frame_count;
    float pos, step, vol;
    uint32_t generation;
    bool active, looping;
} pcmmix_voice;

typedef struct pcmmix_music_voice {
    const int16_t *data;
    size_t frame_count;
    double pos;
    float vol, target, fade_step;
    bool looping, active;
} pcmmix_music_voice;

typedef struct pcmmix {
    /* private: use the API below */
    uint32_t sample_rate, max_voices, latency_ms, block_frames;
    bool soft_clip, offline;
    pcmmix_voice voices[PCMMIX_VOICES_MAX];
    pcmmix_music_voice music[PCMMIX_MUSIC_SLOTS];
    pcmmix_generator_fn generator;
    void *generator_user;
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_started;
    bool started;
    bool running;
    bool enabled;
    bool pipe_paced;
    uint32_t voice_generation;
    int sink_fd;
    pid_t sink_pid;
    char backend[64];
    bool sink_alive;
} pcmmix;

/* --- lifecycle ------------------------------------------------------ */

/* Fill *options with the defaults documented above. */
void pcmmix_options_init(pcmmix_options *options);

/* Start the mixer. The mixer object may hold any prior contents; it is
 * fully (re)initialized. Pass NULL options for defaults. In transport mode
 * this ignores SIGPIPE process-wide, spawns the sink (each candidate gets
 * an ~80 ms liveness probe), and launches the mixer thread. Returns false
 * -- with the object left in a safe, stopped state -- when no sink
 * survives the probe, an option is out of range, or a resource limit is
 * hit. Must not be called on a mixer that is already started. */
bool pcmmix_start(pcmmix *mixer, const pcmmix_options *options);

/* Stop the mixer: joins the mixer thread, closes the pipe, and reaps the
 * sink (escalating to SIGTERM then SIGKILL if it will not exit). After
 * pcmmix_stop() returns, nothing touches the mixer object or any sample
 * memory again. Safe after a failed start, and idempotent. */
void pcmmix_stop(pcmmix *mixer);

/* True from a successful start until pcmmix_stop(), unless the sink died:
 * sink death winds the mixer down into a silent state where every call is
 * a safe no-op and this returns false. Offline mixers report true. */
bool pcmmix_is_running(pcmmix *mixer);

/* Executable name of the live sink ("pacat", "aplay", ...), or NULL when
 * the mixer is offline, stopped, never started, or the sink has died. */
const char *pcmmix_backend_name(pcmmix *mixer);

/* Master mute. Disabling silences and clears all voices and music
 * immediately, and rejects new play/loop/music calls until re-enabled.
 * Sink death also clears the enabled flag. */
void pcmmix_set_enabled(pcmmix *mixer, bool on);
bool pcmmix_is_enabled(pcmmix *mixer);

/* --- one-shots and held loops --------------------------------------- */

/* Play a one-shot. vol is a linear gain, clamped to 0..2; pitch is the
 * resampling ratio, clamped to 1/32..32 (values <= 0 mean 1.0). Returns a
 * voice handle (> 0) or -1 when the mixer is not running, is disabled, the
 * sample is invalid, or no slot could be claimed. When every slot is busy
 * the most-finished non-looping voice is stolen; looping voices are never
 * stolen. Samples longer than 2^24 frames are rejected -- use the music
 * layer for long material. */
int pcmmix_play(pcmmix *mixer, const pcmmix_sample *sample, float vol,
                float pitch);

/* Like pcmmix_play() but the voice loops seamlessly (last frame
 * interpolates back into the first) until pcmmix_voice_stop(). */
int pcmmix_loop(pcmmix *mixer, const pcmmix_sample *sample, float vol,
                float pitch);

/* Live-adjust a voice's gain and pitch (thrust/engine style control).
 * Handles are generation-checked: a stale handle whose slot has been
 * recycled is rejected. Returns false when the handle no longer names a
 * live voice. */
bool pcmmix_voice_set(pcmmix *mixer, int voice, float vol, float pitch);

/* Stop a voice immediately. Returns false for stale or finished handles. */
bool pcmmix_voice_stop(pcmmix *mixer, int voice);

/* True while the handle still names a live voice. */
bool pcmmix_voice_active(pcmmix *mixer, int voice);

/* --- music ----------------------------------------------------------- */

/* Play a music track at 1.0 pitch, automatically crossfading (0.5 s) from
 * whatever is currently playing. Calling it again with the sample that is
 * already playing just retargets its volume and loop flag. Tracks are
 * identified by their frames pointer. */
void pcmmix_music_play(pcmmix *mixer, const pcmmix_sample *sample, float vol,
                       bool loop);

/* Fade all music to silence over fade_seconds (<= 0 stops immediately). */
void pcmmix_music_stop(pcmmix *mixer, float fade_seconds);

/* Frames pointer of the track currently playing or fading in, or NULL. */
const int16_t *pcmmix_music_current(pcmmix *mixer);

/* --- generator hook --------------------------------------------------- */

/* Install (or clear, with fn == NULL) the generator hook. May be called
 * while the mixer is running; the swap is atomic with respect to mixing. */
void pcmmix_set_generator(pcmmix *mixer, pcmmix_generator_fn fn, void *user);

/* --- offline rendering ------------------------------------------------ */

/* Render `frames` frames of the master mix into dst, advancing all voice,
 * music, and generator state. This is the exact fill the transport thread
 * streams to the sink; call it yourself only on an offline mixer (unit
 * tests, bounce-to-disk), never while a mixer thread is live. */
void pcmmix_mix_block(pcmmix *mixer, int16_t *dst, size_t frames);

/* --- WAV loading ------------------------------------------------------ */

/* Strict WAV loader: PCM, mono, 16-bit, 44100 Hz only. Anything else fails
 * with a human-readable reason written to err (when err_len > 0). Walks
 * RIFF chunks (word-aligned, bounds-checked, 64 MiB file cap) rather than
 * assuming a canonical 44-byte header. Returns a malloc'd frame buffer to
 * release with pcmmix_wav_free(); *out_frames receives the frame count. */
int16_t *pcmmix_wav_load(const char *path, size_t *out_frames, char *err,
                         size_t err_len);

/* Release a buffer returned by pcmmix_wav_load(). NULL is allowed. */
void pcmmix_wav_free(int16_t *frames);

#ifdef __cplusplus
}
#endif

#endif /* PCM_MIXER_H */
