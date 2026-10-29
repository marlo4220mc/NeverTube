# Cutting a release

One commit, one release: `.github/workflows/release.yml` builds the eight
deliverables, computes their checksums and publishes the GitHub Release with the
notes of [`notes.md`](notes.md).

It is the fifth workflow of the repository, not a replacement: `linux.yml`,
`windows.yml`, `windows_x86_win10.yml` and `windows_x64.yml` still exist and
carry the same steps, and they are what you run when you only want to re-check
one platform (`gh workflow run linux.yml --ref main`, or a push that touches
that file). They never publish. What only `release.yml` can do is: build
**everything from one commit**, check the version, and publish.



| job | runner | produces |
|---|---|---|
| `version` | `ubuntu-24.04` | the version strings (nothing to download) |
| `linux-deb-2204` | `ubuntu-22.04` | `.deb` for 22.04 / Mint 21 / Debian 12 |
| `linux-deb-2404` | `ubuntu-24.04` | `.deb` for 24.04 / Mint 22 / Debian 13 |
| `linux-rpm` | Fedora container | `.rpm` |
| `linux-arch` | Arch container | pacman package |
| `linux-flatpak` | `ubuntu-24.04` + flatpak-builder | bundle + OSTree repo (tar) |
| `windows-win7-x86` | `windows-2022`, toolset v142 | the Win7 setup (FFmpeg engine) |
| `windows-win10-x86` | `windows-latest` | the Windows 10/11 32-bit setup (mpv i686) |
| `windows-win10-x64` | `windows-latest` | the Windows 10/11 64-bit setup (mpv x64) |
| `linux-online` | `ubuntu-22.04` | nothing: the online/GPU selftests, report only |
| `release` | `ubuntu-24.04` | renames, `SHA256SUMS.txt`, the GitHub Release |

The `release` job `needs` all eight builds: a tag that publishes with one of
them missing is not possible. Publishing in one file is what makes that
guarantee: the per-platform workflows are the same builds, but no one of them
can publish.

## 1. Set the version

The version of a release is decided by the **tag**, and its core has to be the
version of `CMakeLists.txt`:

```sh
grep -n "project(nevertube VERSION" CMakeLists.txt   # 1.0.0
```

The `version` job stops with an error in the first seconds if they disagree, so
an inconsistency costs ten seconds and not an hour of builds. To cut `1.1.0`
first bump `CMakeLists.txt`, commit it, and only then tag.

Two more places carry a version **by hand**, and each is checked by the build
that would otherwise lie: the first `<release version="...">` of
`packaging/linux/flatpak/io.github.marlo4220mc.NeverTube.metainfo.xml` (what the
software centre shows; `packaging/linux/flatpak/build.sh` refuses to build a
Flatpak whose core disagrees with the version of the build) and the `pkgver` of
the published `packaging/linux/arch/PKGBUILD` (the AUR recipe; its source is
pinned to `v$pkgver` and `packaging/linux/arch/build.sh` refuses a pkgver that
disagrees with `CMakeLists.txt`, a placeholder or a moving branch). The packages
and the file names take their version from the tag and cannot drift.

**The app inside** does not: `NEVERTUBE_VERSION` (a CMake cache variable, default
`${PROJECT_VERSION}`) is compiled into the binary, and Ajustes, the About note
and the updater read it from there. The pipeline passes the display spelling
(`-DNEVERTUBE_VERSION=1.0.0-alpha.1`, the one the file names carry) to the five
builds that take it, and the Flatpak and the AUR recipe deliberately build
without it: their `pkgver` is the distro spelling (`1.0.0.alpha.1`), which is not
a version that was ever published, so the app would announce something that does
not exist. There is nothing to keep in sync for it; `settings.version` of the
catalogs is only the shape of the line (`NeverTube %1`).

Pre-releases carry the suffix after a hyphen: `v1.0.0-alpha.1`,
`v1.0.0-beta.1`, `v1.1.0-rc.1`. The suffix marks the release as a pre-release
automatically (GitHub hides it behind "Pre-release", and the notes get the
alpha banner). Each ecosystem gets the spelling it accepts:

| | `v1.0.0-alpha.1` |
|---|---|
| files, notes, Windows installers | `1.0.0-alpha.1` |
| dpkg / rpm | `1.0.0~alpha.1` (so it sorts *before* `1.0.0`) |
| pacman (`pkgver`) | `1.0.0.alpha.1` (`-` and `~` are not allowed there) |

## 2. Dry run (no release is created)

Before tagging, build everything from the branch you are on and look at the
artifacts. To redo a single platform, prefer the per-platform workflow of that
platform (`gh workflow run linux.yml --ref main`): it is the same build without
paying for the other jobs. This is the full pipeline:

```sh
gh workflow run release.yml --ref main
gh run watch
```

Without `publish` ticked nothing is published: the packages are left as
artifacts of the run (21 days), named `linux-deb-2204`, `linux-rpm`,
`nevertube-windows-x64-setup`, ... This is the mode to use while touching this
workflow or several packaging recipes at once. Add `-f online_selftests=true` if you also want
the report of the network/GPU selftests (they never fail the job).

The version of a dry run is `1.0.0-dev.<n>`, where `<n>` is one more than the
`v1.0.0-dev.*` tags that already exist (it is what keeps two dry runs from
publishing the same version, and deleting those tags starts the series over at
`dev.1`), so the packages do not pretend to be a release; pass
`-f version=1.0.0-alpha.1` to build exactly the bytes of the release you are
about to cut.

## 3. Cut the release

```sh
git tag -a v1.0.0-alpha.1 -m "NeverTube 1.0.0-alpha.1"
git push origin v1.0.0-alpha.1
```

The push of the tag starts the workflow and, when all eight builds are green,
creates the release. Re-running a failed tag (`gh run rerun <id>` or pushing the
tag again after `git tag -f`) **replaces the assets** of the existing release
rather than failing, so a flaky runner does not cost a new version number.

Publishing by hand without a tag also works: `gh workflow run release.yml
-f version=1.0.0-alpha.1 -f publish=true` creates `v1.0.0-alpha.1` on the
commit that was run.

Note that a push to `main` starts nothing: the per-platform workflows only
re-run when they themselves are touched, and `release.yml` only on a tag or by
dispatch. A change to a packaging recipe is therefore validated by dispatching
its workflow, not by the push.

## 4. The notes

The release body is [`notes.md`](notes.md) of the repository (English, with the
Spanish version inside a `<details>` at the end), reviewed like the rest of the
docs. `tools/release_notes.py` replaces `@@VERSION@@`, `@@TAG@@`, `@@DATE@@` and
`@@PRERELEASE_NOTE@@`, appends the `SHA256SUMS.txt` of that build, and **fails
if a placeholder is left**: a release body with `@@VERSION@@` on it is worse
than none.

To see the body before publishing:

```sh
mkdir -p /tmp/sums && echo "abc  NeverTube-1.0.0-alpha.1-Windows-10-11-x64-Setup.exe" > /tmp/sums/SHA256SUMS.txt
python3 tools/release_notes.py --version 1.0.0-alpha.1 --tag v1.0.0-alpha.1 \
  --date 2026-10-26 --prerelease --checksums /tmp/sums/SHA256SUMS.txt
```

What goes in them, when a release changes what the app does: the feature list
only when something is added or removed, the known limitations always (they are
what a first-time user hits), and the install commands if a file name changed.
The file names in the notes are the ones the `release` job produces; if one
changes, both move together or `sha256sum -c` in the notes stops matching.

## 5. After publishing

- `gh release view v1.0.0-alpha.1 --json assets --jq '[.assets[].name]'` must
  list the ten names (nine files + `SHA256SUMS.txt`).
- Download one and check it: `sha256sum -c SHA256SUMS.txt`.
- The Flatpak bundle (the `.flatpak` file) is **unsigned**: it installs, but
  there is no signature on it. The REPOSITORY is the thing that can be signed,
  and it is, when it is published (step 6). Before that, adding it by hand needs
  `--no-gpg-verify`.
- Windows installers are **unsigned**: SmartScreen warns on first run, and the
  update dialog says so. Signing needs certificates that the project does not
  have yet; it is documented and not pretended.
- The app's own updater offers this release to the installed copies (Windows):
  it reads `releases/latest`, compares with the version compiled into the binary
  and verifies the installer against `SHA256SUMS.txt`. A release whose installer
  asset matches neither its current name
  (`NeverTube-<version>-Windows-...-Setup.exe`) nor the legacy one
  (`nevertube-<version>-setup-<flavor>.exe`) -- or that carries no
  `SHA256SUMS.txt` -- is a release the updater refuses to install, saying why.
- `flatpak run io.github.marlo4220mc.NeverTube` on a real desktop is the one
  check that no runner can do (icon association by `FLATPAK_ID`, the database in
  `~/Documentos` of the host, `hwdec=auto` with `--device=dri`). It is in the
  manual checklist of `packaging/linux/flatpak/README.md`.

## 6. Publish the package repositories (optional, after the release)

`apt update` / `apt upgrade` and `flatpak update` need a REPOSITORY, and it is
published by a workflow of its own:

```sh
gh workflow run publish-repos.yml --ref main -f version=1.0.0
```

It downloads the `.deb` files, the Flatpak repository tar and `SHA256SUMS.txt`
**of that release**, verifies them against it, builds the APT trees
(`jammy` for 22.04, `noble` for 24.04: the two packages share their name and
version but not their dependencies), signs them and the Flatpak repository with
the project's key and commits the result to `gh-pages`, which is what GitHub
Pages serves. It is not part of `release.yml` on purpose: the release is
immutable, and a repository that fails to publish must not be able to modify or
block it. It never rebuilds anything, so what it serves is the bytes that were
tested.

What ends up served and can be handed to a user:

| file | what it is |
|---|---|
| `apt/key.asc` + `apt/dists/{jammy,noble}` | the signed APT repository (the `.deb` configures it by itself) |
| `flatpak/nevertube.flatpakrepo` | the Flatpak remote, with the public key |
| `flatpak/io.github.marlo4220mc.NeverTube.flatpakref` | **the one-file installer**: `flatpak install --user <that URL>` adds the remote and installs the app |

The Flatpak part signs the COMMITS of every app/runtime ref (`flatpak
build-sign`) and regenerates the static deltas from them before signing the
summary, because ostree verifies the commit — or the delta that carries it — and
a signed summary over unsigned commits fails at `flatpak install` with "GPG
verification enabled, but no signatures found". The job ends by **installing the
`.flatpakref` it just wrote** against a copy of the repository served from
localhost: the branch, the key and the signature are checked by flatpak itself,
so an installer that points at a branch nobody has cannot be published.

Two things are set up by hand, **once**:

```sh
# 1. a key for the project (no passphrase: it is a CI key and an empty one is
#    what makes it usable non-interactively)
gpg --batch --quick-generate-key "NeverTube packages <marlo4220mc@users.noreply.github.com>" rsa3072 sign never
gpg --list-secret-keys --with-colons | awk -F: '/^fpr:/ {print $10; exit}'   # the id

# 2. the two secrets, and then Pages (Settings -> Pages -> Deploy from a branch
#    -> gh-pages / root)
gpg --armor --export-secret-keys <id> | gh secret set GPG_PRIVATE_KEY
gh secret set GPG_KEY_ID --body "<id>"
```

The workflow refuses to start without them: metadata that says "signed" with no
signature is worse than no repository at all. Without a key it can still be run
locally for a throwaway test with `--unsigned-test`
(`packaging/linux/deb/repo.sh out/apt jammy=...deb --unsigned-test`), which
writes a file that says the tree is unsigned. What the users then do is printed
in the run summary (and `key.asc` is served next to the repository, so nobody has
to look the key up).

## What is a gate and what is a report

Every **gate** is offline: the thirty-odd `--selftest-*` that the recipes run
(db, grid, theme, glyphs, i18n, dates, reels, variants, downloads, single
instance, update, settings page...), plus the packaging smokes (install the package, `ldd`
clean, the selftests from `/usr/bin`), the byte-checks of the Windows payloads
(really 32/64-bit `dumpbin /machine`, the `.iss` guards, the Inno manifest) and
the Flatpak's install-tree validation inside the sandbox.

The **reports** are the ones that need the network or a GPU: `--selftest-tls`,
`-innertube`, `-search`, `-browse`, `-feeds`, `-home` and `-hwprobe`,
`-ffmpeg`, `-player`. YouTube can rate-limit a runner or answer a video without
a channel, and no runner has a GPU, so they run only behind
`online_selftests=true`, with `continue-on-error` and `::warning::`. A red mark
there says "look at it", never "the release is broken".

## If a job fails

| symptom | usually |
|---|---|
| `version` fails in seconds | the tag's core ≠ `CMakeLists.txt` |
| a Windows job dies in the import gate | a new DLL with W8+ imports (the gates list what is allowed) |
| `windows-win7-x86` dies on `qtaudio_wasapi.dll` | it was not purged from `dist/` (the plugin imports `RoGetActivationFactory`) |
| a Windows job dies on the Inno manifest | a `[Files]` edit left out one of the `$need` files (`libmpv-2.dll`, `nt.ico`, `ffmpeg.exe`, the redist...) |
| `linux-flatpak` dies in `appstream-compose` | `flatpak-builder` < 1.4 with a freedesktop-sdk ≥ 24.08, or `appstream-compose` missing (the preflight says which) |
| a package installs but the binary does not start | a missing runtime dependency: the `.deb` derives it with `dpkg-shlibdeps`, the `.rpm` from sonames, the `PKGBUILD` lists it by name |
| `release` cannot find an asset | an artifact name changed in a build job but not in the rename step |
| `linux-arch` fails validating the published PKGBUILD | its `pkgver` no longer matches `CMakeLists.txt` (they move together), or a placeholder/SKIP came back |
| `publish-repos` stops in the first seconds | the `GPG_PRIVATE_KEY` / `GPG_KEY_ID` secrets are not set (it says so) |
| the `.deb` mirror fails before building | the runner's `azure.archive.ubuntu.com` mirror was down: the jobs point apt at `archive.ubuntu.com` for that reason |
