# Experimental personal fork: local review and upload

> **AI-generated documentation:** This is a personal experimental branch, not
> an official Mixxx release or upstream project direction. Commits and an ordinary
> push to the dedicated personal branch are explicitly authorized; releases, PRs
> and upstream messages are not. **End of AI-generated documentation.**

## Branch and fork

The working branch is `local/signalsmith-memory-cues`, based on clean upstream
commit `414699c2f0e0a5bd1640c6bb527e81d75a523bec`; it is separate from the `0cwa`
branch history. The approved existing public fork is `Kybernetria/mixxx`; its
immediate parent is `0cwa/mixxx` and its network source is `mixxxdj/mixxx`. Reuse
is intentional: the fork's immediate parent need not be the official repository
provided the source/network is official and this branch has clean upstream
ancestry. Do not create another fork, import the combined `0cwa` history, rewrite
old branches, or change fork-wide metadata/default branch.

The local `personal-experimental` remote points to
`https://github.com/Kybernetria/mixxx.git`; `origin` remains the official
`https://github.com/mixxxdj/mixxx.git`. The user has authorized committing and
publishing the locally validated implementation and focused refactors to the
branch below. Keep existing fork branches untouched. Never force-push or push
to `origin`; no release, PR or upstream message is authorized.

## Local CI

`.github/workflows/experimental-linux.yml` is prepared for the dedicated branch,
with a fork guard, Debug and Release full builds/tests, and a full isolated-temp
ASan/UBSan run. It is not performance-readiness evidence. Existing workflow
artifacts, licensing, and YAML are outside this documentation change; do not
alter them as part of fork setup. No hosted workflow result is claimed.

## Human review and upload

Review source, tests, vendor provenance/licenses, workflow and docs on the
intended branch. Stage only reviewed paths (never blanket `git add -A`), inspect
the staged names and complete diff, and run the repository hooks and relevant
local checks. Exclude build output, binaries, caches, credentials, personal
settings, audio renders, and databases. Keep PitchShift/RubberBand unchanged;
PitchShift migration and latency work are explicitly out of scope.

The following illustrates the approved review-and-upload procedure. Run commits
and pushes only after local checks and review, inside `mixxxbox` so installed
repository hooks use the same build environment. These commands do not create a
fork or change fork-wide settings:

```sh
cd /var/home/kyvernitria/Applications/mixxx-signalsmith-memory-cues
git status --short --branch
git diff --check
# Stage only individually reviewed paths/hunks; inspect every staged change.
git add -p -- CMakeLists.txt src res
git add -- README.md docs/experimental-fork.md docs/pitchshift-investigation.md \
  docs/signalsmith-memory-cues-development.md \
  tools/developer/pitchshift-investigation
# Only if these paths were individually reviewed and are intended for upload.
git diff --cached --name-status
git diff --cached --stat
git diff --cached
git diff --cached --check
# After reviewing and validating the staged state, commit the reviewed change.
git commit -m "Add experimental Signalsmith deck and memory cue support"
# Verify the destination, branch and commit before the authorized upload.
test "$(git remote get-url --push personal-experimental)" = "https://github.com/Kybernetria/mixxx.git"
test "$(git branch --show-current)" = "local/signalsmith-memory-cues"
git push --set-upstream personal-experimental HEAD:refs/heads/local/signalsmith-memory-cues
```

A push uploads the branch and may trigger configured branch CI. It does not
open a PR, publish a release, or assert readiness. Hosted CI results, artifact
review, and human DJ validation are separate. **End of AI-generated documentation.**
