/* pcm-mixer core: voice mixing, music crossfade, generator hook, and the
 * pipe-to-CLI-sink transport. */
#define _GNU_SOURCE /* F_SETPIPE_SZ on Linux; ignored elsewhere */

#include "pcm_mixer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { CHUNK_CAP = 1024 };

/* Voice handles are generation * PCMMIX_VOICES_MAX + slot; the generation
 * counter wraps below INT_MAX / PCMMIX_VOICES_MAX so handles stay > 0. */
#define GENERATION_MAX ((uint32_t)INT_MAX / PCMMIX_VOICES_MAX - 1u)
#define SAMPLE_FRAMES_MAX (1u << 24) /* float resampler precision bound */
#define MUSIC_FADE_SECONDS 0.5f
#define SINK_PROBE_MS 80
#define SINK_POLL_MS 100
#define SINK_STALL_MS 5000

extern char **environ;

static float clampf(float v, float lo, float hi)
{
    if (isnan(v)) return 0.0f;
    return v < lo ? lo : v > hi ? hi : v;
}

static float sanitize_pitch(float pitch)
{
    if (!isfinite(pitch) || pitch <= 0.0f) return 1.0f;
    return clampf(pitch, 1.0f / 32.0f, 32.0f);
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static void sleep_msec(long milliseconds)
{
    struct timespec ts = { milliseconds / 1000,
                           (milliseconds % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* --- mixing (mixer lock held) ---------------------------------------- */

static void pan_gains(const pcmmix_voice *v, bool stereo, float *left,
                      float *right)
{
    if (!stereo) { *left = *right = v->vol; return; }
    float pan = isfinite(v->pan) ? clampf(v->pan, -1.0f, 1.0f) : 0.0f;
    *left = v->vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
    *right = v->vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
}

static void mix_voice_unity(pcmmix_voice *v, float *left, float *right,
                            size_t frames, bool stereo)
{
    const size_t count = v->frame_count;
    float gain_l, gain_r;
    pan_gains(v, stereo, &gain_l, &gain_r);
    const float scale_l = gain_l * (1.0f / 32768.0f);
    const float scale_r = gain_r * (1.0f / 32768.0f);
    size_t source = (size_t)v->pos;
    size_t output = 0u;

    while (output < frames) {
        size_t available;
        size_t take;

        if (source >= count) {
            if (!v->looping) {
                v->active = false;
                break;
            }
            source = 0u;
        }
        available = count - source;
        take = frames - output;
        if (take > available) take = available;
        for (size_t index = 0u; index < take; ++index) {
            left[output + index] += (float)v->data[source + index] * scale_l;
            right[output + index] += (float)v->data[source + index] * scale_r;
        }
        source += take;
        output += take;
    }
    if (v->looping && source >= count) source = 0u;
    v->pos = (float)source;
    if (!v->looping && source >= count) v->active = false;
}

static void mix_voice(pcmmix_voice *v, float *left, float *right,
                      size_t frames, bool stereo)
{
    if (!v->active) return;
    if (!v->data || v->frame_count == 0) {
        v->active = false;
        return;
    }
    const size_t count = v->frame_count;
    const float length = (float)count;
    if (!isfinite(v->pos) || !isfinite(v->step) || !isfinite(v->vol)) {
        v->active = false;
        return;
    }
    if (v->step == 1.0f && v->pos >= 0.0f &&
        v->pos == floorf(v->pos)) {
        mix_voice_unity(v, left, right, frames, stereo);
        return;
    }
    float gain_l, gain_r;
    pan_gains(v, stereo, &gain_l, &gain_r);
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
        const float sample = a + (b - a) * frac;
        left[i] += sample * gain_l;
        right[i] += sample * gain_r;
        v->pos += v->step;
    }
    if (!v->looping && v->pos >= length) v->active = false;
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
    if (!mv->looping && mv->pos >= (double)mv->frame_count)
        mv->active = false;
}

static int16_t mix_to_s16(float sample)
{
    long rounded;

    if (isnan(sample)) return 0;
    if (sample > (float)INT16_MAX * (1.0f / 32768.0f)) return INT16_MAX;
    if (sample <= -1.0f) return INT16_MIN;
    rounded = lrintf(sample * 32768.0f);
    return (int16_t)rounded;
}

static void mix_chunk_locked(pcmmix *m, int16_t *dst, size_t frames)
{
    float left[CHUNK_CAP], right[CHUNK_CAP], centered[CHUNK_CAP];
    const size_t samples = frames * m->output_channels;

    if (!m->enabled) {
        memset(dst, 0, samples * sizeof dst[0]);
        m->stats.rendered_frames += frames;
        return;
    }
    bool have_source = m->generator != NULL ||
                       m->interleaved_generator != NULL;
    for (uint32_t index = 0u; index < m->max_voices && !have_source; ++index)
        have_source = m->voices[index].active;
    for (uint32_t index = 0u;
         index < PCMMIX_MUSIC_SLOTS && !have_source; ++index)
        have_source = m->music[index].active;
    if (!have_source) {
        memset(dst, 0, samples * sizeof dst[0]);
        m->stats.rendered_frames += frames;
        return;
    }
    memset(left, 0, frames * sizeof left[0]);
    memset(right, 0, frames * sizeof right[0]);
    memset(centered, 0, frames * sizeof centered[0]);
    for (uint32_t i = 0; i < m->max_voices; i++)
        mix_voice(&m->voices[i], left, right, frames,
                  m->output_channels == 2u);
    for (uint32_t i = 0; i < PCMMIX_MUSIC_SLOTS; i++)
        mix_music(&m->music[i], centered, frames);
    if (m->generator) {
        float gen[CHUNK_CAP];
        memset(gen, 0, frames * sizeof gen[0]);
        m->generator(gen, frames, m->generator_user);
        for (size_t i = 0; i < frames; i++) centered[i] += gen[i];
    } else if (m->interleaved_generator) {
        float gen[CHUNK_CAP * 2u];
        memset(gen, 0, samples * sizeof gen[0]);
        m->interleaved_generator(gen, frames, m->output_channels,
                                 m->generator_user);
        for (size_t frame = 0u; frame < frames; ++frame) {
            left[frame] += gen[frame * m->output_channels];
            if (m->output_channels == 2u)
                right[frame] += gen[frame * 2u + 1u];
        }
    }
    /* Music and the legacy mono generator are centered. */
    for (size_t i = 0; i < frames; ++i) {
        left[i] += centered[i];
        right[i] += centered[i];
    }
    for (size_t i = 0; i < frames; i++) {
        const float l = m->soft_clip ? tanhf(left[i]) : left[i];
        if (m->output_channels == 1u) dst[i] = mix_to_s16(l);
        else {
            const float r = m->soft_clip ? tanhf(right[i]) : right[i];
            dst[i * 2u] = mix_to_s16(l);
            dst[i * 2u + 1u] = mix_to_s16(r);
        }
    }
    m->stats.rendered_frames += frames;
}

static void silence_all_locked(pcmmix *m)
{
    memset(m->voices, 0, sizeof m->voices);
    memset(m->music, 0, sizeof m->music);
}

/* --- sink discovery and transport ------------------------------------ */

static bool set_close_on_exec(int descriptor)
{
    const int flags = fcntl(descriptor, F_GETFD);

    return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool make_cloexec_pipe(int descriptors[2])
{
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(descriptors, O_CLOEXEC) == 0) return true;
    if (errno != ENOSYS && errno != EINVAL) return false;
#endif
    if (pipe(descriptors) != 0) return false;
    if (!set_close_on_exec(descriptors[0]) ||
        !set_close_on_exec(descriptors[1])) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        return false;
    }
    return true;
}

static bool move_above_stdio(int *descriptor)
{
    int duplicate;

    if (*descriptor > STDERR_FILENO) return true;
#ifdef F_DUPFD_CLOEXEC
    duplicate = fcntl(*descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
    duplicate = fcntl(*descriptor, F_DUPFD, STDERR_FILENO + 1);
    if (duplicate >= 0 && !set_close_on_exec(duplicate)) {
        (void)close(duplicate);
        duplicate = -1;
    }
#endif
    if (duplicate < 0) return false;
    (void)close(*descriptor);
    *descriptor = duplicate;
    return true;
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

/* Spawn the sink with a pipe dup2'd to its stdin, shrink the pipe so its
 * capacity is the transport latency, and give the child a short window to
 * prove it can stay alive (a sink with no server to talk to exits
 * immediately). Sink chatter on stdout/stderr is discarded so it cannot
 * land in the host's terminal UI. */
static bool spawn_sink_argv(pcmmix *m, const char *const *argv)
{
    posix_spawn_file_actions_t actions;
    pid_t pid = -1;
    int action_error;
    int devnull;
    int flags;
    int pfd[2];
    bool actions_initialized;

    if (!argv || !argv[0]) return false;
    if (!make_cloexec_pipe(pfd)) return false;
    if (!move_above_stdio(&pfd[0]) || !move_above_stdio(&pfd[1])) {
        (void)close(pfd[0]);
        (void)close(pfd[1]);
        return false;
    }
#ifdef O_CLOEXEC
    devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
#else
    devnull = open("/dev/null", O_RDWR);
#endif
    if (devnull < 0 || !move_above_stdio(&devnull) ||
        !set_close_on_exec(devnull)) {
        if (devnull >= 0) (void)close(devnull);
        (void)close(pfd[0]);
        (void)close(pfd[1]);
        return false;
    }
    flags = fcntl(pfd[1], F_GETFL);
    if (flags < 0 || fcntl(pfd[1], F_SETFL, flags | O_NONBLOCK) != 0) {
        (void)close(devnull);
        (void)close(pfd[0]);
        (void)close(pfd[1]);
        return false;
    }

    action_error = posix_spawn_file_actions_init(&actions);
    actions_initialized = action_error == 0;
    if (action_error == 0)
        action_error = posix_spawn_file_actions_adddup2(
            &actions, pfd[0], STDIN_FILENO);
    if (action_error == 0)
        action_error = posix_spawn_file_actions_adddup2(
            &actions, devnull, STDOUT_FILENO);
    if (action_error == 0)
        action_error = posix_spawn_file_actions_adddup2(
            &actions, devnull, STDERR_FILENO);
    if (action_error == 0)
        action_error = posix_spawn_file_actions_addclose(&actions, pfd[0]);
    if (action_error == 0)
        action_error = posix_spawn_file_actions_addclose(&actions, pfd[1]);
    if (action_error == 0)
        action_error = posix_spawn_file_actions_addclose(&actions, devnull);
    if (action_error == 0) {
        action_error = posix_spawnp(
            &pid, argv[0], &actions, NULL,
            (char *const *)(uintptr_t)argv, environ);
    }
    if (actions_initialized)
        (void)posix_spawn_file_actions_destroy(&actions);
    (void)close(devnull);
    (void)close(pfd[0]);
    if (action_error != 0) {
        (void)close(pfd[1]);
        errno = action_error;
        return false;
    }

    m->pipe_paced = false;
#ifdef F_SETPIPE_SZ
    {
        /* A continuously fed pipe runs full, so its size IS the latency:
         * the default 64 KiB of mono s16 at 44.1 kHz would be ~740 ms of
         * lag between a cue and the speaker. */
        uint64_t bytes = (uint64_t)m->sample_rate * m->latency_ms / 1000u *
                         sizeof(int16_t) * m->output_channels;
        if (bytes < 4096u) bytes = 4096u;
        if (bytes > (1u << 20)) bytes = 1u << 20;
        if (fcntl(pfd[1], F_SETPIPE_SZ, (int)bytes) >= 0)
            m->pipe_paced = true;
    }
#endif

    sleep_msec(SINK_PROBE_MS);
    int status;
    pid_t r;
    do {
        r = waitpid(pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == pid || (r < 0 && errno == ECHILD)) {
        (void)close(pfd[1]);
        return false;
    }
    if (r < 0) {
        (void)close(pfd[1]);
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
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
    char channel_eq[24], channel_num[12];
    snprintf(latency_arg, sizeof latency_arg, "--latency-msec=%u",
             m->latency_ms);
    snprintf(rate_eq, sizeof rate_eq, "--rate=%u", m->sample_rate);
    snprintf(rate_num, sizeof rate_num, "%u", m->sample_rate);
    snprintf(channel_eq, sizeof channel_eq, "--channels=%u", m->output_channels);
    snprintf(channel_num, sizeof channel_num, "%u", m->output_channels);

    const char *pacat_argv[] = { "pacat", "--raw", latency_arg, rate_eq,
                                 channel_eq, "--format=s16le", NULL };
    const char *pw_argv[] = { "pw-play", "--raw", rate_eq, channel_eq,
                              "--format=s16", "-", NULL };
    const char *aplay_argv[] = { "aplay", "-q", "-t", "raw", "-f", "S16_LE",
                                 "-r", rate_num, "-c", channel_num,
                                 "-B", "30000", "-F", "10000", NULL };
    const char *play_argv[] = { "play", "-q", "-t", "raw", "-r", rate_num,
                                "-e", "signed", "-b", "16", "-c", channel_num, "-",
                                NULL };
    const char *const *candidates[] = { pacat_argv, pw_argv, aplay_argv,
                                        play_argv };

    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
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

/* Write one block to the sink. Readiness against the shrunken pipe paces the
 * mixer to the sink clock. The descriptor stays nonblocking and an absolute
 * no-progress deadline survives EINTR, so neither a large descriptor number,
 * a signal storm, nor a partially writable pipe can defeat bounded shutdown. */
static bool write_block(pcmmix *m, const void *block, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)block;
    size_t left = bytes;
    uint64_t deadline = monotonic_milliseconds() + SINK_STALL_MS;

    while (left > 0) {
        if (!still_running(m)) return true; /* shutting down: abandon */
        const uint64_t now = monotonic_milliseconds();
        if (now >= deadline) return false;
        uint64_t remaining = deadline - now;
        int timeout = remaining < SINK_POLL_MS ? (int)remaining : SINK_POLL_MS;
        struct pollfd sink = {m->sink_fd, POLLOUT, 0};
        int rc = poll(&sink, 1u, timeout);

        if (rc == 0) {
            pthread_mutex_lock(&m->lock);
            m->stats.poll_timeouts++;
            pthread_mutex_unlock(&m->lock);
            continue;
        }
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if ((sink.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
        if ((sink.revents & POLLOUT) == 0) continue;
        ssize_t n = write(m->sink_fd, p, left);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return false;
        }
        if (n == 0) return false;
        pthread_mutex_lock(&m->lock);
        m->stats.bytes_written += (uint64_t)n;
        if ((size_t)n < left) m->stats.partial_writes++;
        pthread_mutex_unlock(&m->lock);
        p += (size_t)n;
        left -= (size_t)n;
        deadline = monotonic_milliseconds() + SINK_STALL_MS;
    }
    return true;
}

static bool write_pcm_block(pcmmix *m, const int16_t *block, size_t frames)
{
    uint8_t little_endian[CHUNK_CAP * 2u * sizeof block[0]];
    const size_t samples = frames * m->output_channels;

    for (size_t index = 0u; index < samples; ++index) {
        const uint16_t sample = (uint16_t)block[index];

        little_endian[index * 2u] = (uint8_t)(sample & UINT16_C(0xff));
        little_endian[index * 2u + 1u] = (uint8_t)(sample >> 8);
    }
    return write_block(m, little_endian, samples * sizeof block[0]);
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
    pthread_mutex_lock(&m->lock);
    m->sink_alive = false;
    m->enabled = false;
    m->running = false;
    m->stats.sink_failures++;
    silence_all_locked(m);
    pthread_mutex_unlock(&m->lock);
    reap_sink(m);
}

static void *mixer_thread_main(void *arg)
{
    pcmmix *m = arg;
    int16_t out[CHUNK_CAP * 2u];
    const size_t frames = m->block_frames;
    const long block_ms =
        (long)((uint64_t)frames * 1000u / m->sample_rate) + 1;
    sigset_t blocked;

    if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGPIPE) != 0 ||
        pthread_sigmask(SIG_BLOCK, &blocked, NULL) != 0) {
        transport_fail(m);
        return NULL;
    }

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

        if (!write_pcm_block(m, out, frames)) {
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
    options->output_channels = 1u;
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
    m->output_channels = options->output_channels ? options->output_channels : 1u;
    if (m->output_channels > 2u) return false;
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

uint32_t pcmmix_channels(pcmmix *m)
{
    if (!m || !m->started) return 0u;
    return m->output_channels;
}

void pcmmix_get_stats(pcmmix *m, pcmmix_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!m || !m->started) return;
    pthread_mutex_lock(&m->lock);
    *out = m->stats;
    pthread_mutex_unlock(&m->lock);
}

void pcmmix_set_enabled(pcmmix *m, bool on)
{
    if (!m || !m->started) return;
    pthread_mutex_lock(&m->lock);
    m->enabled = on && m->running;
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
                       float pitch, float pan, bool looping)
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
                .pan = isfinite(pan) ? clampf(pan, -1.0f, 1.0f) : 0.0f,
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
    return start_voice(m, sample, vol, pitch, 0.0f, false);
}

int pcmmix_play_pan(pcmmix *m, const pcmmix_sample *sample, float vol,
                    float pitch, float pan)
{
    return start_voice(m, sample, vol, pitch, pan, false);
}

int pcmmix_loop(pcmmix *m, const pcmmix_sample *sample, float vol,
                float pitch)
{
    return start_voice(m, sample, vol, pitch, 0.0f, true);
}

int pcmmix_loop_pan(pcmmix *m, const pcmmix_sample *sample, float vol,
                    float pitch, float pan)
{
    return start_voice(m, sample, vol, pitch, pan, true);
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

bool pcmmix_voice_set_pan(pcmmix *m, int voice, float vol, float pitch,
                          float pan)
{
    if (!m || !m->started) return false;
    pthread_mutex_lock(&m->lock);
    pcmmix_voice *v = voice_from_handle(m, voice);
    if (v) {
        v->vol = clampf(vol, 0.0f, 2.0f);
        v->step = sanitize_pitch(pitch);
        v->pan = isfinite(pan) ? clampf(pan, -1.0f, 1.0f) : 0.0f;
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
        if (!isfinite(fade_seconds) || fade_seconds <= 0.0f) {
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
    m->interleaved_generator = NULL;
    m->generator_user = fn ? user : NULL;
    pthread_mutex_unlock(&m->lock);
}

void pcmmix_set_generator_interleaved(
    pcmmix *m, pcmmix_generator_interleaved_fn fn, void *user)
{
    if (!m || !m->started) return;
    pthread_mutex_lock(&m->lock);
    m->generator = NULL;
    m->interleaved_generator = fn;
    m->generator_user = fn ? user : NULL;
    pthread_mutex_unlock(&m->lock);
}

void pcmmix_mix_block(pcmmix *m, int16_t *dst, size_t frames)
{
    if (!m || !dst || frames == 0) return;
    if (!m->started || !m->offline) {
        /* An unstarted object has no negotiated channel count; retain the
         * legacy guarantee of clearing `frames` samples. */
        const size_t channels = m->started && m->output_channels == 2u ? 2u : 1u;
        if (frames > SIZE_MAX / (sizeof(int16_t) * channels)) return;
        memset(dst, 0, frames * channels * sizeof(int16_t));
        return;
    }
    if (frames > SIZE_MAX / (sizeof(int16_t) * m->output_channels)) return;
    pthread_mutex_lock(&m->lock);
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > CHUNK_CAP) n = CHUNK_CAP;
        mix_chunk_locked(m, dst + done * m->output_channels, n);
        done += n;
    }
    pthread_mutex_unlock(&m->lock);
}
