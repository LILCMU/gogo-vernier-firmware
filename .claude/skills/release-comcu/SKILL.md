---
name: release-comcu
description: Cut a co-MCU firmware release for this repo (vernier-firmware) and bundle it into the host gogo-firmware repo's `co-mcu-firmware/` directory — auto-summarises commits since the last release tag into release notes, copies the fresh `factory.bin`, updates `VERSIONS.txt`. Use when the user wants to ship a new vernier (or other co-MCU) firmware version without touching gogo-firmware manually. Triggers on "release vernier firmware", "release co-mcu", "ship co-mcu to host", "bundle vernier into gogo-firmware".
---

# release-comcu — bundle this co-MCU firmware into the host repo

This skill takes a freshly-tagged (or about-to-be-tagged) co-MCU
firmware build and pushes its artefacts into the host `gogo-firmware`
repo's `co-mcu-firmware/` directory so the next host stable release
ships them automatically.

## Layout this skill maintains in `gogo-firmware/`

```
gogo-firmware/
└── co-mcu-firmware/
    ├── README.md                                       # workflow doc (don't touch)
    ├── VERSIONS.txt                                    # manifest — one line per co-MCU
    ├── gogo-co-firmware-vernier.factory.bin            # bundled flash image
    ├── gogo-co-firmware-tasmota32c3.factory.bin
    └── release-notes/
        ├── vernier-v2.0.0.md
        └── tasmota-gogo-<sha>.md
```

`VERSIONS.txt` format — one line per co-MCU, columns separated by
double-space (or tab):

```
# repo            version              commit     date         binary
vernier           v2.0.0               4fcd645    2026-05-09   gogo-co-firmware-vernier.factory.bin
tasmota           gogo-4451f8f         4fdda6d    2026-05-09   gogo-co-firmware-tasmota32c3.factory.bin
```

The host CI's `github-release` job reads this file at release time and
embeds it (plus the matching `release-notes/*.md` content) in the
GitHub release description.

## Workflow

### 1. Preflight — refuse early if the environment is wrong

Run these checks in order. Stop and tell the user what's missing
instead of guessing.

- **cwd is a known co-MCU repo.** Detect via:
  - `vernier-firmware`: `platformio.ini` contains
    `FIRMWARE_FEATURE_FLAG="vernier"`. Bin name pattern:
    `gogo-co-firmware-vernier.factory.bin`.
  - `tasmota-build`: directory name is `tasmota-build` and there's a
    `build_output/firmware/gogo-co-firmware-tasmota32c3.factory.bin`.
  - Any other repo: refuse with the list of supported co-MCU repos.
- **Working tree is clean.** `git status -s` must be empty. Refuse if
  not — releasing from a dirty tree obscures what shipped.
- **Host repo is reachable.** Check `../gogo-firmware/co-mcu-firmware/`
  (default), or `$GOGO_FIRMWARE_REPO/co-mcu-firmware/` if the env var
  is set. If `co-mcu-firmware/` doesn't exist, refuse and tell the
  user to bootstrap it first (see "Bootstrapping" section at bottom).
- **Host repo branch matches the user's intent.** Don't auto-switch.
  Show the host's current branch and confirm the user wants to drop
  the bundle there; if not, stop.

### 2. Resolve the new version

- **vernier-firmware:** parse `FIRMWARE_MAJOR_VERSION`,
  `FIRMWARE_MINOR_VERSION`, `FIRMWARE_PATCH_VERSION` from
  `platformio.ini`. Format as `vMAJOR.MINOR.PATCH`. The release tag
  this skill will write is `version-MAJOR.MINOR.PATCH` (matching the
  host's tag convention).
- **tasmota-build:** there's no semver. Use `gogo-<short-sha>` as the
  version label, where `<short-sha>` is `git rev-parse --short HEAD`.
  The release tag this skill writes is the same string.

### 3. Find the previous tag for the changelog diff

`git describe --tags --abbrev=0` returns the latest reachable tag. If
the repo has no prior release tag (greenfield), fall back to the
initial commit and warn the user that the changelog will be the entire
history.

For tasmota-build, prefer the latest tag matching `gogo-*` (the
GoGo-specific build markers); the upstream Tasmota tags (`v15.0.0`
etc.) shouldn't be used as the diff base.

### 4. Build a fresh release artefact

Make sure `dist/<bin>.factory.bin` (vernier) or
`build_output/firmware/<bin>.factory.bin` (tasmota) exists AND was
built from the current HEAD.

- **vernier-firmware:** if `dist/gogo-co-firmware-vernier.factory.bin`
  is older than the most recent commit's timestamp, run
  `pio run -e release` and verify it lands a fresh file. The release
  env intentionally has no `FIRMWARE_DEBUG_FLAG`, so the artefact
  drops the `-debug` suffix.
- **tasmota-build:** the user does this build themselves
  (`build_output/firmware/gogo-co-firmware-tasmota32c3.factory.bin`).
  The skill should NOT trigger a tasmota build automatically — it's a
  multi-hour build with a vendored toolchain. Verify the file exists
  and is fresh; if not, ask the user to rebuild and re-invoke.

### 5. Generate the release notes

Run `git log <prev-tag>..HEAD --no-merges --pretty=format:'%h %s'`,
group commits by conventional-commit type (`feat`, `fix`, `refactor`,
`perf`, `docs`, `chore`, `build`, `test`, etc.), drop pure-docs and
pure-chore noise unless they're the entire changeset.

Format the output as `release-notes/<repo>-<version>.md`:

```markdown
# <repo> <version>

Released YYYY-MM-DD against host <host-version-or-branch>.

## Highlights
- One sentence per major user-visible change.

## Wire / behaviour changes
- Each protocol or NVS schema delta — call out anything the host firmware
  has to know about. Skip if there are none.

## Fixes
- Each `fix:` commit, one line. Drop if the entire changeset is
  features.

## Internal
- Refactors, perf, build changes — one paragraph rolled up.

## Commits
<full git log block, oldest-first>
```

The "Highlights" and "Wire / behaviour changes" sections are the
high-leverage parts the host release-notes consumer reads. The full
commit log goes at the bottom for auditability.

### 6. Copy the artefact

```
cp <co-mcu-repo>/<bin-path> <host>/co-mcu-firmware/<bin-name>
```

The bin name in the host directory is FIXED (no version suffix) so
the host CI's glob pattern stays simple. Version metadata lives in
VERSIONS.txt and the release-notes filename.

### 7. Update VERSIONS.txt

Read the existing file (preserve other co-MCUs' lines), replace this
co-MCU's row with the new version + commit + date + bin filename.
Keep the columns aligned for human readability. Don't sort — preserve
the existing row order.

### 8. Stage in the host repo, don't commit

```
cd <host>/co-mcu-firmware/
git add VERSIONS.txt release-notes/<repo>-<version>.md <bin-name>
```

Then in the conversation, tell the user:

- Which files were staged in the host repo.
- The exact commit message they should use (suggest:
  `chore(co-mcu): bundle <repo> <version> for next host release`).
- The release tag they should add to THIS co-MCU repo
  (`git tag <version-tag>` — show the exact command).
- A reminder that the host CI will pick the bundle up automatically
  on the next stable host release; no further co-MCU work needed.

DO NOT commit or push for the user — they review and commit themselves
in each repo.

## Bootstrapping (first run, before `co-mcu-firmware/` exists)

If the directory doesn't exist in the host repo, refuse the release
and tell the user to:

1. `mkdir -p <host>/co-mcu-firmware/release-notes`
2. Create `<host>/co-mcu-firmware/README.md` describing the workflow.
3. Create an empty `<host>/co-mcu-firmware/VERSIONS.txt` with the
   header line and any pre-existing bins listed.
4. Re-run this skill.

(The very first run after the gogo-firmware CI patch lands will be the
bootstrap — after that, every release flows through this skill.)

## Edge cases the skill must handle

- **No previous tag.** Use the initial commit as the diff base; warn
  in the release-notes body.
- **Dirty working tree.** Refuse — see Preflight.
- **Already-tagged HEAD.** If `git tag --points-at HEAD` returns a
  matching version tag, ask the user whether to overwrite the bundle
  for that tag (re-release) or stop.
- **Stale dist artefact.** Build mtime older than HEAD commit time →
  rebuild before copying.
- **Host repo on the wrong branch.** Show the branch and confirm with
  the user; never auto-switch.
- **Host repo dirty.** Refuse — the user might have unrelated work in
  flight; staging the bundle on top of dirt is rude.

## Why this exists

Before this skill: the host gogo-firmware release manager had to
remember to rebuild + copy two co-MCU firmwares every time a stable
gogo-firmware tag landed. Forgetting meant shipping stale co-MCU bins.

After: each co-MCU repo cuts its own release whenever it's ready;
the skill writes the artefact + notes into the host repo; the host
CI picks them up on the next stable tag without any cross-repo
attention from the host maintainer.
