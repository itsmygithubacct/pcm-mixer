# pcm-mixer

`pcm-mixer` is a small C11 library that gives terminal games and applications
sound without an audio dependency. It software-mixes 16-bit mono PCM voices on
a background thread and streams mono or stereo interleaved s16le down a pipe
to the first available command-line sink (`pacat`, `pw-play`, `aplay`, or sox
`play`). It
consolidates the audio layer shared by the terminal-lander / chess-bash
family of terminal games.

The mixer provides pitch-shifted one-shots with voice stealing, held loops
with live volume/pitch control, a crossfading two-slot music layer, a
generator callback for live synthesis, a strict diagnostic WAV loader, and
an ownership-aware cue/variant bank, and an offline mode for tests and
bounce-to-disk rendering. It requires POSIX and pthreads; there are no other
dependencies.

## Build and test

```sh
make
make test
make sanitize
make tsanitize
make benchmark
./build/beep
```

The final command plays an audible test tone through the default sink
probe. The test suite is silent: it renders offline and captures transport
output through `cat`, and never spawns a real audio player.
The sanitizer targets repeat it under ASan/UBSan and ThreadSanitizer.

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

## Cue and variant bank

`pcmmix_bank` replaces the sample arrays, WAV ownership loops, and random
variant selection that games otherwise repeat. It stores up to 64 cues with
eight variants each, can borrow generated PCM or own loaded/allocated PCM,
avoids immediate random repeats, and applies per-variant gain and pitch.

```c
#include "pcmmix_bank.h"

pcmmix_bank bank;
pcmmix_bank_init(&bank, SFX_COUNT, 0x1234u);
pcmmix_bank_load_variants(&bank, SFX_HIT, hit_paths, hit_path_count,
                          0.8f, 1.0f, error, sizeof error);
pcmmix_bank_play(&mixer, &bank, SFX_HIT, 1.0f, 1.0f);

pcmmix_stop(&mixer);   /* no voice may still reference bank memory */
pcmmix_bank_clear(&bank);
```

Variant loading is transactional: if any path fails, the cue's previous
samples remain intact. `pcmmix_bank_take()` transfers a malloc-compatible
generated buffer only on success; `pcmmix_bank_set_borrowed()` leaves
ownership with the caller. A frame pointer may be borrowed by more than one
slot, but it may be owned by only one; attempts to create an ownership alias
are rejected without changing the bank. Bank mutation and cleanup are
control-thread operations and must not race playback or precede mixer
shutdown.

## Transport

The stream contract is signed 16-bit little-endian at the configured sample
rate (default 44100 Hz) and channel count (one by default, optionally two).
`pcmmix_start()` probes for
sinks in this order and uses `posix_spawnp()` for the first executable on
`PATH` that survives an ~80 ms liveness window (a sink with no server to
talk to exits immediately):

1. `pacat` (PulseAudio)
2. `pw-play` (PipeWire)
3. `aplay` (ALSA)
4. `play` (sox)

The sink reads explicitly encoded little-endian PCM from a close-on-exec
pipe mapped to its stdin; its stdout/stderr are sent to `/dev/null` so sink
chatter cannot land in the host's terminal UI. `options.sink_argv` replaces
the probe with a custom NULL-terminated argv, which is also how integration
tests capture the stream (`sh -c 'cat > file'`).

The latency model: on Linux the pipe is shrunk with `F_SETPIPE_SZ` to about
`latency_ms` worth of audio, the mixer streams continuously — silence
included — so the pipe runs full, and the sink's blocking reads then clock
the mixer thread at a fixed latency with no underruns. Where
`F_SETPIPE_SZ` is unavailable the mixer falls back to sleep-based pacing,
at the cost of a larger pipe buffer. The write end remains nonblocking and
`poll()` uses a monotonic no-progress deadline, so large descriptor numbers,
signals, partial writes, and a wedged sink cannot make shutdown hang.

Only the mixer thread blocks `SIGPIPE`; the host process's signal disposition
is not changed. A sink that dies mid-run (write error, exit, or a
multi-second stall) winds the mixer down into a silent state:
`pcmmix_is_running()` returns false, `pcmmix_backend_name()` returns NULL,
the enabled flag clears, and every playback call becomes a safe no-op that
cannot revive the dead transport. A previously returned backend-name pointer
remains valid until `pcmmix_stop()`. The game keeps running, just silently.
`pcmmix_stop()` is still required and reaps the sink, escalating to SIGTERM
then SIGKILL if it will not exit.

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
voice. Non-finite controls are normalized before they reach the resampler,
and a one-shot becomes inactive in the same block that consumes its final
frame.

The master bus applies `tanh` soft-clipping by default
(`options.soft_clip = false` selects hard clipping over the full signed
16-bit range, which tests use for sample-exact output).

## Music

`pcmmix_music_play()` plays a track at 1.0 pitch and automatically
crossfades (0.5 s) from whatever is playing: the old track fades out while
the new one fades in on the second slot. Calling it again with the track
that is already playing just retargets its volume and loop flag. Tracks
are identified by their frames pointer. `pcmmix_music_stop(fade_seconds)`
fades everything to silence, and `pcmmix_music_current()` reports the
frames pointer of the track currently playing or fading in.

## Generator hook

`pcmmix_set_generator()` installs a mono block callback for live synthesis —
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

For a stereo source, set `options.channels = 2` and install
`pcmmix_set_generator_interleaved()`. Its zero-filled buffer contains
`frames * channels` samples in frame-major order. Existing mono clips, music,
and legacy generators are centered into both output channels. The selected
channel count is passed to every built-in sink and is included in pipe-latency
accounting. `pcmmix_get_stats()` reports coherent rendered-frame, write,
timeout, and sink-failure counters.

## WAV loader

`pcmmix_wav_load()` is deliberately strict: PCM, mono, 16-bit, 44100 Hz
only. An asset that does not match is a bug in whatever generated it, and
silently resampling would hide that. Failures return NULL with a
human-readable reason (`"sfx/hit.wav is 2ch/16bit/44100Hz, expected
1ch/16bit/44100Hz"`) written into the caller's error buffer. The parser
streams RIFF chunks within the declared RIFF boundary, validates chunk
padding, duplicate format/data chunks, PCM byte rate and block alignment,
and even sample-byte counts rather than assuming a canonical 44-byte
header. Files are capped at 64 MiB, and only the decoded output buffer is
allocated.

## Offline rendering and testing

`options.offline = true` starts the mixer with no sink and no thread; the
caller pulls the master mix with `pcmmix_mix_block()`, which renders the
exact fill the transport thread would stream. This is how the unit tests
assert on mixer output without any audio hardware, and it doubles as a
bounce-to-disk path. For transport-level tests, `options.sink_argv` can
point at any PCM-consuming command. Calling `pcmmix_mix_block()` on a
transport-backed mixer returns silence without advancing live voice, music,
or generator state.

## Performance checks

`make benchmark` builds deterministic mixer and WAV-loader microbenchmarks.
The mixer benchmark covers idle output, unity-pitch and resampled voices,
soft and hard clipping, a saturated 16-voice mix, and bank selection. The
WAV benchmark generates and repeatedly loads a strict fixture. Results are
reported as nanoseconds per frame, choice, or input byte so changes can be
compared on the same host; they are measurements, not pass/fail tests.

## Threading contract

`pcmmix_start()` and `pcmmix_stop()` belong to one control thread;
`pcmmix_stop()` must not race other calls, and after it returns nothing
touches the mixer object or sample memory. All other calls are
thread-safe against the mixer thread and each other. The generator
callback runs on the mixer thread. `pcmmix_mix_block()` is for offline
mixers; an accidental transport-mode call is safe but produces silence.

## Install

```sh
make install PREFIX=/usr/local
```

The build produces static and shared libraries. Applications may instead
compile `src/pcm_mixer.c`, `src/pcm_wav.c`, and `src/pcmmix_bank.c` directly.
The library has no dependencies beyond the C and POSIX system libraries.

An optional control-thread asset layer accepts WAV, OGG Vorbis, bounded SFZ
regions, and SF2 presets while keeping codecs and allocation out of the mixer:

```sh
make assets test-assets
```

`pcmmix_asset_load()` normalizes the selected asset to owned mono signed
16-bit PCM at the requested rate. It uses libsndfile for WAV/OGG and
FluidSynth for a bounded SF2 note render; the small SFZ resolver supports the
key, velocity, root-key, `default_path`, and relative sample opcodes needed by
curated banks. Link `libpcm-mixer-assets` with the `sndfile` and `fluidsynth`
pkg-config dependencies. The realtime mixer remains dependency-free when the
optional layer is not built.

The API is pre-1.0 and may change between minor releases. Version 0.3.0 adds
optional stereo transport, interleaved generation, and transport statistics
without changing mono defaults. Version 0.4.0 adds the separate bounded audio
asset layer without changing the core library ABI.

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
