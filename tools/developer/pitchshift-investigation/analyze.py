from pathlib import Path
import csv
import json
import numpy as np
import soundfile as sf
from scipy.ndimage import gaussian_filter1d

root = Path("/tmp/mixxx-pitchshift-investigation")
sr = 44100
configs = [
    "default120_30",
    "cheaper100_40",
    "manual64_16",
    "manual32_8",
    "manual16_4",
    "manual32_4",
    "rubberband_actual_rate",
    "rubberband_placeholder96k",
]


def rendered(fixture, c, q=0, f=False):
    return sf.read(root / "renders" / fixture / f"{c}_q{q:.6f}_f{int(f)}.wav")[
        0
    ][:, 0]


def spectrum(x):
    w = np.hanning(len(x))
    return abs(np.fft.rfft(x * w))


def frequency(x, target):
    s = spectrum(x)
    lo = max(1, int(target * 0.75 * len(x) / sr))
    hi = min(len(s) - 2, int(target * 1.3 * len(x) / sr) + 1)
    p = lo + np.argmax(s[lo:hi])
    z = np.log(np.maximum(s[p - 1 : p + 2], 1e-30))
    delta = 0.5 * (z[0] - z[2]) / (z[0] - 2 * z[1] + z[2])
    return (p + delta) * sr / len(x)


metrics = []
pitches = []
formants = []
sample_metrics = []
reference = sf.read(root / "transients.wav")[0][:, 0]
vowel = sf.read(root / "vowel.wav")[0][:, 0]
for c in configs:
    y = rendered("transients", c)
    lag = int(np.argmax(abs(y[: sr * 2]))) - sr
    x0 = reference[: len(reference) - lag]
    y0 = y[lag : lag + len(x0)]
    snr = 10 * np.log10(np.sum(x0 * x0) / max(1e-30, np.sum((x0 - y0) ** 2)))
    peak = sr + lag
    energy = np.sum(y[peak - 100 : peak + 101] ** 2)
    outside = np.sum(y[peak - 100 : peak + 101] ** 2) - y[peak] ** 2
    alignedmix = 0.5 * x0 + 0.5 * y0
    unitymixerr = float(np.sqrt(np.mean((alignedmix - x0) ** 2)))
    metrics.append(
        dict(
            config=c,
            impulse_delay_frames=lag,
            impulse_delay_ms=lag * 1000 / sr,
            unity_snr_db=float(snr),
            impulse_peak=float(y[peak]),
            impulse_off_peak_energy_fraction=float(
                outside / max(1e-30, energy)
            ),
            aligned_half_mix_rms_error=unitymixerr,
        )
    )
    for q in [-2, -1, -0.5, 0.5, 1, 2]:
        for f in [False, True]:
            y = rendered("tones", c, q, f)
            for second, freq in enumerate([55, 110, 440, 1760]):
                a = int((second + 0.3) * sr) + lag
                b = int((second + 0.75) * sr) + lag
                measured = frequency(y[a:b], freq * 2**q)
                cents = 1200 * np.log2(measured / (freq * 2**q))
                pitches.append(
                    dict(
                        config=c,
                        octaves=q,
                        formants=f,
                        input_hz=freq,
                        measured_hz=measured,
                        error_cents=cents,
                        rms=float(np.sqrt(np.mean(y[a:b] ** 2))),
                    )
                )
    for q in [-0.5, 0.5, 1]:
        for f in [False, True]:
            y = rendered("vowel", c, q, f)
            a = sr + lag
            n = sr
            out = spectrum(y[a : a + n])
            source = spectrum(vowel[sr : sr + n])
            # A broad log-spectral envelope suppresses the harmonic comb.
            # Not perceptual MOS.
            out = gaussian_filter1d(out, 60)
            source = gaussian_filter1d(source, 60)
            bands = np.arange(200, 3500)
            intended = bands if f else bands / 2**q
            expected = np.interp(intended, np.arange(len(source)), source)
            measured = np.log(np.maximum(out[bands], 1e-8))
            expected = np.log(np.maximum(expected, 1e-8))
            corr = float(np.corrcoef(measured, expected)[0, 1])
            formants.append(
                dict(
                    config=c,
                    octaves=q,
                    preserve=f,
                    intended_envelope_log_correlation=corr,
                )
            )
    for fixture in ["trance_mainmix", "04-vocal", "01-drum"]:
        original = sf.read(
            Path(
                "/var/home/kyvernitria/Applications/"
                "mixxx-signalsmith-memory-cues/src/test/stems/stem02"
            )
            / (fixture + ".wav")
        )[0][:, 0]
        for q in [0, 0.5, -0.5, 1, -1]:
            for f in [False, True]:
                y = rendered(fixture, c, q, f)
                z = y[lag : lag + len(original)]
                sample_metrics.append(
                    dict(
                        config=c,
                        fixture=fixture,
                        octaves=q,
                        formants=f,
                        finite=bool(np.isfinite(z).all()),
                        peak=float(max(abs(z))),
                        rms=float(np.sqrt(np.mean(z * z))),
                        relative_rms_db=float(
                            20
                            * np.log10(
                                max(1e-20, np.sqrt(np.mean(z * z)))
                                / max(
                                    1e-20,
                                    np.sqrt(np.mean(original * original)),
                                )
                            )
                        ),
                    )
                )
for name, rows in [
    ("quality-unity", metrics),
    ("quality-pitch", pitches),
    ("quality-formant", formants),
    ("quality-fixtures", sample_metrics),
]:
    with open(root / (name + ".csv"), "w") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
print(json.dumps(metrics, indent=2))
print(
    "\nPitch error cents: median absolute / max absolute over "
    "+/-0.5,1,2 octaves, 55/110/440/1760Hz, formants both"
)
for c in configs:
    errors = np.array(
        [abs(r["error_cents"]) for r in pitches if r["config"] == c]
    )
    print(c, np.median(errors), max(errors))
print("\nSynthetic envelope correlations: mean preserve / shifted")
for c in configs:
    print(
        c,
        *[
            np.mean(
                [
                    r["intended_envelope_log_correlation"]
                    for r in formants
                    if r["config"] == c and r["preserve"] == p
                ]
            )
            for p in [True, False]
        ],
    )
