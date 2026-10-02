# ARCHIVED: PitchShift latency investigation (historical record)

> **AI-generated scope notice:** This is a durable historical record only. The
> PitchShift migration and latency investigation are intentionally out of scope
> and are not unfinished completion gates or recommended next work. Keep the
> existing PitchShift effect and RubberBand consumers unchanged. **End of
> AI-generated scope notice.**

The original Signalsmith deck quality preset remains 120/30 ms. This archive's
lower-latency settings are not recommendations or candidates for further
investigation. Linear is the intended option for natural pitch changes and
scratching. All measurements and caveats below are retained as historical data,
not as authorization for production effect changes.

The historical investigation noted that fully bypassed audio was immediate;
switching between immediate bypass and delayed active audio changes timelines.
A fade can soften the switch but cannot make those timelines identical: the
recorded risks include cancellation, a short level dip, and skipped/repeated
timing. The measured transition experiment below is **not** a production
transition implementation or current request for follow-up.

This effect delay is separate from the deck seek worker wait. Earlier deck
software workloads measured a roughly 3–5 ms seek-to-new-output proxy; that is
not PitchShift latency.

## Method and scope

All probes, binaries, CSVs and audio renders are disposable files under
`/tmp/mixxx-pitchshift-investigation/`, not shipping source or Git additions.
The repository's PitchShift implementation, manifest and dependency remain
unchanged. Vendor sources remain unchanged.

GCC 13.3, `-O3 -DNDEBUG`, inside `mixxxbox`; portable vendored Signalsmith FFT,
seed 0, stereo, time ratio 1, split computation enabled. Impulse probes cover
44.1, 48 and 96 kHz. Repeated timing covers all six configurations at those three
rates, 128/512/1024 **interleaved samples** (64/256/512 frames), three repetitions
of 1,000 callbacks per case, with the first callback reported separately.
Timing includes scalar pitch/formant updates and planar conversion, but not
Mixxx's outer effect-chain/mixer callback or audio-device scheduling.

Pitch maps to `pow(2, pitch * range)` with the existing fractional-semitone
rounding. Formant preservation uses `setFormantFactor(1, preserve)` and automatic
base estimation. A direct API probe checks 72 scalar cases, rapid automation
and independent instances. This does not validate saved presets or host
parameter messages; those were migration checks in the historical study and
are not current completion gates.

RubberBand comparators use realtime mode, unity time ratio and the production
maximum process size of 8,192 frames. Both actual-rate construction and the
current EngineEffect placeholder 96 kHz construction are recorded. Comparator
output tails are explicitly initialized, unlike the existing effect's partial
retrieval path; these are DSP comparisons, not bit-for-bit production models.

## Measured latency and processing cost

Unity input-to-output impulse peak delay, including the DSP's input and output
latency, at 44.1 kHz. This is **not** an integrated Mixxx/device end-to-end test.
The 48/96 kHz Signalsmith impulse probes agree within sample rounding.

| Window / interval | Measured DSP delay | 44.1 kHz, 128 samples: p99 range over 3 runs | Largest callback in those runs |
| --- | ---: | ---: | ---: |
| Default 120 / 30 ms | 150.00 ms | 67–84 µs | 766 µs |
| Cheaper 100 / 40 ms | 140.00 ms | 41–49 µs | 419 µs |
| Manual 64 / 16 ms | 79.98 ms | 50–54 µs | 175 µs |
| Manual 32 / 8 ms | 39.98 ms | 36–44 µs | 56 µs |
| Manual 16 / 4 ms | 19.98 ms | 36–40 µs | 313 µs |
| Manual 32 / 4 ms | 35.99 ms | 59–65 µs | 96 µs |
| RubberBand, actual 44.1 kHz | 40.63 ms | 258–284 µs | 1,320 µs |
| RubberBand, placeholder 96 kHz | 87.07 ms | 164–560 µs | 1,867 µs |

The software deadline for the shown 64-frame, 44.1 kHz calls is 1,451 µs.
Across 162 Signalsmith rows / 162,000 calls, no measured DSP-call overruns were
observed. The RubberBand placeholder comparator had three overruns in the shown
128-sample runs. Other rates and block sizes are in `results-timing.csv`.
Repetitions have variability and scheduling outliers; this does not prove that
several effects plus decks fit a complete real-time callback.

Signalsmith first and subsequent automated processing recorded zero counted
C++ allocations/deallocations. RubberBand comparator rows recorded allocations
and emitted realtime window-allocation warnings. Counters do not cover C
allocators, locks, I/O, every production branch or device behavior.

## Audio-quality evidence, not listening approval

At 44.1 kHz, renders cover unity, ±0.5, ±1 and ±2 octaves with formants both on
and off, synthetic tones/vowel/transients and three upstream test fixtures:
`src/test/stems/stem02/{trance_mainmix,04-vocal,01-drum}.wav` (12.8 seconds each).
These fixtures are labelled music, vocal and drum material; their content and
individual licensing were not independently authenticated. They were used only
locally, and no audio fixture/render is added to Git or CI artifacts. Synthetic
vowel measurements are not a substitute for real sung-vocal listening.

All measured fixture renders were finite. Unity transient reconstruction SNR
for the Signalsmith candidates was 134–137 dB. Delaying the dry reference by the
measured DSP latency aligned the unity 50/50 mix with RMS error about 1e-9.
This validates the probe's steady-state alignment, **not** the host chain.

For output fundamentals at least 40 Hz, input tones 55/110/440/1760 Hz,
±0.5/±1 octave, the following are screening measurements, not perceptual scores
or approved thresholds. Tone frequency uses a windowed-FFT interpolated peak;
errors at low frequencies and extremes need longer/independent confirmation.

| Configuration | Formants-on absolute pitch error: median / max | Synthetic preserved-envelope correlation |
| --- | ---: | ---: |
| Default 120 / 30 | 2.7 / 27.1 cents | 0.920 |
| Cheaper 100 / 40 | 3.9 / 49.5 cents | 0.928 |
| Manual 64 / 16 | 7.6 / 72.3 cents | 0.880 |
| Manual 32 / 8 | 19.6 / 96.1 cents | 0.886 |
| Manual 16 / 4 | 31.5 / 173.4 cents | 0.303 |
| Manual 32 / 4 | 16.0 / 55.5 cents | 0.906 |
| RubberBand, actual rate | 8.4 / 155.2 cents | 0.643 |
| RubberBand, placeholder | 1.4 / 38.8 cents | 0.709 |

Envelope correlation compares smoothed log spectra with the intended preserved
or transposed synthetic envelope. It does not score intelligibility, stereo
image, musical phasing or perceived naturalness.

Pitched unit-impulse renders also measure temporal spreading: the interval
containing the middle 90% of impulse energy. Across ±0.5/±1/±2 octaves, formants
both on/off, median/maximum widths were 0.12/16.33 ms for the default preset,
0.33/4.31 ms for 32/8, 1.19/4.92 ms for 32/4 and 1.41/3.47 ms for 16/4.
RubberBand comparators measured 5.99/25.78 ms at the actual rate and
35.10/164.08 ms at the placeholder rate. This is an isolated transient metric,
not a perceptual attack-quality score; it shows another cost/quality tradeoff
rather than making the shortest window a winner.

Pitched music, vocal and drum A/B WAVs are available under `renders/`; nobody
has supplied a listening assessment. A live-DJ quality requirement is therefore
not satisfied.

## Automation and bypass experiment

A standalone 44.1 kHz probe covers the 20, 36 and 150 ms candidates, 2,000 counted
callback updates each. It observes finite output and zero C++ allocations/frees
for scalar automation. Reset was measured separately at about 2–53 µs with no
counted C++ allocation; that alone does not authorize callback reset in the host.
Independent-instance formant testing uses non-unity transpose, not a unity
case where compensation would have no audible effect.

The deliberately unaligned 20 ms blend from current dry to delayed wet and
back preserves bypass samples exactly in the simulation, but gives undesirable
transition level variation. For the 36 ms candidate the minimum measured
10 ms output/input energy ratio around the toggles was about 0.31 (roughly
−5 dB). Automation and the synthetic program material also affect this metric;
it is not a perceptual score or an isolated measurement of delay combing.
`transition-manual32_4.wav` illustrates the candidate tradeoff. The simulation
continues processing while bypassed; it does **not** validate host skip paths,
startup warmup or stale-history rejection after inactivity.

## Historical integration observations (not current work items)

The following observations document the investigated production boundary; they
are not an open completion list. Original PitchShift consumers, implementation,
controls and RubberBand use remain unchanged.

- Preserve `org.mixxx.effects.pitchshift`, parameter IDs/order/defaults/ranges,
  links, presets and indexed/ID-based automation. No control may disappear.
- Prepare/reclaim DSP states off callback for actual formats, including startup,
  routing and sample-rate changes. The historical implementation initializes
  states with a placeholder 96 kHz and has no subsequent preparation dispatch.
- Make dry compensation per input/output route and avoid modifying caller input.
  Current EngineEffectChain has shared delay history and mutates its input.
- Implement and test an approved immediate-bypass transition policy, warmup,
  repeated enabling, stale history, independent decks/headphones and stacked
  latency-bearing effects. Test both dry/wet mixing modes through the host.
- Validate C allocation, locks and I/O as well as C++ counters; measure complete
  mixer deadlines with decks and effects together, not just isolated DSP calls.
- The historical proposal listed SoundTouch beat-analyzer cleanup and further
  migration validation. SoundTouch BPM analysis has since been removed in favor
  of Queen Mary analysis; SoundTouch remains only as a historical plugin fallback
  preserving existing valid grids. Analysis is not forced and explicit import
  preferences remain.

## Durable source and artifacts

Exact archived sources are retained in
[`tools/developer/pitchshift-investigation/`](../tools/developer/pitchshift-investigation/).
The original probes and measurements used `/tmp/mixxx-pitchshift-investigation/`;
its WAVs, renders, CSVs, binaries and logs are not tracked. The durable source
README describes provenance, dependencies, fixture inputs and the manual-only
runner. Staged copies rewrite old checkout and `/tmp` paths without changing
these archival originals. The runner never executes probes unless the human
explicitly passes `--run`; see its README for the command. Do not add generated
audio, binaries, databases or results to Git or CI.
