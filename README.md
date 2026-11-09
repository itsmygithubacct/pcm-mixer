# pcm-mixer

`pcm-mixer` is a small C11 library that mixes game sound on a background thread
and streams it as s16le PCM to a command-line audio player (`pacat`, `pw-play`,
`aplay` or sox `play`), so a terminal game gets audio without an audio library.

It is not installed on its own. Kilix games and apps pin it as a Git submodule
(most of them through
[kilix-game-sdk](https://github.com/itsmygithubacct/kilix-game-sdk#readme)) and
compile it into their binaries; those games and apps install on demand from the
Kilix content catalog.

## What it does

- Plays pitch-shifted one-shots with voice stealing and held loops whose volume
  and pitch can change live.
- Crossfades music between two slots and fades it out on request.
- Runs a generator callback for live synthesis, mono or interleaved stereo.
- Outputs mono by default or interleaved stereo with a linear pan law.
- Groups samples into a cue and variant bank (64 cues, eight variants each) with
  ownership tracking and no immediate random repeats.
- Loads strict 16-bit mono 44100 Hz WAV files, and, through an optional separate
  library, WAV, OGG Vorbis, SFZ regions and SF2 presets.
- Renders offline with no sink and no thread, for tests and bounce-to-disk.
- Needs only POSIX and pthreads; the optional asset layer adds libsndfile and
  FluidSynth.

## Quick start

```sh
git clone https://github.com/itsmygithubacct/pcm-mixer.git
cd pcm-mixer
make            # build/libpcm-mixer.a, build/libpcm-mixer.so, build/beep
make test       # silent: renders offline, never starts a real player
./build/beep    # plays an audible test tone through the first working sink
```

```c
#include "pcm_mixer.h"

pcmmix mixer;
pcmmix_options options;
pcmmix_options_init(&options);
if (!pcmmix_start(&mixer, &options)) {
    /* no sink available: carry on silently */
}
pcmmix_play(&mixer, &explosion, 0.8f, 1.0f);   /* explosion: a pcmmix_sample */
pcmmix_stop(&mixer);
```

A program may instead compile `src/pcm_mixer.c`, `src/pcm_wav.c` and
`src/pcmmix_bank.c` directly.

## Usage

| Area | Main calls |
|---|---|
| Lifecycle | `pcmmix_options_init`, `pcmmix_start`, `pcmmix_stop`, `pcmmix_is_running`, `pcmmix_backend_name`, `pcmmix_get_stats` |
| Voices | `pcmmix_play`, `pcmmix_play_pan`, `pcmmix_loop`, `pcmmix_loop_pan`, `pcmmix_voice_set`, `pcmmix_voice_stop`, `pcmmix_voice_active` |
| Music | `pcmmix_music_play`, `pcmmix_music_stop`, `pcmmix_music_current` |
| Synthesis | `pcmmix_set_generator`, `pcmmix_set_generator_interleaved` |
| Samples | `pcmmix_wav_load`, `pcmmix_wav_free` |
| Cue bank (`pcmmix_bank.h`) | `pcmmix_bank_init`, `pcmmix_bank_load_variants`, `pcmmix_bank_take`, `pcmmix_bank_set_borrowed`, `pcmmix_bank_play`, `pcmmix_bank_clear` |
| Offline | `options.offline = true`, then `pcmmix_mix_block` |
| Assets (`pcmmix_asset.h`, optional) | `pcmmix_asset_load`, `pcmmix_asset_free` |

| Make target | What it does |
|---|---|
| `make` | static and shared libraries plus the `beep` example |
| `make test` | mixer and bank tests, offline and silent |
| `make sanitize`, `make tsanitize` | the tests under ASan/UBSan and under ThreadSanitizer |
| `make benchmark` | mixer and WAV-loader microbenchmarks |
| `make assets test-assets` | the optional asset library and its tests |
| `make install PREFIX=/usr/local`, `make install-assets` | install the libraries and headers |

## How it works

```
 game thread                          mixer thread
 ───────────                          ────────────
 pcmmix_play / loop / music ──lock──▶ voices + music slots + generator
                                       │ mix, pitch-resample, soft clip
                                       ▼
                                 s16le blocks ──pipe──▶ pacat | pw-play | aplay | play
```

`pcmmix_start()` probes the four players in order and starts the first one on
`PATH` that is still alive after about 80 ms, using `posix_spawnp()` with its
stdin connected to a close-on-exec pipe. On Linux the pipe is shrunk to about
`latency_ms` of audio and the mixer writes continuously, silence included, so
the player's blocking reads pace the mixer at a fixed latency. If the player
dies or stalls, the mixer winds down into a silent state and every playback call
becomes a no-op; the game keeps running. `pcmmix_stop()` reaps the player.

Samples belong to the caller: the mixer never copies or frees PCM. Handles are
generation-checked, so a stale voice handle cannot adjust a recycled slot. The
master bus soft-clips with `tanh` unless hard clipping is requested.

Depth: [docs/transport.md](docs/transport.md) for the sink and latency model,
[docs/api.md](docs/api.md) for voices, music, the generator hook, the WAV
loader, the bank, offline rendering and the threading contract.

## Documentation

- [docs/api.md](docs/api.md): the API guide, from the quick start to the
  threading contract.
- [docs/transport.md](docs/transport.md): sink probing, the pipe-paced latency
  model and sink failure.
- [docs/building.md](docs/building.md): build, test, sanitizer, benchmark,
  install and optional asset-layer targets.
- [CHANGELOG.md](CHANGELOG.md): release history.

## Related repositories

Projects that pin `pcm-mixer`:

- [kilix-game-sdk](https://github.com/itsmygithubacct/kilix-game-sdk#readme)
  pins it at `kilix-game-kit/third_party/pcm-mixer`, and `kilix-game-kit`'s
  audio layer (`kilix_game_audio.h`) is built on it. Games on the SDK get their
  sound through it: the games in
  [kilix-games](https://github.com/itsmygithubacct/kilix-games#readme),
  [pleb-tower](https://github.com/itsmygithubacct/pleb-tower#readme),
  [super-kilix](https://github.com/itsmygithubacct/super-kilix#readme),
  [pleb-plant-grower](https://github.com/itsmygithubacct/pleb-plant-grower#readme)
  and [kilix-land](https://github.com/itsmygithubacct/kilix-land#readme).
- [kilix-techno](https://github.com/itsmygithubacct/kilix-techno#readme) pins it
  directly at `third_party/pcm-mixer`.

[chip-sequencer](https://github.com/itsmygithubacct/chip-sequencer#readme)
renders chiptune music into one of this mixer's generator slots.

## Part of Plebian-OS

pcm-mixer is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It reaches a user compiled into the Kilix games and apps that pin it, which
install on demand from the Kilix content catalog. The map of every repository
and how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
