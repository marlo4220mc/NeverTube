#!/usr/bin/env bash
#
# Build the Debian/Ubuntu package (.deb) for nevertube_qt2.
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
  desktop-file-utils

echo "== configure + build =="
cmake -S . -B build-deb -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-deb --parallel "$(nproc)"

echo "== offline selftests (gate) =="
export QT_QPA_PLATFORM=offscreen
for t in --selftest-db --selftest-discover --selftest-ssl \
         --selftest-grid --selftest-theme --selftest-glyphs \
         --selftest-i18n --selftest-dates --selftest-suggest-parse \
         --selftest-suggest-panel --selftest-suggest-gestures \
         --selftest-av1hls; do
  echo "SELFTEST $t"
  ./build-deb/nevertube_qt2 "$t"
done

ver="$(grep -oP 'project\(nevertube_qt2 VERSION \K[0-9.]+' CMakeLists.txt | head -1)"
arch="$(dpkg --print-architecture)"
pkgver="${ver}+git$(git rev-parse --short HEAD)"
echo "== packaging nevertube-qt2 $pkgver ($arch) =="

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

root="$tmp/root"
# --strip: a Release build keeps its symbol table (541 KB in the current
# binary). Stripping is what a distro package does and saves ~18% of the binary;
# it does not touch the dynamic section, so dpkg-shlibdeps below still reads the
# NEEDED libraries from the exe.
DESTDIR="$root" cmake --install build-deb --prefix /usr --strip
test -x "$root/usr/bin/nevertube_qt2"
test -f "$root/usr/share/applications/nevertube_qt2.desktop"
test -f "$root/usr/share/icons/hicolor/256x256/apps/nevertube_qt2.png"
desktop-file-validate "$root/usr/share/applications/nevertube_qt2.desktop"

# Debian policy: the copyright file goes in /usr/share/doc/<pkg>/copyright. It
# references /usr/share/common-licenses/GPL-3 instead of embedding the text.
mkdir -p "$root/usr/share/doc/nevertube-qt2"
cp packaging/linux/deb/copyright "$root/usr/share/doc/nevertube-qt2/copyright"
test -f "$root/usr/share/doc/nevertube-qt2/copyright"

# NEEDED -> packages. dpkg-shlibdeps reads the package name from a
# debian/control in the working directory, so it runs from a throwaway one.
mkdir -p "$tmp/debian"
printf 'Source: nevertube-qt2\nSection: video\nPriority: optional\nMaintainer: nevertube <noreply@localhost>\nStandards-Version: 4.6.0\n' \
  > "$tmp/debian/control"
shlibs="$( cd "$tmp" && dpkg-shlibdeps -O -e"$repo/build-deb/nevertube_qt2" \
            | sed 's/^shlibs:Depends=//' )"
# libqt5sql5-sqlite is the SQLite driver plugin: Qt dlopen()s it, so it does
# not show up as a linked library but the app is unusable without it.
depends="$shlibs, libqt5sql5-sqlite"
echo "Depends: $depends"

for needle in libmpv libavformat libpulse libopengl libqt5 libqt5sql5; do
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
deb="dist/nevertube-qt2_${pkgver}_${arch}.deb"
dpkg-deb --build --root-owner-group "$root" "$deb"
dpkg-deb -f "$deb" Package Version Architecture Depends
dpkg-deb -c "$deb" | grep -E 'usr/bin/nevertube_qt2$|applications/nevertube_qt2.desktop$|hicolor/48x48/apps/nevertube_qt2.png$|doc/nevertube-qt2/copyright$'

chk="$tmp/chk"
dpkg-deb -x "$deb" "$chk"
test -x "$chk/usr/bin/nevertube_qt2"

# Installability (same "viability" gate as the .rpm and pacman jobs): install the
# package and run the offline selftests from the INSTALLED path (not the build
# tree). apt pulls the declared Depends, so nothing is assumed about the system.
$SUDO apt-get install -y "$repo/$deb"
# ldd catches a package whose Depends do not cover a NEEDED library. grep WITHOUT
# -q: with pipefail, -q closes the pipe early and ldd dies with SIGPIPE (141).
if ldd /usr/bin/nevertube_qt2 | grep 'not found' >/dev/null; then
  echo "ERROR: missing shared libraries after install:" >&2
  ldd /usr/bin/nevertube_qt2 | grep 'not found' >&2
  exit 1
fi
export QT_QPA_PLATFORM=offscreen
/usr/bin/nevertube_qt2 --selftest-db
/usr/bin/nevertube_qt2 --selftest-i18n

echo "OK: $deb ($(du -h "$deb" | cut -f1))"
