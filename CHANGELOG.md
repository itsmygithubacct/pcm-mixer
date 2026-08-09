# Changelog

## 0.2.0 - Unreleased

- Preserve the public symbol set and structure layout while documenting the
  component and bank versions explicitly.
- Make transport startup safe for multi-threaded hosts with `posix_spawnp()`,
  close-on-exec descriptors, nonblocking writes, `poll()`, and a monotonic
  no-progress deadline.
- Limit `SIGPIPE` blocking to the transport thread and make sink failure an
  immediate, irreversible silent state until the mixer is stopped.
- Encode transport samples explicitly as little-endian and map hard-clipped
  output across the complete signed 16-bit range.
- Normalize non-finite controls, retire one-shots and non-looping music at
  their exact end, and prevent transport-mode offline rendering from
  advancing live state.
- Add idle and unity-pitch mixer fast paths.
- Replace whole-file WAV buffering with bounded streaming RIFF parsing and
  stricter size, padding, format, alignment, and PCM-length validation.
- Reject cue-bank ownership aliases while continuing to permit borrowed
  aliases and safe same-slot ownership updates.
- Enable strict warnings, generated dependency files, isolated sanitizer
  builds, expanded regression tests, and maintained mixer/WAV benchmarks.
