# Archived experimental PitchShift bench (manual only)

> **AI-generated archival notice:** These scripts preserve historical exploratory
> evidence; they are not production code, CI, a migration plan, or permission to
> change PitchShift. Do not run the investigation unless a human explicitly
> invokes `run.py --run`. PitchShift and existing RubberBand consumers remain
> unchanged. **End of AI-generated archival notice.**

The source set is retained for reproducibility: `probe.cpp`, `timing.cpp`,
`render.cpp`, `transition.cpp`, `generate.py`, `analyze.py`, and
`transients.py`. Their recorded measurements and limitations are documented in
[`docs/pitchshift-investigation.md`](../../../docs/pitchshift-investigation.md).
These probes are not production effect or CI code. In particular, they do not
establish listening approval, end-to-end host latency, or hardware readiness.

## Environment and inputs

Original measurements used GCC 13.3 with `-O3 -DNDEBUG` inside the `mixxxbox`
distrobox, vendored Signalsmith Stretch headers from `lib/signalsmith-stretch`,
Mixxx headers under `lib`, and system RubberBand (`-lrubberband`) and
libsndfile (`-lsndfile`). Python probes require NumPy, SciPy, and soundfile.
`generate.py` and `analyze.py` use the repository's three existing upstream
fixtures under `src/test/stems/stem02/` (`trance_mainmix.wav`, `04-vocal.wav`,
`01-drum.wav`); the generated tones, vowel and transient are synthetic. Fixture
content/licensing was not independently authenticated. No fixture audio,
renders, CSVs, binaries, or databases belong in Git or CI artifacts.

The record was captured against the experimental worktree at
`414699c2f0e0a5bd1640c6bb527e81d75a523bec` and references the probes' historical
output under `/tmp/mixxx-pitchshift-investigation`. Exact source copies live
here; their contents are retained, not reformatted. The runner patches paths in
staged copies only and leaves these archived originals untouched.

## Reproduce deliberately

Review the scripts first. Output defaults to
`/tmp/mixxx-pitchshift-archive`; choose a new, empty directory outside the
checkout if it already exists. The runner rejects output inside the repository
and refuses to overwrite any existing output directory. It stages rewritten
source copies and binaries/results only in that output directory. Staging and
compilation are opt-in too; `--run` is required to compile or execute the full
historical probe sequence.

```sh
distrobox enter mixxxbox -- bash -lc '
  /var/home/kyvernitria/Applications/mixxx-signalsmith-memory-cues/tools/developer/pitchshift-investigation/run.py \
    --repo-root /var/home/kyvernitria/Applications/mixxx-signalsmith-memory-cues \
    --output /tmp/mixxx-pitchshift-archive-2026-10-01 --run
'
```

Without `--run`, the runner only stages copies and prints the isolated output
location. With `--run`, it uses GCC C++17, `-O3 -DNDEBUG`, and the include/library
flags described above; it runs the historical timing, DSP/control, render,
synthetic generation, fixture rendering, analysis and transition probes. Expect
substantial runtime and generated media. The scripts contain no production
integration and their lower-latency configurations are not recommendations.

All output is disposable local experimental material. Keep it out of the source
checkout and do not publish fixtures or renders. **End of AI-generated archival
notice.**
