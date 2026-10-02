# Signalsmith snapshots

Fetched independently from upstream on 2026-10-01. Vendor sources are unchanged.
Only the header-only source portions are included; upstream MIT licenses are
preserved in each dependency directory. Stretch reports version 1.3.2.

| Dependency | Upstream | Commit | Codeload archive SHA-256 |
| --- | --- | --- | --- |
| Stretch | https://github.com/Signalsmith-Audio/signalsmith-stretch | 57b93f4e9206a089a45387eaa39bdc9f310d3308 | ad02e24334438b203e81d44f6c9906f3c6773e90a4ea923bb3e73d15697187d6 |
| Linear | https://github.com/Signalsmith-Audio/linear | 5668673560146a9cfe38c25315071e3fd68c8317 | 91d09ff4924c6958c70b2d182ec2553526fc301176652111b27cb08fc03f532e |

Verification procedure:

```
curl -fL https://codeload.github.com/Signalsmith-Audio/signalsmith-stretch/tar.gz/57b93f4e9206a089a45387eaa39bdc9f310d3308 -o stretch.tar.gz
curl -fL https://codeload.github.com/Signalsmith-Audio/linear/tar.gz/5668673560146a9cfe38c25315071e3fd68c8317 -o linear.tar.gz
sha256sum stretch.tar.gz linear.tar.gz
```

Digests match the reference PR23 record, independently downloaded here. Included
headers/licenses were copied from those upstream archives, not modified fork
sources. The portable Linear FFT is used; no platform FFT library is required.

Audit scope: configuration allocation, vector capacities, reset/preroll and
processing call paths, and callback input/output contracts. This is not an
independent cryptographic signature or a claim of exhaustive vendor review.
