/* pcm-mixer core: voice mixing, music crossfade, generator hook, and the
 * pipe-to-CLI-sink transport. */
#define _GNU_SOURCE /* F_SETPIPE_SZ on Linux; ignored elsewhere */

#include "pcm_mixer.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { CHUNK_CAP = 1024 };

/* Voice handles are generation * PCMMIX_VOICES_MAX + slot; the generation
 * counter wraps below INT_MAX / PCMMIX_VOICES_MAX so handles stay > 0. */
#define GENERATION_MAX 0x3FFFFFu
#define SAMPLE_FRAMES_MAX (1u << 24) /* float resampler precision bound */
#define MUSIC_FADE_SECONDS 0.5f
#define SINK_PROBE_MS 80
#define SINK_STALL_LIMIT 50 /* x 100 ms select timeout = ~5 s wedged */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float sanitize_pitch(float pitch)
{
    if (pitch <= 0.0f) return 1.0f;
    return clampf(pitch, 1.0f / 32.0f, 32.0f);
}

static void sleep_msec(long milliseconds)
{
    struct timespec ts = { milliseconds / 1000,
                           (milliseconds % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* --- mixing (mixer lock held) ---------------------------------------- */

static void mix_voice(pcmmix_voice *v, float *mix, size_t frames)
{
    if (!v->active) return;
    if (!v->data || v->frame_count == 0) {
        v->active = false;
        return;
    }
    const size_t count = v->frame_count;
    const float length = (float)count;
    for (size_t i = 0; i < frames; i++) {
        if (v->pos >= length) {
            if (!v->looping) {
                v->active = false;
                break;
            }
            v->pos = fmodf(v->pos, length);
        }
        size_t ip = (size_t)v->pos;
        if (ip >= count) ip = count - 1; /* float rounding edge */
        size_t ip2 = ip + 1;
        if (ip2 >= count) ip2 = v->looping ? 0 : ip;
        float frac = v->pos - (float)ip;
        float a = (float)v->data[ip] * (1.0f / 32768.0f);
        float b = (float)v->data[ip2] * (1.0f / 32768.0f);
        mix[i] += (a + (b - a) * frac) * v->vol;
        v->pos += v->step;
    }
}

static void mix_music(pcmmix_music_voice *mv, float *mix, size_t frames)
{
    if (!mv->active) return;
    if (!mv->data || mv->frame_count == 0) {
        mv->active = false;
        return;
    }
    for (size_t i = 0; i < frames; i++) {
        if (mv->vol < mv->target) {
            mv->vol += mv->fade_step;
            if (mv->vol > mv->target) mv->vol = mv->target;
        } else if (mv->vol > mv->target) {
            mv->vol -= mv->fade_step;
            if (mv->vol < mv->target) mv->vol = mv->target;
        }
        if (mv->vol <= 0.0f && mv->target <= 0.0f) {
            mv->active = false;
            return;
        }
        size_t ip = (size_t)mv->pos;
        if (ip >= mv->frame_count) {
            if (!mv->looping) {
                mv->active = false;
                return;
            }
            mv->pos -= (double)mv->frame_count;
            ip = (size_t)mv->pos;
            if (ip >= mv->frame_count) ip = 0;
        }
        mix[i] += ((float)mv->data[ip] * (1.0f / 32768.0f)) * mv->vol;
        mv->pos += 1.0;
    }
}

static void mix_chunk_locked(pcmmix *m, int16_t *dst, size_t frames)
{
    float mix[CHUNK_CAP];
    memset(mix, 0, frames * sizeof mix[0]);
    if (m->enabled) {
        for (uint32_t i = 0; i < m->max_voices; i++)
            mix_voice(&m->voices[i], mix, frames);
        for (uint32_t i = 0; i < PCMMIX_MUSIC_SLOTS; i++)
            mix_music(&m->music[i], mix, frames);
        if (m->generator) {
            float gen[CHUNK_CAP];
            memset(gen, 0, frames * sizeof gen[0]);
            m->generator(gen, frames, m->generator_user);
            for (size_t i = 0; i < frames; i++) mix[i] += gen[i];
        }
    }
    for (size_t i = 0; i < frames; i++) {
        float v = m->soft_clip ? tanhf(mix[i]) : mix[i];
        v = clampf(v, -1.0f, 1.0f);
        dst[i] = (int16_t)lrintf(v * 32767.0f);
    }
}

static void silence_all_locked(pcmmix *m)
{
    memset(m->voices, 0, sizeof m->voices);
    memset(m->music, 0, sizeof m->music);
}

/* --- sink discovery and transport ------------------------------------ */

static bool in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) return false;
    char *copy = strdup(path);
    if (!copy) return false;
    bool found = false;
    for (char *p = copy, *tok; (tok = strsep(&p, ":")) != NULL;) {
        if (!*tok) tok = ".";
        char full[512];
        if (snprintf(full, sizeof full, "%s/%s", tok, name) <
                (int)sizeof full &&
            access(full, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(copy);
    return found;
}

/* Reap a sink that should be exiting; escalate if it will not. Returns
 * once the child no longer exists. */
static bool wait_for_exit(pid_t pid, long milliseconds)
{
    long waited = 0;
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) return true;
        if (waited >= milliseconds) return false;
        sleep_msec(10);
        waited += 10;
    }
}

static void reap_sink(pcmmix *m)
{
    if (m->sink_fd >= 0) {
        close(m->sink_fd);
        m->sink_fd = -1;
    }
    if (m->sink_pid > 0) {
        if (!wait_for_exit(m->sink_pid, 200)) {
            kill(m->sink_pid, SIGTERM);
            if (!wait_for_exit(m->sink_pid, 200)) {
                kill(m->sink_pid, SIGKILL);
                while (waitpid(m->sink_pid, NULL, 0) < 0 && errno == EINTR) {}
            }
        }
        m->sink_pid = -1;
    }
}

/* Fork the sink with a pipe dup2'd to its stdin, shrink the pipe so its
 * capacity is the transport latency, and give the child a short window to
 * prove it can stay alive (a sink with no server to talk to exits
 * immediately). Sink chatter on stdout/stderr is discarded so it cannot
 * land in the host's terminal UI. */
static bool spawn_sink_argv(pcmmix *m, const char *const *argv)
{
    if (!argv || !argv[0]) return false;

    int pfd[2];
    if (pipe(pfd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, STDOUT_FILENO);
            dup2(nul, STDERR_FILENO);
            if (nul > STDERR_FILENO) close(nul);
        }
        execvp(argv[0], (char *const *)(uintptr_t)argv);
        _exit(127);
    }
    close(pfd[0]);
    (void)fcntl(pfd[1], F_SETFD, FD_CLOEXEC);

    m->pipe_paced = false;
#ifdef F_SETPIPE_SZ
    {
        /* A continuously fed pipe runs full, so its size IS the latency:
         * the default 64 KiB of mono s16 at 44.1 kHz would be ~740 ms of
         * lag between a cue and the speaker. */
        uint64_t bytes = (uint64_t)m->sample_rate * m->latency_ms / 1000u *
                         sizeof(int16_t);
        if (bytes < 4096u) bytes = 4096u;
        if (bytes > (1u << 20)) bytes = 1u << 20;
        if (fcntl(pfd[1], F_SETPIPE_SZ, (int)bytes) >= 0)
            m->pipe_paced = true;
    }
#endif

    sleep_msec(SINK_PROBE_MS);
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid || (r < 0 && errno == ECHILD)) {
        close(pfd[1]);
        return false;
    }

    m->sink_fd = pfd[1];
    m->sink_pid = pid;
    m->sink_alive = true;
    const char *slash = strrchr(argv[0], '/');
    snprintf(m->backend, sizeof m->backend, "%s",
             slash ? slash + 1 : argv[0]);
    return true;
}

static bool spawn_probe(pcmmix *m)
{
    char latency_arg[32], rate_eq[32], rate_num[16];
    snprintf(latency_arg, sizeof latency_arg, "--latency-msec=%u",
             m->latency_ms);
    snprintf(rate_eq, sizeof rate_eq, "--rate=%u", m->sample_rate);
    snprintf(rate_num, sizeof rate_num, "%u", m->sample_rate);

    const char *pacat_argv[] = { "pacat", "--raw", latency_arg, rate_eq,
                                 "--channels=1", "--format=s16le", NULL };
    const char *pw_argv[] = { "pw-play", "--raw", rate_eq, "--channels=1",
                              "--format=s16", "-", NULL };
    const char *aplay_argv[] = { "aplay", "-q", "-t", "raw", "-f", "S16_LE",
                                 "-r", rate_num, "-c", "1",
                                 "-B", "30000", "-F", "10000", NULL };
    const char *play_argv[] = { "play", "-q", "-t", "raw", "-r", rate_num,
                                "-e", "signed", "-b", "16", "-c", "1", "-",
                                NULL };
    const char *const *candidates[] = { pacat_argv, pw_argv, aplay_argv,
                                        play_argv };

    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        if (!in_path(candidates[i][0])) continue;
        if (spawn_sink_argv(m, candidates[i])) return true;
    }
    return false;
}

static bool still_running(pcmmix *m)
{
    pthread_mutex_lock(&m->lock);
    bool r = m->running;
    pthread_mutex_unlock(&m->lock);
    return r;
}

/* Write one block to the sink. Blocking against the shrunken pipe is what
 * paces the mixer to the DAC clock; select() bounds each wait so shutdown
 * never hangs on a wedged sink. Returns false when the sink is gone. */
static bool write_block(pcmmix *m, const int16_t *block, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)block;
    size_t left = bytes;
    int stalls = 0;
    while (left > 0) {
        if (!still_running(m)) return true; /* shutting down: abandon */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(m->sink_fd, &wfds);
        struct timeval tv = { 0, 100 * 1000 };
        int rc = select(m->sink_fd + 1, NULL, &wfds, NULL, &tv);
        if (rc == 0) {
            if (++stalls >= SINK_STALL_LIMIT) return false;
            continue;
        }
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        ssize_t n = write(m->sink_fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += (size_t)n;
        left -= (size_t)n;
        stalls = 0;
    }
    return true;
}

static bool sink_exited(pcmmix *m)
{
    if (m->sink_pid <= 0) return true;
    int status;
    pid_t r = waitpid(m->sink_pid, &status, WNOHANG);
    if (r == m->sink_pid || (r < 0 && errno == ECHILD)) {
        m->sink_pid = -1; /* already reaped */
        return true;
    }
    return false;
}

/* Sink death: wind the transport down and leave the mixer in a silent
 * state where every API call is a safe no-op. */
static void transport_fail(pcmmix *m)
{
    reap_sink(m);
    pthread_mutex_lock(&m->lock);
    m->sink_alive = false;
    m->backend[0] = '\0';
    m->enabled = false;
    m->running = false;
    silence_all_locked(m);
    pthread_mutex_unlock(&m->lock);
}

static void *mixer_thread_main(void *arg)
{
    pcmmix *m = arg;
    int16_t out[CHUNK_CAP];
    const size_t frames = m->block_frames;
    const long block_ms =
        (long)((uint64_t)frames * 1000u / m->sample_rate) + 1;

    for (;;) {
        pthread_mutex_lock(&m->lock);
        if (!m->running) {
            pthread_mutex_unlock(&m->lock);
            break;
        }
        /* Silence is streamed too: a continuously fed sink never underruns
         * (no pops) and its blocking read paces this loop for free. */
        mix_chunk_locked(m, out, frames);
        pthread_mutex_unlock(&m->lock);

        if (!write_block(m, out, frames * sizeof out[0])) {
            transport_fail(m);
            break;
        }
        if (sink_exited(m)) {
            transport_fail(m);
            break;
        }
        if (!m->pipe_paced) sleep_msec(block_ms);
    }
    return NULL;
}

/* --- lifecycle -------------------------------------------------------- */

void pcmmix_options_init(pcmmix_options *options)
{
    if (!options) return;
    options->sample_rate = 44100u;
    options->max_voices = 16u;
    options->latency_ms = 24u;
    options->sink_argv = NULL;
    options->soft_clip = true;
    options->offline = false;
}

bool pcmmix_start(pcmmix *m, const pcmmix_options *options)
{
    if (!m) return false;

    pcmmix_options defaults;
    pcmmix_options_init(&defaults);
    if (!options) options = &defaults;

    memset(m, 0, sizeof *m);
    m->sink_fd = -1;
    m->sink_pid = -1;

    m->sample_rate = options->sample_rate ? options->sample_rate : 44100u;
    if (m->sample_rate < 4000u || m->sample_rate > 192000u) return false;
    m->max_voices = options->max_voices ? options->max_voices : 16u;
    if (m->max_voices > PCMMIX_VOICES_MAX) return false;
    m->latency_ms = options->latency_ms ? options->latency_ms : 24u;
    if (m->latency_ms < 5u || m->latency_ms > 1000u) return false;
    m->soft_clip = options->soft_clip;
    m->offline = options->offline;

    uint32_t latency_frames =
        (uint32_t)((uint64_t)m->sample_rate * m->latency_ms / 1000u);
    m->block_frames = latency_frames / 2u;
    if (m->block_frames < 64u) m->block_frames = 64u;
    if (m->block_frames > CHUNK_CAP) m->block_frames = CHUNK_CAP;

    if (pthread_mutex_init(&m->lock, NULL) != 0) return false;
    m->enabled = true;
    m->voice_generation = 1u;

    if (m->offline) {
        m->running = true;
        m->started = true;
        return true;
    }

    signal(SIGPIPE, SIG_IGN); /* sink death must not kill the host */

    bool spawned = options->sink_argv ? spawn_sink_argv(m, options->sink_argv)
                                      : spawn_probe(m);
    if (!spawned) {
        pthread_mutex_destroy(&m->lock);
        return false;
    }

    m->running = true;
    if (pthread_create(&m->thread, NULL, mixer_thread_main, m) != 0) {
        m->running = false;
        reap_sink(m);
        pthread_mutex_destroy(&m->lock);
        return false;
    }
    m->thread_started = true;
    m->started = true;
    return true;
}

void pcmmix_stop(pcmmix *m)
{
    if (!m || !m->started) return;
    if (m->thread_started) {
        pthread_mutex_lock(&m->lock);
        m->running = false;
        pthread_mutex_unlock(&m->lock);
        pthread_join(m->thread, NULL);
        m->thread_started = false;
    }
    m->running = false;
    reap_sink(m);
    pthread_mutex_destroy(&m->lock);
    silence_all_locked(m);
    m->sink_alive = false;
    m->backend[0] = '\0';
    m->started = false;
}

bool pcmmix_is_running(pcmmix *m)
{
    if (!m || !m->started) return false;
    pthread_mutex_lock(&m->lock);
    bool r = m->running;
    pthread_mutex_unlock(&m->lock);
    return r;
}

const char *pcmmix_backend_name(pcmmix *m)
{
    if (!m || !m->started) return NULL;
    pthread_mutex_lock(&m->lock);
    const char *name = m->sink_alive ? m->backend : NULL;
    pthread_mutex_unlock(&m->lock);
    return name;
}

void pcmmix_set_enabled(pcmmix *m, bool on)
{
    if (!m || !m->started) return;
    pthread_mutex_lock(&m->lock);
    m->enabled = on;
    if (!on) silence_all_locked(m);
    pthread_mutex_unlock(&m->lock);
}

bool pcmmix_is_enabled(pcmmix *m)
{
    if (!m || !m->started) return false;
    pthread_mutex_lock(&m->lock);
    bool on = m->enabled;
    pthread_mutex_unlock(&m->lock);
    return on;
}

/* --- voices ----------------------------------------------------------- */

static int alloc_voice_slot_locked(pcmmix *m)
{
    int slot = -1;
    float most = -1.0f;
    for (uint32_t i = 0; i < m->max_voices; i++) {
        pcmmix_voice *v = &m->voices[i];
        if (!v->active) return (int)i;
        if (v->looping || v->frame_count == 0) continue;
        float done = v->pos / (float)v->frame_count;
        if (done > most) { /* steal the most-finished one-shot */
            most = done;
            slot = (int)i;
        }
    }
    return slot;
}

static int start_voice(pcmmix *m, const pcmmix_sample *sample, float vol,
                       float pitch, bool looping)
{
    if (!m || !m->started || !sample || !sample->frames ||
        sample->frame_count == 0 || sample->frame_count > SAMPLE_FRAMES_MAX)
        return -1;
    int handle = -1;
    pthread_mutex_lock(&m->lock);
    if (m->running && m->enabled) {
        int slot = alloc_voice_slot_locked(m);
        if (slot >= 0) {
            uint32_t generation = m->voice_generation;
            m->voice_generation =
                generation >= GENERATION_MAX ? 1u : generation + 1u;
            m->voices[slot] = (pcmmix_voice){
                .data = sample->frames,
                .frame_count = sample->frame_count,
                .pos = 0.0f,
                .step = sanitize_pitch(pitch),
                .vol = clampf(vol, 0.0f, 2.0f),
                .generation = generation,
                .active = true,
                .looping = looping,
            };
            handle =
                (int)(generation * PCMMIX_VOICES_MAX + (uint32_t)slot);
        }
    }
    pthread_mutex_unlock(&m->lock);
    return handle;
}

int pcmmix_play(pcmmix *m, const pcmmix_sample *sample, float vol,
                float pitch)
{
    return start_voice(m, sample, vol, pitch, false);
}

int pcmmix_loop(pcmmix *m, const pcmmix_sample *sample, float vol,
                float pitch)
{
    return start_voice(m, sample, vol, pitch, true);
}

/* Lock held. Returns NULL for stale, recycled, or out-of-range handles. */
static pcmmix_voice *voice_from_handle(pcmmix *m, int handle)
{
    if (handle <= 0) return NULL;
    uint32_t h = (uint32_t)handle;
    uint32_t slot = h % PCMMIX_VOICES_MAX;
    uint32_t generation = h / PCMMIX_VOICES_MAX;
    if (slot >= m->max_voices) return NULL;
    pcmmix_voice *v = &m->voices[slot];
    if (!v->active || v->generation != generation) return NULL;
    return v;
}

bool pcmmix_voice_set(pcmmix *m, int voice, float vol, float pitch)
{
    if (!m || !m->started) return false;
    pthread_mutex_lock(&m->lock);
    pcmmix_voice *v = voice_from_handle(m, voice);
    if (v) {
        v->vol = clampf(vol, 0.0f, 2.0f);
        v->step = sanitize_pitch(pitch);
    }
    pthread_mutex_unlock(&m->lock);
    return v != NULL;
}

bool pcmmix_voice_stop(pcmmix *m, int voice)
{
    if (!m || !m->started) return false;
    pthread_mutex_lock(&m->lock);
    pcmmix_voice *v = voice_from_handle(m, voice);
    if (v) v->active = false;
    pthread_mutex_unlock(&m->lock);
    return v != NULL;
}

bool pcmmix_voice_active(pcmmix *m, int voice)
{
    if (!m || !m->started) return false;
    pthread_mutex_lock(&m->lock);
    bool active = voice_from_handle(m, voice) != NULL;
    pthread_mutex_unlock(&m->lock);
    return active;
}

/* --- music ------------------------------------------------------------ */

void pcmmix_music_play(pcmmix *m, const pcmmix_sample *sample, float vol,
                       bool loop)
{
    if (!m || !m->started || !sample || !sample->frames ||
        sample->frame_count == 0)
        return;
    pthread_mutex_lock(&m->lock);
    if (!m->running || !m->enabled) {
        pthread_mutex_unlock(&m->lock);
        return;
    }
    vol = clampf(vol, 0.0f, 2.0f);
    const float fade_step = 1.0f / (MUSIC_FADE_SECONDS * (float)m->sample_rate);

    /* Already the current track: just retarget. */
    for (uint32_t i = 0; i < PCMMIX_MUSIC_SLOTS; i++) {
        pcmmix_music_voice *mv = &m->music[i];
        if (mv->active && mv->data == sample->frames && mv->target > 0.0f) {
            mv->target = vol;
            mv->looping = loop;
            pthread_mutex_unlock(&m->lock);
            return;
        }
    }

    /* Crossfade: everything playing fades out, the new track fades in. */
    int free_slot = -1;
    for (uint32_t i = 0; i < PCMMIX_MUSIC_SLOTS; i++) {
        pcmmix_music_voice *mv = &m->music[i];
        if (mv->active) {
            mv->target = 0.0f;
            mv->fade_step = fade_step;
        } else {
            free_slot = (int)i;
        }
    }
    if (free_slot < 0) /* both busy: steal the quieter one */
        free_slot = m->music[0].vol <= m->music[1].vol ? 0 : 1;

    m->music[free_slot] = (pcmmix_music_voice){
        .data = sample->frames,
        .frame_count = sample->frame_count,
        .pos = 0.0,
        .vol = 0.0f,
        .target = vol,
        .fade_step = fade_step,
        .looping = loop,
        .active = true,
    };
    pthread_mutex_unlock(&m->lock);
}

void pcmmix_music_stop(pcmmix *m, float fade_seconds)
{
    if (!m || !m->started) return;
    pthread_mutex_lock(&m->lock);
    for (uint32_t i = 0; i < PCMMIX_MUSIC_SLOTS; i++) {
        pcmmix_music_voice *mv = &m->music[i];
        if (!mv->active) continue;
        if (fade_seconds <= 0.0f) {
            *mv = (pcmmix_music_voice){0};
        } else {
            mv->target = 0.0f;
            mv->fade_step = 1.0f / (fade_seconds * (float)m->sample_rate);
        }
    }
    pthread_mutex_unlock(&m->lock);
}

const int16_t *pcmmix_music_current(pcmmix *m)
{
    if (!m || !m->started) return NULL;
    const int16_t *current = NULL;
    pthread_mutex_lock(&m->lock);
    for (uint32_t i = 0; i < PCMMIX_MUSIC_SLOTS; i++)
        if (m->music[i].active && m->music[i].target > 0.0f)
            current = m->music[i].data;
    pthread_mutex_unlock(&m->lock);
    return current;
}

/* --- generator and offline fill --------------------------------------- */

void pcmmix_set_generator(pcmmix *m, pcmmix_generator_fn fn, void *user)
{
    if (!m || !m->started) return;
    pthread_mutex_lock(&m->lock);
    m->generator = fn;
    m->generator_user = fn ? user : NULL;
    pthread_mutex_unlock(&m->lock);
}

void pcmmix_mix_block(pcmmix *m, int16_t *dst, size_t frames)
{
    if (!m || !dst || frames == 0) return;
    if (frames > SIZE_MAX / sizeof(int16_t)) return;
    if (!m->started) {
        memset(dst, 0, frames * sizeof(int16_t));
        return;
    }
    pthread_mutex_lock(&m->lock);
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > CHUNK_CAP) n = CHUNK_CAP;
        mix_chunk_locked(m, dst + done, n);
        done += n;
    }
    pthread_mutex_unlock(&m->lock);
}
