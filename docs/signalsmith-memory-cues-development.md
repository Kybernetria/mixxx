# Local integration record

> **AI-generated current-scope notice:** This personal experimental branch covers
> Signalsmith deck quality, linear scaling, and memory cues. PitchShift migration
> and latency investigation are intentionally out of scope; the existing effect
> and RubberBand consumers remain unchanged. SoundTouch BPM analysis was
> removed: Queen Mary analysis is used, with SoundTouch retained only as a
> historical plugin fallback that preserves existing valid beat grids; analysis
> is not forced and explicit import preferences remain. Historical failed gates
> and results below are preserved as history, not masked. Local final validation
> is pending actual host results; no unreported passes are claimed. **End of
> AI-generated current-scope notice.**

No commits or upstream submissions are authorized.

## Human listening checklist

Before treating deck changes as ready for live use, test with headphones and
multiple decks: toggle keylock and change key repeatedly; pitch naturally and
scratch; exercise loops, reverse playback and cue jumps; verify memory-cue
persistence and beatgrid preservation; change sample rate and restart, then
repeat the checks. Listen for glitches, timing shifts, stale cues and unexpected
state changes. This is a human DJ check, not a substitute for local validation.

## Starting point and baseline

- Upstream reference: existing `origin/main`, `414699c2f0e0a5bd1640c6bb527e81d75a523bec`.
- Reference only: PR23 `bc34f556affbbb947213a70c8038079ec45d80ad`.
- Worktree: `/var/home/kyvernitria/Applications/mixxx-signalsmith-memory-cues`,
  branch `local/signalsmith-memory-cues`.
- Container: distrobox `mixxxbox`; GCC 13.3, CMake 3.28.3, Qt 6.4.2.

Baseline commands (inside container, before behavioral changes):

```sh
cmake -S . -B build-baseline-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-baseline-debug --parallel 2
cmake --build build-baseline-debug --parallel 4
ctest --test-dir build-baseline-debug --output-on-failure --parallel 2
```

Application and test executable built. First two build invocations hit the
1200-second harness timeout; later invocation completed. CTest: 1300 passed,
1 failed, 38 upstream-disabled (1339 discovered). Failure: NewUi startup,
missing runtime QtCore QML module. Installed `qml6-module-qtcore`,
`qml6-module-qtqml`, `clang-format-18`, `pre-commit`, and optional
`libpipewire-0.3-dev` inside container. Reran:

```sh
ctest --test-dir build-baseline-debug --output-on-failure -R 'QmlSkins/QmlStartupSmokeTest.Starts/NewUi'
```

1/1 passed. No upstream assertions/features disabled or repaired.

## Reuse assessment, before implementation

Retain upstream EngineBufferScale boundary and linear path, transport policies,
read-ahead/logging, loops/cues/crossfades/slip/sync/reverse/visual accounting.
Use PR23's deterministic scaler tests as scenarios, not assertions that the
implementation is correct. Reuse immutable Signalsmith vendor headers only
following independent provenance verification. Do not import PR23's unrelated
beat-grid/downbeat, deployment, skin or application changes.

Rewrite the adapter: callback `setSignal` must only publish a scalar format
request; configuration allocation and destruction belong on a preparation
thread. Preserve source input prefixes and fractional accounting in persistent
pending state; distinguish cache unavailability from EOF. Bound read attempts
and process sizes; initialize every requested sample. Keep source consumption
separate from audible lookahead. Test stereo/stems, resampling, seeks,
reconfiguration, EOF and parameter changes during partial requests.

Do not reuse Seek30WorkerState's load-only cache. Memory cue commands must
capture their source-frame position and quantization at command time, carry a
track generation, and mutate Track/Cue off the callback. Synchronize against
shared Track membership/type/order, not a deck-local stale cache. Keep
`Memory=19`, unindexed, distinct from HotCue. Restrict editing actions to valid
memory actions rather than incomplete conversions. Existing CueDAO generic
integer persistence is reusable, but requires round-trip tests.

## Vendor verification

Downloaded directly from the two upstream codeload endpoints on 2026-10-01:

- Stretch `57b93f4e9206a089a45387eaa39bdc9f310d3308`, archive SHA256
  `ad02e24334438b203e81d44f6c9906f3c6773e90a4ea923bb3e73d15697187d6`.
- Linear `5668673560146a9cfe38c25315071e3fd68c8317`, archive SHA256
  `91d09ff4924c6958c70b2d182ec2553526fc301176652111b27cb08fc03f532e`.

Both match PR23's recorded digests. Core Stretch/FFT headers compared identical.
Copy dependencies from independently downloaded archives, preserve their MIT
licenses and source layout; do not modify vendor algorithms.

Audit notes: configure allocates; reset and processing resize/copy vectors within
configuration capacities. Allocation instrumentation is a mandatory check, not
an inference from reserve calls. `outputSeek` resets and performs preroll; it
must be bounded and included in deadline measurements. The PitchShift statement
is historical and superseded; existing effect and RubberBand consumers remain
unchanged and migration is out of scope. SoundTouch BPM analysis was subsequently
removed in favor of Queen Mary analysis. SoundTouch is retained only as the
historical plugin fallback, preserving existing valid grids; analysis is not
forced and explicit import preferences remain.

## Reproduced defects and subsequent corrections

The PR23 adapter was compiled unchanged in a temporary harness with a narrow
read-ahead API shim. Its format setup performed **38 C++ allocations**. A scripted
64-frame prefix followed by a cache retry delivered **256 frames while consuming
320 frames**. These are reproductions of the reference behavior, not passing
replacement gates. Logs: `/tmp/mixxx-reference-regressions-tests.log`.

A separate Qt 6.4.2 direct-signal ownership test disconnected an active functor
from another thread. Emission completion performed **two C++ frees** on the
emitter (`/tmp/mixxx-qt-ownership-tests.log`). Consequently, navigation no longer
rebuilds direct Cue observers. Track owns an atomic revision token; Cue mutations
advance attached Track tokens synchronously under their existing lock. Membership
changes also advance the revision. Shared Cues retain all current Track tokens;
removal, replacement and destruction detach tokens while the Track still owns
its token. Normal first attachment to an unpublished Cue does not allocate or
acquire another lock. Bulk `setCuePoints` installation remains off callback.

GUI navigation publishes an immutable snapshot holding a revision token through
one ready slot. Audio consumes it only with an empty retirement slot, validates
its generation, seek/navigation epochs and revision, then retires it without
copying ownership or deleting it. The GUI timer reclaims retired snapshots. At
most ready, retired and producer-in-progress snapshots exist. Cancellation is
checked when handing the request to the existing engine seek queue, not after
that accepted request has entered upstream transport handling.

The scaler worker now tracks adopted formats rather than only its last prepared
format, covering rapid A/B/A/B requests. Parameter classification uses upstream
`util_isfinite`, because the repository enables `-ffast-math` even in Debug.
Transpose bounds also protect the vendor's mapped-bin-to-int conversions.
Positive short reads have a separate budget from zero-progress recovery; tests
cover maximum engine buffers with 150-frame reads and extreme supported sample
rate ratios. Vendor algorithms remain unchanged.

Backend selection is published atomically and captured once per callback for
format preparation and activation. Signalsmith selections skip allocating format
preparation in inactive legacy scalers. Crossfade reads reject a mismatched old
layout. Legacy backend preparation and option mutation remain existing legacy
paths, not a claim of callback compliance for those implementations.

## Current verification ledger

- An earlier full feature Debug suite passed 1,334 tests; 38 upstream tests were
  disabled. Later coverage increased the suite and found one unavailable-ID/NaN
  compatibility failure. The explicit finite guard was added afterward.
- The first subsequent targeted run stopped on a teardown fixture that created
  duplicate controls. The fixture now uses a separate group and a retained
  input proxy. This failed run is not counted as a successful current suite.
- Latest source-level focused review of `/tmp/mixxx-revision-review.patch` found
  no actionable defect. It did not run builds or tests and is not readiness
  approval.
- All-changed-files hooks passed in `/tmp/mixxx-pre-commit-revision-2.log`.
  Vendored source whitespace is preserved and excluded from formatting hooks.
- Current application/test builds succeeded in Debug and Release. Following the
  controller lifetime fixture correction, full CTest passed **1,360/1,360** in
  Debug (230.82 s) and Release (227.29 s); 38 upstream-disabled, one opt-in
  workload skipped. Logs: `/tmp/mixxx-debug-teardown-fix-full.log` and
  `/tmp/mixxx-release-teardown-fix-full.log`.
- The earlier targeted Debug abort and Release teardown segfault exposed a
  fixture violating ControlObject lifetime: its direct private-value forwarding
  can enter a QObject wrapper during destruction. The Memory command gate does
  not protect that wrapper. CoreServices destroys ControllerManager before
  EngineMixer; the fixture now joins the controller producer first. A separate
  concurrent test mutates Cue positions while destroying a control holding a
  navigation snapshot. This does not claim nonquiesced QObject destruction is
  supported. Audio/reader/controller quiescence remains an owner precondition.
- ASan/UBSan application/tests built. The first focused run found a test-only
  allocation-family mismatch: instrumented delete used free while nothrow new
  remained ASan-provided. Instrumentation now supplies matching scalar, array,
  nothrow and aligned families with a direct family regression. No sanitizer
  suppression was added. **60/60** focused tests pass with leak detection and
  undefined-behavior halt enabled, in 58.604 s:
  `/tmp/mixxx-sanitizers-allocation-fix-targeted.log`. Updated Debug focused tests
  also pass **60/60**. After formatting and the allocator family update, final
  full Debug and Release suites pass **1,361/1,361** (238.71 s / 245.41 s), with
  38 upstream-disabled and the opt-in workload skipped. Final ASan/UBSan focused
  run again passes **60/60**, leak detection enabled, in 60.406 s. Logs:
  `/tmp/mixxx-final-debug-full.log`, `/tmp/mixxx-final-release-full.log`,
  `/tmp/mixxx-final-sanitizers-targeted.log`. All-changed-files hooks pass in
  `/tmp/mixxx-pre-commit-validation-final-2.log`.
- The matched workload uses three decks, 1,000 measured callbacks, warmup,
  source-progress and finite-output checks for all three decks, and only the
  samples processed in that callback. It reports deadline percentiles, overruns,
  stalls, silent deck-buffers and peak RSS. Ninety quiet, matched Debug runs
  completed: upstream backends 0/1, feature 0/1/5, buffers 128/512/1024 samples,
  seek periods 0/50, three repetitions. Data:
  `/tmp/mixxx-matched-workloads-quiet/results.csv`. Earlier under-build-load
  runs are not used as comparative gate evidence. Both build types compile DSP
  with `-O3`; Debug additionally retains assertions/debug information.

### Failed seek deadline gate

Signalsmith has no silent/stalled deck-buffers in these runs, and no steady-state
software deadline overruns. With all three decks seeking every 50 callbacks,
128- and 512-sample buffers each have **20/1,000 overruns in all three repetitions**.
The 1024-sample workload has zero. Three additional Release repetitions per
buffer/seek case reproduce the same result. Release Signalsmith seek p99 ranges:
8.168–8.180 ms (128 samples, 1.451 ms deadline), 8.541–8.602 ms (512 samples,
5.805 ms deadline), 9.453–9.564 ms (1024 samples, 11.610 ms deadline).
Do not switch defaults or remove alternatives on this evidence.

An isolated vendor harness initially used the wrong preset split-computation
flag; its results are not matched adapter attribution. Corrected
`/tmp/mixxx-seek-profile/profile.cpp`, compiled/run inside mixxxbox with the
adapter's default stereo 44.1 kHz preset, split computation true, seed 0, tempo
1.25 and transpose/formant 1, reports five 1,000-operation batches:
reset median batch mean 47.94 µs; outputSeek (including its internal reset)
2,318.80 µs; first 256-frame process median 157.12 µs; normal process median
batch mean 153.34 µs. Log: `/tmp/mixxx-seek-profile/matched.log`. These isolate
vendor API costs, not full engine read-ahead/crossfade attribution.

The public vendor API does not expose resumable outputSeek's final compensated
preroll. An exclusive worker handoff could move it off callback without changing
vendor bytes, but would introduce asynchronous seek wait/silence and require
new transport/visual accounting and cancellation tests. Do not silently trade
seek timing failures for audible gaps.

A disposable adapter overlay was then linked against current Debug objects,
replacing only the adapter object in a temporary copied library. Nine real
EngineMixer workload runs (three each: 128 seek / 512 seek / 128 steady) confirm
vendor priming attribution: outputSeek averages 2.49–2.56 ms per event; separate
reset about 53–55 µs; reader calls about 1.8–2.2 µs each; deinterleave about
0.9–1.6 µs each; ordinary process about 154–159 µs. Seek p99 remains 8.235 /
8.594 ms, with 20/1,000 overruns; steady has none. All have zero stalled/silent
deck-buffers. Source, commands, logs and limitations:
`/tmp/mixxx-seek-profile/profile-report.md`. Clock reads and TLS records exist
only in disposable copies, not shipping code. An initial temporary executable
resource-discovery failure was fixed by temporary staging; its failed log is
retained. This isolates adapter/vendor costs, not every engine transport cost,
and is not a live device measurement.

## Asynchronous seek and deck migration checkpoint

The user authorized a brief fade/mute on the seeking deck. Seek input is
collected by the callback through its ReadAheadManager; the configuration and
frozen prefix are exclusively loaned to the existing worker for preroll.
Pending preroll consumes no source frames and publishes zero visual speed.
New seeks invalidate older preparation. Configuration reclamation stays off
callback. Tests cover held preparation, newer seeks, reverted formats,
shutdown and another deck advancing while the seeking deck is held.

Before legacy deck removal, full Debug CTest passed 1,366 tests. The focused
ASan/UBSan rerun passed 65 tests with leak detection and UB halt enabled
(`/tmp/mixxx-async-sanitizers-rerun.log`). Its first run exposed a test's fixed
startup-sleep assumption: the fractional-transport test now waits, with a
bounded iteration count, for actual delivered transport before measuring.
No transport assertion was weakened or sanitizer finding suppressed.

Six repeated 128-interleaved-sample, deadline-paced Debug workloads passed:
three with one seeking deck and three with all three decks seeking. All had
zero observed software deadline overruns. Callback p99 ranged from 389 to
648 microseconds and maximum from 459 to 770 microseconds, against a
1,451-microsecond deadline. Maximum seek-to-source-progress/nonzero-output
proxy delays ranged from 3.42 to 4.96 milliseconds. With only deck 1 seeking,
decks 2 and 3 had zero source stalls and silent buffers. These are software
measurements, not hardware or listening validation. Logs:
`/tmp/mixxx-async-gate-deck{0,1}-r{1,2,3}.log`.

The legacy deck SoundTouch/RubberBand adapters and deck RubberBand worker
pool were then removed, along with their Qt/QML selector and dual-threading
settings. Historical IDs 0 through 5 remain stable; only Signalsmith (5) is
available, and legacy/invalid selections resolve to it. Linear scaling and
transport policies remain. The migration worker reports a successful Debug
build and 1,366-test full CTest run; final-tree Release, sanitizers, hooks and
broader repeated workloads remain to be independently rerun.

### Historical PitchShift proposal (superseded; explicitly out of scope)

This paragraph records an earlier request and is retained only for historical
context. It is not current scope or a completion gate. The existing PitchShift
effect, all consumers and RubberBand dependency stay unchanged; no migration or
latency investigation is being continued. SoundTouch BPM analysis is removed;
the library remains only for historical plugin fallback behavior that preserves
existing valid grids, without forcing analysis and while retaining explicit
import preferences.

Inspection found two necessary host fixes, not just a DSP substitution:
EngineEffect currently initializes states with a placeholder 96 kHz format
without dispatching subsequent format changes, and EngineEffectChain uses
one dry-delay history across routes and modifies its input during compensation.
A replacement must establish off-callback format preparation, route-isolated
dry histories and input immutability. Vendor bytes must remain unchanged.

The historical probe measured 20–150 ms configurations, processing cost,
synthetic quality, fixture renders and an intentionally unaligned transition;
see the [archived PitchShift investigation](pitchshift-investigation.md). Its
lower-latency settings are not candidates for future work. The 120/30 ms default
quality preset remains; Linear is intended for natural pitch changes and
scratching. PitchShift and its RubberBand consumers remain unchanged.

The approved existing public fork `Kybernetria/mixxx` has immediate parent
`0cwa/mixxx` and network source `mixxxdj/mixxx`. The experimental branch remains
cleanly based on upstream `414699c2f0e0a5bd1640c6bb527e81d75a523bec`, separate
from combined `0cwa` history. The existing `personal-experimental` remote points
to that fork; `origin` remains official. No commit, push, release, PR, or message
has been made. See [experimental fork procedure](experimental-fork.md).

## Current validation status

## Final local software validation (2026-10-02)

Validated the revised tree in `mixxxbox`, without personal settings or databases:

- Full Debug: **1,381 passed**, 236.19 seconds.
- Full Release: **1,381 passed**, 233.39 seconds.
- Focused ASan/UBSan with leak detection: **92 passed**, 107.28 seconds,
  plus **2 DAO beat-grid tests passed** in a separate 3.03-second run;
  no reported sanitizer errors. This includes the analyzer/preferences, memory
  cues, scaler/reader, QML effects proxy and Echo/Reverb routing checks.
- Changed-file repository hooks passed. Direct SoundTouch dependency discovery,
  linking, version reporting and build/package requirements were removed.
  Release `ldd` retains RubberBand and shows no SoundTouch library. All 15
  vendored Signalsmith files remain byte-identical to the audited snapshots.
- Original PitchShift and Echo/Reverb processors remain unchanged. The default
  deck quality preset remains **120/30 ms**; natural-pitch playback and scratching
  retain the linear path. Historical deck IDs still normalize to Signalsmith.

The first expanded sanitizer run exposed fixture errors, not accepted results:
independent-pitch startup used a fixed 16-callback wait, and effects tests omitted
application-clock startup. A subsequent leak report identified unacknowledged
engine messages in the existing GUI-only QML effects fixture. Tests now await
observable transport delivery, initialize the application clock before effects
setup, and complete the test fixture's engine-message handoff before destruction.
Assertions and leak detection were not disabled. Earlier failed logs are retained.

### Repeated callback workloads

**108 quiet runs**, 1,000 callbacks each: Debug and Release; three repetitions
of steady playback, only deck 1 seeking, and all three decks seeking every 50
callbacks; 128/512/1,024 interleaved samples; Echo/Reverb with headphone routing
both disabled and enabled. All runs used Signalsmith ID 5, stereo 44.1 kHz,
three decks at tempo 1.25, and deadline-paced callbacks.

| Interleaved samples | Callback deadline | Maximum observed callback | Maximum seek-output proxy |
| --- | --- | --- | --- |
| 128 | 1,451.25 µs | 1,387.60 µs | 8.38 ms |
| 512 | 5,804.99 µs | 1,622.67 µs | 12.68 ms |
| 1,024 | 11,609.98 µs | 3,015.47 µs | 13.98 ms |

Observed overruns: **zero / 108,000 callbacks**. Steady runs reported no source
stalls or silent deck buffers. When only deck 1 sought, decks 2 and 3 reported
no stalls or silent buffers. No seek was canceled or unfinished in these runs.
Seeking decks intentionally stall/mute while worker preparation runs. The
smallest-buffer maximum leaves only about **64 µs** of measured margin: this is
not sufficient evidence for live-device safety under additional load.

The seek-output measure requires delivered source progress and nonzero deck
output; it is **not** true impulse, effects-tail, device or listening latency.
The workload measures the software mixer call, not hardware wake-up jitter,
controller input latency or a complete device deadline. C++ allocation counters
and source review do not establish absence of all C allocations, locks or I/O.
Hardware/listening approval, stem-heavy/device-rate workloads and driver xrun
checks remain human gates, not claimed successes.

Reproduction adds `MIXXX_STRETCH_BENCHMARK_EFFECTS=1` to the existing workload
command for Echo/Reverb and headphone routing. Set the seek period to 0 for
steady playback; set `MIXXX_STRETCH_BENCHMARK_SEEK_DECK=1` for only deck 1, or 0
for all decks. Run without concurrent builds and repeat each configuration.

Logs: `/tmp/mixxx-revised-final-{debug,release}-full-5.log`,
`/tmp/mixxx-revised-final-sanitizers-tests-4.log`,
`/tmp/mixxx-revised-final-sanitizers-dao.log`,
`/tmp/mixxx-revised-final-hooks-4.log` and
`/tmp/mixxx-revised-final-workloads/` (including `results.csv`). The reproducible
workload is checked in as `src/test/stretchcallbackworkload_test.cpp`; archived
PitchShift sources are under `tools/developer/pitchshift-investigation/`.
New audio renders, executables, logs and generated CSVs are not added to Git.

Earlier 1,366-test/65-test checkpoints and synchronous seek failures above are
historical, not substituted for these results. PitchShift migration/latency work
is intentionally archived and out of scope, not a remaining completion gate.
The fork remote is approved and configured; no commit, push, hosted CI run,
release or upstream message has been made. Human headphones/DJ checks in the
checklist remain outstanding. **End of AI-generated documentation.**
