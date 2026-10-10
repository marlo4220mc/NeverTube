#!/usr/bin/env bash
#
# Build the Arch package (pacman) for nevertube_qt2.
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

ver="$(grep -oP 'project\(nevertube_qt2 VERSION \K[0-9.]+' CMakeLists.txt | head -1)"
echo "== packaging nevertube-qt2 $ver (x86_64) =="

# git refuses to operate on a repo owned by another user (the mounted checkout
# is owned by the runner UID and this runs as root).
git config --global --add safe.directory "$repo"

id -u builder >/dev/null 2>&1 || useradd -m builder

rm -rf archbuild
mkdir -p archbuild
sed "s|__PKGVER__|$ver|g" packaging/linux/arch/PKGBUILD > archbuild/PKGBUILD
# The "source" of the PKGBUILD is a git archive of the checkout, so what is
# built is exactly what is committed.
git archive --format=tar.gz --prefix=nevertube_qt2/ \
  -o "archbuild/nevertube_qt2-$ver.tar.gz" HEAD

chown -R builder:builder archbuild
su builder -c "cd '$repo/archbuild' && makepkg -f --noconfirm"

mkdir -p dist
pkg="dist/nevertube-qt2-${ver}-x86_64.pkg.tar.zst"
cp archbuild/*.pkg.tar.zst "$pkg"
tar -tvf "$pkg" | grep -q 'usr/bin/nevertube_qt2'
tar -tvf "$pkg" | grep -q 'usr/share/applications/nevertube_qt2.desktop'
tar -xOf "$pkg" .PKGINFO > "$repo/archbuild/PKGINFO"
for dep in qt5-base mpv ffmpeg libpulse; do
  grep -qx "depend = $dep" "$repo/archbuild/PKGINFO"
done
echo "OK: $pkg ($(du -h "$pkg" | cut -f1))"
