<!-- AI-generated implementation and validation record, prepared at the user's request. -->

# Audio stall fixes (2026-10-04)

These changes follow the review of `608d73c539ecc2e9f5b91d6698e5daf9d0296058`
on the personal `local/signalsmith-memory-cues` branch. Signalsmith remains the
deck keylock implementation.

## Changes

- The shared reader scheduler retains a wake with a semaphore. Notifications
  are coalesced, and a wake arriving between a worker scan and sleep is preserved.
  This removes a lost-wake interleaving that can leave either or both readers
  waiting for unrelated work during playback or loading.
- A suspended crossfade read records the exact unavailable destination range.
  Read-ahead submits that range ahead of ordinary cache hints, including the
  samples preceding a forward loop/jump destination or following a reverse
  destination. Successful retries and explicit seeks clear the pending hint.
- Primary decks allocate their maximum eight-channel stem scratch buffer during
  construction. Stem processing no longer allocates or replaces that buffer
  inside the audio callback. Unsupported buffer sizes are rejected safely.

## Validation

Standalone probes compiled the actual scheduler/worker and read-ahead sources
with minimal Qt and reader/control substitutes. They compare the parent source
against the changed source; they are not full Mixxx or hardware tests.

- Injecting work after the scheduler scan and before sleeping leaves both
  parent-version workers asleep despite 100 subsequent callbacks. The fixed
  version wakes both and completes 10,000 additional two-worker wake cycles.
- All four combinations of stereo/eight-channel and forward/reverse playback
  remain suspended for 2,001 attempts in the parent version when the crossfade
  destination straddles a cache boundary. The fixed version supplies a hint for
  the missing range and resumes on the next attempt. Explicit seek cancellation
  of that hint is also checked.
- Equivalent scheduler and read-ahead regressions are included in `mixxx-test`
  and registered in the existing CMake test target.
- Both fixed-source probes also pass with UndefinedBehaviorSanitizer enabled
  and recovery disabled.

The cloud checkout has no Qt/CMake build environment, so the new native unit
tests and a complete application build have not been run here. The branch's
existing GitHub Actions workflow builds Debug, Release and ASan/UBSan variants
and runs their full suites after the push; their results must be checked.
The local ASan probe reached its functional assertions but could not complete
LeakSanitizer's exit scan because this execution environment denies `/proc`
access. This is not recorded as a sanitizer pass.

## Remaining limits

These repairs do not establish that every reported dropout is gone. The review
also identified audio-thread Track locking during saved-jump quantization,
slip restoration that does not follow the moving timeline during preparation,
and the already documented concurrent saved-jump disarm race. Those require
separate changes. Bounded Signalsmith live recovery can still extend silence
after a worker delay. Actual loading latency, device underruns and listening
quality still need verification on the user's DJ setup.

<!-- End of AI-generated implementation and validation record. -->
