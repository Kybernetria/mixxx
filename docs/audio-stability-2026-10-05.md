<!-- AI-generated implementation and validation record, prepared at the user's request. -->

# Audio stability follow-up (2026-10-05)

This follow-up continues the personal `local/signalsmith-memory-cues` branch
after `d81a6715b2d09f63ad4c995b65080c6c69bcde50`. Deck keylock still exposes
only Signalsmith. The earlier fixes and this work address confirmed defects;
they do not establish the cause of every reported hardware dropout.

## Callback and loading changes

- BPM matching, quantization, beat clocks and looping borrow immutable beat generations through fixed
  hazard slots. They no longer copy concurrently modified beat shared pointers
  or release beat ownership on the callback. Cross-deck phase matching no
  longer locks Track to retrieve its beat grid. Loop beat validity and the
  real/fallback beat flag are published together.
- Loop edits in callback scope publish scalar geometry. A 20 ms owner-thread
  timer persists the latest valid temporary loop cue. Track reloads and newer
  synchronous edits invalidate old requests. This removes callback Track/Cue
  metadata writes from held endpoints and saved-loop activation.
- EngineDeck captures one published channel count for rendering and stem
  downmix. EngineBuffer validates that layout under its existing nonblocking
  pause lock before updating scalers or processing samples. A stale layout
  receives silence instead of being interpreted with the new format. Stem
  vector bounds now compare available stems against the required stem count.
- Slip restoration is a callback-local fallback. An explicit cue queued for
  that callback takes priority rather than being overwritten by slip release.
- Off-callback track and beat notifications are serialized with track changes.
  Delayed notifications read current track metadata. BPM-lock signals from
  replaced tracks cannot update the new track's deck state. Audio loaded-state
  checks use the control value rather than a racing shared pointer.
- Beat undo releases the Track mutex before emitting beat notifications. Its
  undo-stack query is protected by that mutex. This avoids a lock-order cycle
  with concurrent track notifications and a racing undo-stack read.
- Reader requests, lifecycle messages, chunk results, engine notifications and
  queued GUI notifications now carry a request generation. An old open cannot
  resurrect an ejected track, replace a newer load or reset newer stem controls.
  Returning an old chunk cannot remove a newer mapping with the same index.
- Both cache containers reserve their full 80-entry capacity before playback.
  A sorted pointer vector replaces the allocating hash nodes; the free pool
  uses a reserved vector. Callback status draining processes only the initial
  bounded queue contents. A full worker status queue sleeps briefly and checks
  shutdown instead of spinning indefinitely.
- Saved-jump status and arm generation share one atomic token. Geometry edits
  invalidate captured decisions before publishing positions. Callback
  consumption cannot disarm a newer arm and does not emit Qt signals or write
  public status controls. One owner-thread timer projects status every 20 ms;
  public lights may take longer to update when the GUI is blocked.
- A control-ring reader previously decremented a writer-owned zero counter,
  wrapping it to the maximum unsigned value. Another reader or writer could
  then enter during a payload write. Bounded compare/exchange reader acquisition
  now leaves zero unchanged, preserving exclusive writer ownership.

## Sanitizer defects

The previous full Debug and Release jobs passed. Its ASan/UBSan job reached
1,255 of 1,447 tests before the two-hour limit and reported real errors:

- History cleanup called back into a Library whose derived lifetime had ended.
  Library now destroys its owned history feature while its members remain live.
- ColorMapper tests leaked their JavaScript engines; they now own them.
- Renderer tests leaked borrowed thread controls; the fixture now owns them
  and keeps them alive until each renderer has stopped and joined.
- PortMidi tests retained device strings from an expired constructor stack;
  the mock device descriptions now have static storage.
- Unstarted performance timers subtracted the minimum clock sentinel and
  overflowed. Elapsed/difference return zero while stopped; first restart
  starts the timer and returns zero.
- Initial read-ahead capacity growth passed null source pointers to a
  zero-length memcpy. Empty buffers no longer perform that copy.
- Failed MP3 header decoding left invalid libmad enum fields that logging
  subsequently read. Header checks now require successful decoding and failed
  headers are not inspected. A regression explicitly selects MAD so provider
  priority cannot bypass the damaged-file case.

The sanitizer workflow retains leak detection and the entire test suite. It
uses two test processes, line-level debug information, and a three-hour job
limit so build time does not prevent completing the tests.

Run `37270539583` completed all 1,472 registered tests. Full Debug and Release
passed. Sanitizers exposed seven failing tests: two additional renderer fixture
leaks, an expired command-line argument reference in the QML library fixture,
two fractional slip assertions and two QML startup leak reports. The fixtures
now retain the borrowed objects for their required lifetimes. Recovery discards
whole output frames, so its source-time residual is bounded by one output frame
at the current tempo; the integration assertions now use that derived bound and
separately check subsequent callback movement.

The startup reports also contain defects in the old system dependencies:
PortMidi's removed preference lookup leaks two path buffers, and Qt 6.4.2's
software render context does not release its texture containers on invalidation.
The upstream Qt fix is
[`f1b188df132c42da62197055725e5f7eebcc4249`](https://github.com/qt/qtdeclarative/commit/f1b188df132c42da62197055725e5f7eebcc4249).
Sanitizer CI pins Qt 6.8.3 and an instrumented PortMidi 2.0.7 with the existing
Flatpak archive checksum. It verifies selected libraries and runs repeated
PortMidi initialization/termination with leak detection. Debug and Release
continue testing the system dependency configuration. No leak suppression or
test exclusion is added.

## Validation and limits

Focused probes compile actual modified repository helpers and extracted
methods with small substitutes for unavailable Qt dependencies:

- Immutable snapshot tests pass ASan/UBSan and TSan, including eight readers,
  two publishers, retained generations, exhaustion and off-reader destruction.
- EngineBuffer format handoff passes ASan/UBSan and TSan across 200,000
  concurrent callbacks and loader format changes, with output canaries.
- Loop persistence passes UBSan with 1,001 publications and no callback model
  writes or allocations, including invalidation and latest-state draining.
- Actual previous timer and buffer sources fail their UBSan probes; the fixed
  sources pass. The previous slip code fails the queued-cue priority probe;
  the fixed code passes.
- Native regressions cover the transport, format, beat, persistence, shutdown,
  timer, buffer and decoder changes. Full native validation runs in GitHub CI.
- Extracted reader cache methods pass UBSan with one million turnovers, two
  million lookups and zero C++ allocations/deallocations. The actual status
  publisher exits with a full 128-entry FIFO and no consumer.
- The saved-jump source probe rejects 10,000 stale rearm/geometry decisions with
  zero callback allocations, deallocations, public status writes or Qt signals.
- The actual control-ring regression fails with the previous header and passes
  with the fixed header. Concurrent snapshots pass UBSan and TSan. These tests
  establish payload exclusion, not an ordering guarantee for concurrent writers.
- Signalsmith fractional recovery passes 48 deterministic cases and 432 checks
  in optimized and UBSan builds, covering stereo/stems, forward/reverse playback,
  fractional tempos and different callback sizes.

The new native regressions and dependency configuration still require the next
full GitHub CI run. Probe results are not a substitute for those native checks.

Local Qt/CMake builds and hardware listening are unavailable in this execution
environment. Leak scanning is unavailable for local ASan probes because `/proc`
access is restricted; GitHub CI still enables it. Helper tests do not prove
whole-application thread safety or device timing.

General seek-queue publication during callback consumption still needs a
command-ownership change: a seek queued during notification can be erased by
the callback's final queue clear. Signalsmith worker
latency, live-recovery budgeting and device underruns still require measured
playback and loading tests on the user's setup.

<!-- End of AI-generated implementation and validation record. -->
