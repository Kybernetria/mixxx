from pathlib import Path
import csv
import numpy as np
import soundfile as sf

p = Path("/tmp/mixxx-pitchshift-investigation")
sr = 44100
unity = {
    r["config"]: int(r["impulse_delay_frames"])
    for r in csv.DictReader(open(p / "quality-unity.csv"))
}
rows = []
for c, lag in unity.items():
    for q in [-2, -1, -0.5, 0.5, 1, 2]:
        for formants in [False, True]:
            y = sf.read(
                p
                / "renders"
                / "transients"
                / f"{c}_q{q:.6f}_f{int(formants)}.wav"
            )[0][:, 0]
            begin = sr // 2
            end = int(sr * 1.7)
            z = y[begin:end]
            e = z * z
            total = float(np.sum(e))
            cum = np.cumsum(e) / max(1e-30, total)
            lo = int(np.searchsorted(cum, 0.05))
            hi = int(np.searchsorted(cum, 0.95))
            peak = int(np.argmax(abs(z)))
            expected = sr + lag - begin
            rows.append(
                dict(
                    config=c,
                    octaves=q,
                    formants=formants,
                    energy90_width_ms=(hi - lo) * 1000 / sr,
                    peak_offset_from_unity_ms=(peak - expected) * 1000 / sr,
                    precursor_energy_fraction=float(
                        np.sum(e[:expected]) / max(total, 1e-30)
                    ),
                    peak=float(max(abs(z))),
                    total_energy=total,
                )
            )
with open(p / "quality-transients.csv", "w") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0]))
    w.writeheader()
    w.writerows(rows)
print(
    "Pitched unit impulse energy90 width: median / max ms, "
    "all ±.5/1/2 octaves, formants both"
)
for c in unity:
    r = [x for x in rows if x["config"] == c]
    print(
        c,
        np.median([x["energy90_width_ms"] for x in r]),
        max(x["energy90_width_ms"] for x in r),
    )
