# pcm-mixer

`pcm-mixer` is a small C11 library that gives terminal games sound without
an audio dependency. It software-mixes 16-bit mono PCM voices on a
background thread and streams raw s16le down a pipe to the first available
command-line sink (`pacat`, `pw-play`, `aplay`, or sox `play`). It
consolidates the audio layer shared by the terminal-lander / chess-bash
family of terminal games.

The mixer provides pitch-shifted one-shots with voice stealing, held loops
with live volume/pitch control, a crossfading two-slot music layer, a
generator callback for live synthesis, a strict diagnostic WAV loader, and
an offline mode for tests and bounce-to-disk rendering. It requires POSIX
and pthreads; there are no other dependencies.

## Build and test

```sh
make
make test
make sanitize
./build/beep
```

The final command plays an audible test tone through the default sink
probe. The test suite is silent: it renders offline and captures transport
output through `cat`, and never spawns a real audio player.

## Quick start

```c
#include "pcm_mixer.h"

pcmmix mixer;
pcmmix_options options;
pcmmix_options_init(&options);

if (!pcmmix_start(&mixer, &options)) {
    /* No sink available: run the game silently. */
}

char err[256];
size_t frames = 0;
int16_t *pcm = pcmmix_wav_load("assets/sfx/explosion.wav", &frames,
                               err, sizeof err);
pcmmix_sample explosion = { pcm, frames };

pcmmix_play(&mixer, &explosion, 0.8f, 1.0f);          /* fire and forget */

int engine = pcmmix_loop(&mixer, &engine_sample, 0.5f, 1.0f);
pcmmix_voice_set(&mixer, engine, 0.7f, 1.4f);         /* rev it live */
pcmmix_voice_stop(&mixer, engine);

pcmmix_music_play(&mixer, &battle_theme, 1.0f, true); /* crossfades in */
pcmmix_music_stop(&mixer, 2.0f);

pcmmix_stop(&mixer);
pcmmix_wav_free(pcm);   /* samples are caller-owned; free after stop */
```

## Transport

The stream contract is fixed: signed 16-bit little-endian, mono, at the
configured sample rate (default 44100 Hz). `pcmmix_start()` probes for
sinks in this order and forks the first executable on `PATH` that survives
an ~80 ms liveness window (a sink with no server to talk to exits
immediately):

1. `pacat` (PulseAudio)
2. `pw-play` (PipeWire)
3. `aplay` (ALSA)
4. `play` (sox)

The sink reads PCM from a pipe dup2'd to its stdin; its stdout/stderr are
sent to `/dev/null` so sink chatter cannot land in the host's terminal UI.
`options.sink_argv` replaces the probe with a custom NULL-terminated argv,
which is also how integration tests capture the stream (`sh -c 'cat >
file'`).

The latency model: on Linux the pipe is shrunk with `F_SETPIPE_SZ` to about
`latency_ms` worth of audio, the mixer streams continuously — silence
included — so the pipe runs full, and the sink's blocking reads then clock
the mixer thread at a fixed latency with no underruns. Where
`F_SETPIPE_SZ` is unavailable the mixer falls back to sleep-based pacing,
at the cost of a larger pipe buffer. Each blocking write is bounded by
`select()` so shutdown never hangs on a wedged sink.

`pcmmix_start()` ignores `SIGPIPE` process-wide: sink death must not kill
the host. A sink that dies mid-run (write error, exit, or a multi-second
stall) winds the mixer down into a silent state: `pcmmix_is_running()`
returns false, `pcmmix_backend_name()` returns NULL, the enabled flag
clears, and every playback call becomes a safe no-op. The game keeps
running, just silently. `pcmmix_stop()` is still required and reaps the
sink, escalating to SIGTERM then SIGKILL if it will not exit.

## Voices

Samples are caller-owned (`pcmmix_sample` is a pointer plus a frame count);
the mixer never copies or frees PCM, so the memory must stay valid until no
voice references it. `pcmmix_play()` starts a one-shot and `pcmmix_loop()`
a seamless loop, both with a linear gain and a resampling pitch ratio
(linear interpolation). They return a voice handle usable with
`pcmmix_voice_set()` (live gain/pitch, the thrust-and-engine control path),
`pcmmix_voice_stop()`, and `pcmmix_voice_active()`.

When every slot is busy, the most-finished one-shot is stolen; looping
voices are never stolen. Handles are generation-checked, so a stale handle
whose slot has been recycled is rejected instead of adjusting the wrong
voice.

The master bus applies `tanh` soft-clipping by default
(`options.soft_clip = false` selects hard clipping instead, which tests use
for sample-exact output).

## Music

`pcmmix_music_play()` plays a track at 1.0 pitch and automatically
crossfades (0.5 s) from whatever is playing: the old track fades out while
the new one fades in on the second slot. Calling it again with the track
that is already playing just retargets its volume and loop flag. Tracks
are identified by their frames pointer. `pcmmix_music_stop(fade_seconds)`
fades everything to silence, and `pcmmix_music_current()` reports the
frames pointer of the track currently playing or fading in.

## Generator hook

`pcmmix_set_generator()` installs a block callback for live synthesis —
engine drones tied to game state, procedural music — that would be awkward
to express as baked samples:

```c
void engines(float *dst, size_t frames, void *user);
pcmmix_set_generator(&mixer, engines, &game);
```

The callback receives a zero-filled float buffer, writes samples in
[-1, 1], and the result is added to the mix. It runs on the mixer thread
with the mixer lock held: it must be fast and lock-free, must not block,
and must not call back into the pcmmix API.

## WAV loader

`pcmmix_wav_load()` is deliberately strict: PCM, mono, 16-bit, 44100 Hz
only. An asset that does not match is a bug in whatever generated it, and
silently resampling would hide that. Failures return NULL with a
human-readable reason (`"sfx/hit.wav is 2ch/16bit/44100Hz, expected
1ch/16bit/44100Hz"`) written into the caller's error buffer. The parser
walks RIFF chunks with word-alignment padding and bounds checks rather than
assuming a canonical 44-byte header, and caps files at 64 MiB.

## Offline rendering and testing

`options.offline = true` starts the mixer with no sink and no thread; the
caller pulls the master mix with `pcmmix_mix_block()`, which renders the
exact fill the transport thread would stream. This is how the unit tests
assert on mixer output without any audio hardware, and it doubles as a
bounce-to-disk path. For transport-level tests, `options.sink_argv` can
point at any PCM-consuming command.

## Threading contract

`pcmmix_start()` and `pcmmix_stop()` belong to one control thread;
`pcmmix_stop()` must not race other calls, and after it returns nothing
touches the mixer object or sample memory. All other calls are
thread-safe against the mixer thread and each other. The generator
callback runs on the mixer thread. `pcmmix_mix_block()` is for offline
mixers only — never call it while a transport thread is live.

## Install

```sh
make install PREFIX=/usr/local
```

The build produces static and shared libraries. Applications may instead
compile `src/pcm_mixer.c` and `src/pcm_wav.c` directly. The library has no
dependencies beyond the C and POSIX system libraries.

The API is pre-1.0 and may change between minor releases.

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
