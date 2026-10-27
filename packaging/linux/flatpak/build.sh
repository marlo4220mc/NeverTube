#!/usr/bin/env bash
#
# Build the Flatpak for nevertube.
#
# Run on a Linux box with flatpak + flatpak-builder >= 1.4 and appstreamcli
# >= 1.0 WITH its compose component (on Ubuntu 24.04 that is a second package,
# appstream-compose). In CI the runner is a plain ubuntu-24.04 with those from apt
# (see the build-flatpak job of .github/workflows/linux.yml, or linux-flatpak of
# release.yml): no container image
# and no privileges involved. Version 1.4 matters: older builders compose the
# AppStream metadata with a binary that lives INSIDE the SDK, and the SDK this
# manifest uses does not ship it any more (the preflight below explains it).
# The script installs the runtime and the SDK from Flathub if they are missing,
# builds the manifest, runs the OFFLINE selftests inside the build sandbox, and
# produces:
#
#   dist/flatpak/repo                       the OSTree repository (self-updates)
#   dist/flatpak/nevertube-<ver>-x86_64.flatpak   the single-file bundle
#   dist/flatpak/nevertube-repo-<ver>-<arch>.tar.gz   the repo, packed
#
# The bundle is the "install this file" deliverable; the repo is what
# `flatpak remote-add` points at so the app updates itself. Both come out of the
# same build. The tarball is the repo in ONE file: an OSTree repo is thousands of
# objects plus symlinks (refs/heads/*), and both lose something in a container
# that copies files around (GitHub's upload-artifact does not preserve
# symlinks), so the artifact and the future upload to a web server take the tar.
#
# Signing is optional: set FLATPAK_GPG_KEY=<key id> and the bundle and the repo
# are signed with it. Without a key, the repo has to be added with
# --no-gpg-verify (and that is what the README documents).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
cd "$repo"

APP_ID="io.github.marlo4220mc.NeverTube"
MANIFEST="packaging/linux/flatpak/${APP_ID}.yml"
RUNTIME="org.kde.Platform//5.15-25.08"
SDK="org.kde.Sdk//5.15-25.08"
OUT="${OUT:-$repo/dist/flatpak}"
BUILD="$repo/build-flatpak"
ARCH="${FLATPAK_ARCH:-x86_64}"
GPG_KEY="${FLATPAK_GPG_KEY:-}"

# A release build passes the version of the tag (NEVERTUBE_PKG_VERSION, set by
# .github/workflows/release.yml), which is the one that must appear in the
# bundle: the Flatpak ref uses it and so does the file name. The OSTree branch
# is derived from it too, so bumping the version creates a new ref to update to.
VERSION="${NEVERTUBE_PKG_VERSION:-$(sed -n 's/^project(nevertube VERSION \([0-9.]*\) .*/\1/p' CMakeLists.txt)}"
if [ -z "$VERSION" ]; then
  echo "ERROR: cannot read the version from CMakeLists.txt" >&2
  exit 1
fi

# The metainfo carries its OWN <release version> and nothing patches it: the ref,
# the branch and the file names take $VERSION, but the AppStream metadata keeps
# announcing whatever is written there (that is how a 1.0.0 bundle came out
# saying 0.2.0 in the software centre). The cores have to agree, so a dry run
# (1.0.0-dev.42) passes and a release of a bumped CMakeLists.txt without the
# metainfo stops here instead of publishing a version nobody can find.
META_VERSION="$(sed -n 's/.*<release version="\([^"]*\)".*/\1/p' "$here/${APP_ID}.metainfo.xml" | head -1)"
if [ "${VERSION%%-*}" != "${META_VERSION%%-*}" ]; then
  echo "ERROR: ${APP_ID}.metainfo.xml announces ${META_VERSION:-nothing} and this build is ${VERSION}." >&2
  echo "       Bump the first <release> of the metainfo together with CMakeLists.txt." >&2
  exit 1
fi

say() { echo; echo "== $* =="; }

say "checking flatpak"
command -v flatpak >/dev/null || {
  echo "ERROR: flatpak is missing (sudo apt install flatpak / sudo dnf install flatpak)" >&2
  exit 1
}
command -v flatpak-builder >/dev/null || {
  echo "ERROR: flatpak-builder is missing (sudo apt install flatpak-builder / sudo dnf install flatpak-builder)" >&2
  exit 1
}
flatpak --version
flatpak-builder --version

# flatpak-builder 1.2 (what Ubuntu 22.04 ships) composes the metainfo with
# `appstream-compose` INSIDE the SDK sandbox. freedesktop-sdk 24.08+ -- the base
# of org.kde.Sdk//5.15-25.08 -- dropped that binary, so the export step dies
# AFTER the whole build with "bwrap: execvp appstream-compose: No such file or
# directory". 1.4 composes it with `appstreamcli` on the host instead. Checking
# the pair up front turns a 15-minute wait into an error that says what to do.
# The output is not the same everywhere: the packaged builder prints
# "flatpak-builder-1.4.2" (a hyphen) and a locally built one prints
# "flatpak-builder 1.4.2" (a space), so the prefix is stripped either way.
FB_VERSION="$(flatpak-builder --version | sed -n 's/^flatpak-builder[ -]*//p' | head -1)"
version_at_least() { # $1 >= $2
  [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$2" ]
}
if [ -z "$FB_VERSION" ] || ! version_at_least "$FB_VERSION" 1.4.0; then
  echo "ERROR: flatpak-builder $FB_VERSION is too old for this manifest." >&2
  echo "       Its export step runs appstream-compose inside the SDK, and" >&2
  echo "       org.kde.Sdk//5.15-25.08 does not ship that binary any more," >&2
  echo "       so the build would fail at the very end. Use >= 1.4.0" >&2
  echo "       (Ubuntu 24.04, Fedora 40+, Arch) or org.flatpak.Builder." >&2
  exit 1
fi
if ! command -v appstreamcli >/dev/null 2>&1; then
  echo "ERROR: appstreamcli is missing, and flatpak-builder >= 1.4 composes the" >&2
  echo "       AppStream metadata with it (apt install appstream, or the appstream" >&2
  echo "       package of your distribution)." >&2
  exit 1
fi
# The subcommand is not enough: on some distributions appstreamcli only
# DISPATCHES `compose` to a helper binary. On Ubuntu 24.04 that helper is
# /usr/libexec/appstreamcli-compose, shipped in the appstream-compose package;
# without it the CLI exits with "AppStream assembly binary ... not found", which
# flatpak-builder reports as a failed export at the very end. Running the
# subcommand's help is the cheapest way to know the whole chain works, and its
# own message says what is missing.
ASC_LOG="$(mktemp)"
if ! appstreamcli compose --help >/dev/null 2>"$ASC_LOG"; then
  echo "ERROR: appstreamcli cannot compose: either it is older than AppStream 1.0" >&2
  echo "       (there was no 'compose' yet) or the compose component is not" >&2
  echo "       installed separately (Ubuntu 24.04: apt install appstream-compose)." >&2
  echo "       It said:" >&2
  sed 's/^/         /' "$ASC_LOG" >&2
  rm -f "$ASC_LOG"
  exit 1
fi
rm -f "$ASC_LOG"

# Flathub is where the runtime and the SDK live. In CI the image already has the
# remote; --if-not-exists keeps it idempotent.
say "flathub remote (user)"
flatpak remote-add --user --if-not-exists flathub \
  https://dl.flathub.org/repo/flathub.flatpakrepo

say "runtime and SDK"
flatpak install --user -y --noninteractive flathub "$RUNTIME" "$SDK"

say "build"
rm -rf "$OUT"
mkdir -p "$OUT"
# --disable-rofiles-fuse: rofiles-fuse is not set up on the CI runner and it is
# not needed anywhere else either. It costs some speed (the tree is copied
# instead of hard-linked from a read-only overlay) but it is the only mode that
# works in a container without FUSE.
# --install-deps-from: installs anything the manifest declares that is missing.
flatpak-builder \
  --user \
  --force-clean \
  --disable-rofiles-fuse \
  --install-deps-from=flathub \
  --state-dir="$OUT/.flatpak-builder" \
  --repo="$OUT/repo" \
  "$BUILD" "$MANIFEST"

# ---------------------------------------------------------------------------
# Validation of the desktop integration. These are the files flatpak exports to
# the host, and a mistake there is invisible in the build (it only shows up as
# "the launcher has no icon").
# ---------------------------------------------------------------------------
say "validating the .desktop and the metainfo"
if command -v desktop-file-validate >/dev/null; then
  desktop-file-validate "$here/${APP_ID}.desktop"
else
  echo "[info] desktop-file-validate is not installed: skipped"
fi
if command -v appstreamcli >/dev/null; then
  # Informational on purpose: the appstreamcli of an older distro predates
  # AppStream 1.0 (it does not know <developer>) and would fail a metainfo that
  # Flathub accepts. The authoritative check is flatpak-builder-lint, which runs
  # when the app is submitted.
  if ! appstreamcli validate --no-net "$here/${APP_ID}.metainfo.xml"; then
    echo "::warning::appstreamcli validate failed (informational: it may be an old appstreamcli)"
  fi
else
  echo "[info] appstreamcli is not installed: skipped"
fi

# The installed tree has to carry the files under the APP ID (the native
# nevertube.desktop must be gone or the menu shows two entries).
say "installed tree"
for f in \
  "/app/share/applications/${APP_ID}.desktop" \
  "/app/share/metainfo/${APP_ID}.metainfo.xml" \
  "/app/share/icons/hicolor/256x256/apps/${APP_ID}.png" \
  "/app/bin/nevertube" \
  "/app/bin/ffmpeg"; do
  flatpak-builder --run "$BUILD" "$MANIFEST" sh -c "test -e '$f'" || {
    echo "ERROR: $f is missing in the sandbox" >&2
    exit 1
  }
done
if flatpak-builder --run "$BUILD" "$MANIFEST" sh -c "test -e /app/share/applications/nevertube.desktop"; then
  echo "ERROR: the native .desktop is still in /app, the menu would show two entries" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# The offline selftests, from INSIDE the sandbox (that is the point: they prove
# that the binary finds its libmpv/libav/Qt sqlite plugin in the runtime).
# QT_QPA_PLATFORM=offscreen: there is no display here.
# HOME=/tmp: flatpak-builder --run leaves HOME pointing at the build dir in some
# versions, and the DB/downloader tests need a writable place.
# ---------------------------------------------------------------------------
say "offline selftests (inside the sandbox)"
flatpak-builder --run "$BUILD" "$MANIFEST" sh -c '
  set -e
  export HOME=/tmp
  export QT_QPA_PLATFORM=offscreen
  for t in --selftest-db --selftest-discover --selftest-ssl \
           --selftest-grid --selftest-theme --selftest-glyphs \
           --selftest-i18n --selftest-dates --selftest-suggest-parse \
           --selftest-suggest-panel --selftest-suggest-gestures \
           --selftest-av1hls --selftest-variants --selftest-reels \
           --selftest-download --selftest-download-button \
           --selftest-download-folder --selftest-download-settings \
           --selftest-download-transport --selftest-download-resume \
           --selftest-single-instance \
           --selftest-video-url --selftest-search-url \
           --selftest-update \
           --selftest-settings-page; do
    echo "SELFTEST $t"
    /app/bin/nevertube "$t"
  done
'

# ---------------------------------------------------------------------------
# Deliverables.
# ---------------------------------------------------------------------------
say "bundle"
BUNDLE="$OUT/nevertube-${VERSION}-${ARCH}.flatpak"
sign=()
if [ -n "$GPG_KEY" ]; then
  sign=(--gpg-sign="$GPG_KEY")
fi
flatpak build-bundle ${sign[@]+"${sign[@]}"} --arch="$ARCH" "$OUT/repo" "$BUNDLE" "$APP_ID"

say "OSTree repo (static deltas)"
flatpak build-update-repo ${sign[@]+"${sign[@]}"} --generate-static-deltas "$OUT/repo"

# The repo as ONE file. An OSTree repository is thousands of small objects plus
# symlinks in refs/heads, and the things that carry it around (a CI artifact, a
# plain HTTP upload) do not always preserve symlinks. The tar is what travels;
# `tar -xzf` gives back the repo that `flatpak remote-add` expects.
say "repo packed as one file"
REPO_TAR="$OUT/nevertube-repo-${VERSION}-${ARCH}.tar.gz"
rm -f "$REPO_TAR"
tar -czf "$REPO_TAR" -C "$OUT" repo

say "done"
ls -la "$OUT" "$BUNDLE" "$REPO_TAR"
echo
echo "Bundle:   $BUNDLE"
echo "Repo:     $OUT/repo"
echo "Repo tar: $REPO_TAR"
echo
echo "Install the bundle:"
echo "  flatpak install --user $BUNDLE"
echo
if [ -n "$GPG_KEY" ]; then
  echo "Add the repo (signed):"
  echo "  flatpak remote-add --user nevertube $OUT/repo"
else
  echo "Add the repo (UNSIGNED; for testing only):"
  echo "  flatpak remote-add --user --no-gpg-verify nevertube $OUT/repo"
fi
echo
echo "Run:"
echo "  flatpak run $APP_ID"
