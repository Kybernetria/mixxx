from pathlib import Path
import numpy as np
import soundfile as sf
import hashlib

root = Path("/tmp/mixxx-pitchshift-investigation")
sr = 44100
n = sr * 5
t = np.arange(n) / sr
# Deliberately separate steady tones for pitch-ratio measurement.
x = np.zeros(n)
for start, freq in [(0, 55), (1, 110), (2, 440), (3, 1760)]:
    a = start * sr
    b = a + sr
    x[a:b] = 0.25 * np.sin(2 * np.pi * freq * t[:sr])
# Last second is silent for state-tail inspection.
sf.write(root / "tones.wav", np.column_stack((x, x)), sr, subtype="FLOAT")
# Synthetic periodic vowel; not a recording or a listening-quality substitute.
f0 = 120
x = np.zeros(n)
for h in range(1, 81):
    freq = f0 * h
    envelope = sum(
        a * np.exp(-0.5 * ((freq - f) / width) ** 2)
        for f, width, a in [(600, 100, 1), (1200, 140, 0.6), (2500, 200, 0.4)]
    )
    x += envelope * np.sin(2 * np.pi * freq * t) / h**0.3
x *= 0.6 / max(abs(x))
x[: sr // 4] *= np.linspace(0, 1, sr // 4)
x[-sr // 4 :] *= np.linspace(1, 0, sr // 4)
sf.write(root / "vowel.wav", np.column_stack((x, x)), sr, subtype="FLOAT")
# Known transient with attack, short decay, and separate stereo impulses.
x = np.zeros((n, 2))
x[sr, 0] = 1
x[sr + 100, 1] = 1
z = np.arange(sr // 10) / sr
burst = 0.5 * np.exp(-60 * z) * np.sin(2 * np.pi * 90 * z)
x[2 * sr : 2 * sr + len(z), :] = burst[:, None]
sf.write(root / "transients.wav", x, sr, subtype="FLOAT")
fixtures = Path(
    "/var/home/kyvernitria/Applications/mixxx-signalsmith-memory-cues/"
    "src/test/stems/stem02"
)
for name in ["trance_mainmix", "04-vocal", "01-drum"]:
    f = fixtures / (name + ".wav")
    x, rate = sf.read(f)
    spectrum = np.abs(np.fft.rfft(x[:rate, 0]))
    share = np.sum(np.sort(spectrum)[-20:] ** 2) / np.sum(spectrum**2)
    print(
        name,
        "rate",
        rate,
        "seconds",
        len(x) / rate,
        "rms",
        np.sqrt(np.mean(x * x)),
        "spectral_top20_share",
        share,
        "sha256",
        hashlib.sha256(f.read_bytes()).hexdigest(),
    )
