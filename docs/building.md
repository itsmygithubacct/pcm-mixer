# Building, testing and installing pcm-mixer

Build, test, benchmark and install instructions, including the optional asset layer, moved here from the README.

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

## Performance checks

`make benchmark` builds deterministic mixer and WAV-loader microbenchmarks.
The mixer benchmark covers idle output, unity-pitch and resampled voices,
soft and hard clipping, a saturated 16-voice mix, and bank selection. The
WAV benchmark generates and repeatedly loads a strict fixture. Results are
reported as nanoseconds per frame, choice, or input byte so changes can be
compared on the same host; they are measurements, not pass/fail tests.

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
