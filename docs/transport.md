# pcm-mixer transport

How the mixer finds an audio sink, streams PCM to it and survives a sink that dies, moved here from the README.

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
