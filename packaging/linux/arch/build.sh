#!/usr/bin/env bash
#
# Build the Arch package (pacman) for nevertube.
#
# MUST run inside Arch (e.g. `docker run --rm -v "$PWD:/work" -w /work
# archlinux:latest bash /work/packaging/linux/arch/build.sh`). The binary is
# compiled INSIDE Arch (through makepkg), so it links Arch's own sonames.
#
# makepkg refuses to run as root, so a throwaway `builder` user is created and
# makepkg runs as that user.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
cd "$repo"

echo "== installing Arch build dependencies =="
pacman -Syu --noconfirm
pacman -S --noconfirm --needed \
  base-devel git cmake shadow \
  qt5-base mpv ffmpeg libpulse libglvnd openssl hicolor-icon-theme ttf-dejavu

ver="$(grep -oP 'project\(nevertube VERSION \K[0-9.]+' CMakeLists.txt | head -1)"
echo "== packaging nevertube $ver (x86_64) =="

# git refuses to operate on a repo owned by another user (the mounted checkout
# is owned by the runner UID and this runs as root).
git config --global --add safe.directory "$repo"

id -u builder >/dev/null 2>&1 || useradd -m builder

rm -rf archbuild
mkdir -p archbuild
sed "s|__PKGVER__|$ver|g" packaging/linux/arch/PKGBUILD > archbuild/PKGBUILD
# The "source" of the PKGBUILD is a git archive of the checkout, so what is
# built is exactly what is committed.
git archive --format=tar.gz --prefix=nevertube/ \
  -o "archbuild/nevertube-$ver.tar.gz" HEAD

chown -R builder:builder archbuild
su builder -c "cd '$repo/archbuild' && makepkg -f --noconfirm"

mkdir -p dist
pkg="dist/nevertube-${ver}-x86_64.pkg.tar.zst"
cp archbuild/*.pkg.tar.zst "$pkg"
# NOTE: grep without -q on purpose: with pipefail, -q closes the pipe early,
# tar gets SIGPIPE and the script exits 141 even though the package is fine.
tar -tvf "$pkg" | grep 'usr/bin/nevertube' >/dev/null
tar -tvf "$pkg" | grep 'usr/share/applications/nevertube.desktop' >/dev/null
tar -xOf "$pkg" .PKGINFO > "$repo/archbuild/PKGINFO"
# Every runtime package the binary needs is a hard `depend`: makepkg does not
# derive them from the sonames, so the PKGBUILD lists them by hand and this gate
# checks none is missing.
for dep in qt5-base mpv ffmpeg libpulse libglvnd openssl hicolor-icon-theme; do
  grep -qx "depend = $dep" "$repo/archbuild/PKGINFO"
done
# Wayland is optional (optdepend): the app runs via xcb/XWayland without it.
grep -q '^optdepend = qt5-wayland' "$repo/archbuild/PKGINFO"

# Installability: install the package and run offline selftests from the
# INSTALLED path (not the build tree), like the .deb job does on the runner.
pacman -R --noconfirm nevertube >/dev/null 2>&1 || true
pacman -U --noconfirm "$pkg"
# ldd catches a package whose depends do not cover a NEEDED library. grep WITHOUT
# -q: with pipefail, -q closes the pipe early and ldd dies with SIGPIPE (141).
if ldd /usr/bin/nevertube | grep 'not found' >/dev/null; then
  echo "ERROR: missing shared libraries after install:" >&2
  ldd /usr/bin/nevertube | grep 'not found' >&2
  exit 1
fi
QT_QPA_PLATFORM=offscreen /usr/bin/nevertube --selftest-db
QT_QPA_PLATFORM=offscreen /usr/bin/nevertube --selftest-i18n

echo "OK: $pkg ($(du -h "$pkg" | cut -f1))"
