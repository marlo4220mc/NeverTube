#!/usr/bin/env bash
#
# Build the Debian/Ubuntu package (.deb) for nevertube.
#
# Run on Debian/Ubuntu (the GitHub runner is ubuntu-22.04). Installs the build
# dependencies, configures + builds with CMake, runs the offline selftests
# (the "viability" gate) and assembles the .deb from the install tree that
# CMake already knows how to lay out (bin + .desktop + hicolor icons).
#
# The package Depends are DERIVED FROM THE BINARY with dpkg-shlibdeps, not a
# hand-kept list: on Ubuntu 24.04 the Qt packages are the ...t64 ones, on 22.04
# they are not, and a hardcoded list would be wrong on one of them.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
cd "$repo"

# git refuses to read a repo owned by another user (root inside a test
# container reading the runner's checkout); harmless on the runner itself.
git config --global --add safe.directory "$repo" 2>/dev/null || true

# On the GitHub runner this is a normal user with passwordless sudo; inside a
# root container (local testing) there is no sudo. Support both.
if [ "$(id -u)" -eq 0 ]; then SUDO=""; elif command -v sudo >/dev/null 2>&1; then SUDO="sudo"; else SUDO=""; fi

echo "== installing Debian/Ubuntu build dependencies =="
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
  build-essential cmake pkg-config git \
  qtbase5-dev libqt5sql5-sqlite \
  libgl1-mesa-dev \
  libmpv-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libpulse-dev \
  libssl-dev ca-certificates \
  gnupg \
  desktop-file-utils

echo "== configure + build =="
# The version shown by the app (Ajustes, updater): the display form of the
# release when the workflow passes it (1.0.0-alpha.1), the one of
# CMakeLists.txt otherwise. It is NOT the dpkg one (1.0.0~alpha.1) that goes
# into the package metadata below: apt and the About box speak differently.
extra=()
if [ -n "${NEVERTUBE_VERSION:-}" ]; then
  extra+=("-DNEVERTUBE_VERSION=$NEVERTUBE_VERSION")
fi
cmake -S . -B build-deb -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr "${extra[@]}"
cmake --build build-deb --parallel "$(nproc)"

echo "== offline selftests (gate) =="
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
  ./build-deb/nevertube "$t"
done

# The version comes from CMakeLists.txt, EXCEPT on a release build: the tag is
# the authority there (NEVERTUBE_PKG_VERSION, set by .github/workflows/
# release.yml) and it already carries the pre-release suffix. The +git marker
# is for development builds only; on a release it would make every build of the
# same tag a different package.
ver="${NEVERTUBE_PKG_VERSION:-$(grep -oP 'project\(nevertube VERSION \K[0-9.]+' CMakeLists.txt | head -1)}"
arch="$(dpkg --print-architecture)"
if [ -n "${NEVERTUBE_PKG_VERSION:-}" ]; then
  pkgver="$NEVERTUBE_PKG_VERSION"
else
  pkgver="${ver}+git$(git rev-parse --short HEAD)"
fi
echo "== packaging nevertube $pkgver ($arch) =="

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

root="$tmp/root"
# --strip: a Release build keeps its symbol table (541 KB in the current
# binary). Stripping is what a distro package does and saves ~18% of the binary;
# it does not touch the dynamic section, so dpkg-shlibdeps below still reads the
# NEEDED libraries from the exe.
DESTDIR="$root" cmake --install build-deb --prefix /usr --strip
test -x "$root/usr/bin/nevertube"
test -f "$root/usr/share/applications/nevertube.desktop"
test -f "$root/usr/share/icons/hicolor/256x256/apps/nevertube.png"
desktop-file-validate "$root/usr/share/applications/nevertube.desktop"

# Debian policy: the copyright file goes in /usr/share/doc/<pkg>/copyright. It
# references /usr/share/common-licenses/GPL-3 instead of embedding the text.
mkdir -p "$root/usr/share/doc/nevertube"
cp packaging/linux/deb/copyright "$root/usr/share/doc/nevertube/copyright"
test -f "$root/usr/share/doc/nevertube/copyright"

# ---------------------------------------------------------------------------
# The APT repository: the PUBLIC key and the maintainer scripts.
#
# Installing the .deb is what makes `apt update && apt upgrade` work on the
# Ubuntu releases the repository is built for, and that is only possible if the
# package carries the key the repository is signed with: the source postinst
# writes points at this keyring with `signed-by=`. The PRIVATE key never leaves
# the CI secret (GPG_PRIVATE_KEY); what goes in here is the published public
# key, byte for byte, in packaging/linux/deb/nevertube-archive-keyring.asc.
#
# The fingerprint is PINNED. It was read from the published repository itself
# (apt/dists/jammy/InRelease, apt/dists/noble/InRelease and
# flatpak/repo/summary.sig all carry this key), and the build STOPS if the file
# in the tree is not that key: a package whose keyring does not match the
# signature makes `apt update` answer NO_PUBKEY for every user, which is worse
# than not shipping the repository at all. If the key of the project ever
# changes on purpose, this constant changes in the same commit as the .asc.
# ---------------------------------------------------------------------------
KEY_FPR="D22DB9BBE7F4EF8FDD13CC9805A7DDE36DCDD33F"
KEY_ASC="packaging/linux/deb/nevertube-archive-keyring.asc"
KEYRING="$root/usr/share/keyrings/nevertube-archive-keyring.gpg"
test -f "$KEY_ASC"
mkdir -p "$tmp/gnupg"
chmod 700 "$tmp/gnupg"
gpg_home=(--batch --no-tty --homedir "$tmp/gnupg")
key_fpr="$(gpg "${gpg_home[@]}" --with-colons --show-keys "$KEY_ASC" \
             | awk -F: '/^fpr:/{print $10; exit}')"
if [ "$key_fpr" != "$KEY_FPR" ]; then
  echo "ERROR: $KEY_ASC carries the key ${key_fpr:-<none>} and the published" >&2
  echo "       repository is signed with $KEY_FPR." >&2
  echo "       Do not replace the key to make this pass: export the key that" >&2
  echo "       signs dists/*/InRelease and find out why they differ first." >&2
  exit 1
fi
mkdir -p "$root/usr/share/keyrings"
gpg "${gpg_home[@]}" --dearmor < "$KEY_ASC" > "$KEYRING"
chmod 0644 "$KEYRING"
# The keyring apt will read (binary, 0644, root) really is that key.
keyring_fpr="$(gpg "${gpg_home[@]}" --with-colons --show-keys "$KEYRING" \
                 | awk -F: '/^fpr:/{print $10; exit}')"
test "$keyring_fpr" = "$KEY_FPR"
echo "APT keyring: $KEYRING ($KEY_FPR)"

# postinst configures the signed source on Ubuntu 22.04/24.04 only (idempotent,
# no network, no apt call); postrm removes it and only it, and not on upgrade.
# Their behaviour is covered by an isolated test that needs no package and no
# second machine, and which runs here so a broken script never reaches a user.
install -D -m 0755 packaging/linux/deb/postinst "$root/DEBIAN/postinst"
install -D -m 0755 packaging/linux/deb/postrm "$root/DEBIAN/postrm"
sh -n "$root/DEBIAN/postinst"
sh -n "$root/DEBIAN/postrm"
bash packaging/linux/deb/test-apt-source.sh

# NEEDED -> packages. dpkg-shlibdeps reads the package name from a
# debian/control in the working directory, so it runs from a throwaway one.
mkdir -p "$tmp/debian"
printf 'Source: nevertube\nSection: video\nPriority: optional\nMaintainer: nevertube <noreply@localhost>\nStandards-Version: 4.6.0\n' \
  > "$tmp/debian/control"
shlibs="$( cd "$tmp" && dpkg-shlibdeps -O -e"$repo/build-deb/nevertube" \
            | sed 's/^shlibs:Depends=//' )"
# libqt5sql5-sqlite is the SQLite driver plugin: Qt dlopen()s it, so it does
# not show up as a linked library but the app is unusable without it.
#
# ffmpeg (the CLI) is a DEPENDENCY OF ITS OWN: dpkg-shlibdeps only sees the
# sonames the binary links (libavformat/libavcodec/libavutil), NOT the
# /usr/bin/ffmpeg that the DOWNLOADER runs to join the two tracks. Without
# the CLI the download finishes but the mux fails ("ffmpeg was not found"),
# so the package has to declare it by name.
depends="$shlibs, libqt5sql5-sqlite, ffmpeg"
echo "Depends: $depends"

for needle in libmpv libavformat libpulse libopengl libqt5 libqt5sql5 ffmpeg; do
  case "$depends" in
    *"$needle"*) : ;;
    *) echo "ERROR: Depends does not carry $needle: $depends" >&2; exit 1 ;;
  esac
done

mkdir -p "$root/DEBIAN"
# The delimiter is '#' and not '|': dpkg-shlibdeps emits alternatives
# ("libqt5gui5 | libqt5gui5-gles") and '|' would break the expression.
sed -e "s#__VERSION__#$pkgver#g" \
    -e "s#__ARCH__#$arch#g" \
    -e "s#__INSTALLED_SIZE__#$(du -sk "$root" | cut -f1)#g" \
    -e "s#__DEPENDS__#$depends#g" \
    packaging/linux/deb/control > "$root/DEBIAN/control"
cat "$root/DEBIAN/control"

mkdir -p dist
deb="dist/nevertube_${pkgver}_${arch}.deb"
dpkg-deb --build --root-owner-group "$root" "$deb"
dpkg-deb -f "$deb" Package Version Architecture Depends
dpkg-deb -c "$deb" | grep -E 'usr/bin/nevertube$|applications/nevertube.desktop$|hicolor/48x48/apps/nevertube.png$|doc/nevertube/copyright$|usr/share/keyrings/nevertube-archive-keyring.gpg$'

chk="$tmp/chk"
dpkg-deb -x "$deb" "$chk"
test -x "$chk/usr/bin/nevertube"
test -f "$chk/usr/share/keyrings/nevertube-archive-keyring.gpg"

# The maintainer scripts travel in the control archive and have to be
# executable there; the keyring read back FROM THE PACKAGE has to be the pinned
# key (checking the tree would not catch a wrong install path).
ctl="$tmp/ctl"
mkdir -p "$ctl"
dpkg-deb -e "$deb" "$ctl"
for s in postinst postrm; do
  test -f "$ctl/$s"
  test -x "$ctl/$s"
done
pkg_fpr="$(gpg "${gpg_home[@]}" --with-colons --show-keys \
            "$chk/usr/share/keyrings/nevertube-archive-keyring.gpg" \
            | awk -F: '/^fpr:/{print $10; exit}')"
if [ "$pkg_fpr" != "$KEY_FPR" ]; then
  echo "ERROR: the keyring inside $deb is ${pkg_fpr:-<none>}, not $KEY_FPR" >&2
  exit 1
fi
echo "package: keyring $pkg_fpr + postinst/postrm"

# Installability (same "viability" gate as the .rpm and pacman jobs): install the
# package and run the offline selftests from the INSTALLED path (not the build
# tree). apt pulls the declared Depends, so nothing is assumed about the system.
$SUDO apt-get install -y "$repo/$deb"
# The postinst has to have configured the APT source of THIS release of Ubuntu,
# pointing at the keyring that came in the package. Nothing here runs
# `apt update`: the script must not need the network, and this checks the file
# it wrote, not connectivity.
if [ ! -f /usr/share/keyrings/nevertube-archive-keyring.gpg ]; then
  echo "ERROR: the keyring is not installed" >&2
  exit 1
fi
# shellcheck disable=SC1091
. /etc/os-release
suite="${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}"
grep -qF 'signed-by=/usr/share/keyrings/nevertube-archive-keyring.gpg' \
  /etc/apt/sources.list.d/nevertube.list
grep -qF " https://marlo4220mc.github.io/NeverTube/apt/ $suite main" \
  /etc/apt/sources.list.d/nevertube.list
if grep -qF 'trusted=yes' /etc/apt/sources.list.d/nevertube.list; then
  echo "ERROR: the APT source is globally trusted" >&2
  exit 1
fi
echo "APT source configured by postinst:"
cat /etc/apt/sources.list.d/nevertube.list
# The CLI of ffmpeg must be THERE after the install: it is what the
# downloader runs to join the two tracks, and it is a dependency of its own
# (the sonames of the binary do not cover it).
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ERROR: ffmpeg (the CLI) is not installed and the package does not bring it" >&2
  exit 1
fi
# ldd catches a package whose Depends do not cover a NEEDED library. grep WITHOUT
# -q: with pipefail, -q closes the pipe early and ldd dies with SIGPIPE (141).
if ldd /usr/bin/nevertube | grep 'not found' >/dev/null; then
  echo "ERROR: missing shared libraries after install:" >&2
  ldd /usr/bin/nevertube | grep 'not found' >&2
  exit 1
fi
export QT_QPA_PLATFORM=offscreen
/usr/bin/nevertube --selftest-db
/usr/bin/nevertube --selftest-i18n

echo "OK: $deb ($(du -h "$deb" | cut -f1))"
