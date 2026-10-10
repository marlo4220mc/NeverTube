#!/usr/bin/env bash
#
# Build the RPM package for nevertube_qt2.
#
# MUST run inside Fedora (e.g. `docker run --rm -v "$PWD:/work" -w /work
# fedora:latest bash /work/packaging/linux/rpm/build.sh`). The binary is
# compiled with Fedora's own Qt5/mpv sonames: a package cross-built on Ubuntu
# would link libavcodec.so.58 and friends and would not run here.
#
# FFmpeg is intentionally NOT installed: it is not in Fedora's base repos (it
# lives in RPM Fusion) and it is not needed to play, because mpv is the
# playback core. The FfmpegBackend is optional and CMake simply skips it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
cd "$repo"

echo "== installing Fedora build dependencies =="
dnf install -y cmake gcc-c++ make pkgconf-pkg-config \
  qt5-qtbase-devel \
  mpv-devel \
  pulseaudio-libs-devel \
  mesa-libGL-devel \
  openssl-devel \
  dejavu-sans-fonts \
  rpm-build desktop-file-utils findutils

echo "== configure + build =="
cmake -S . -B build-rpm -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-rpm --parallel "$(nproc)"

echo "== offline selftests (gate) =="
export QT_QPA_PLATFORM=offscreen
for t in --selftest-db --selftest-discover --selftest-ssl \
         --selftest-grid --selftest-theme --selftest-glyphs \
         --selftest-i18n --selftest-dates --selftest-av1hls; do
  echo "SELFTEST $t"
  ./build-rpm/nevertube_qt2 "$t"
done

ver="$(grep -oP 'project\(nevertube_qt2 VERSION \K[0-9.]+' CMakeLists.txt | head -1)"
echo "== packaging nevertube-qt2 $ver (x86_64) =="

rpmtop="$repo/rpmbuild"
rm -rf "$rpmtop"
mkdir -p "$rpmtop"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# The install tree ends up in BUILD/nevertube_qt2/ (usr/bin, usr/share); the
# spec only copies it into %{buildroot}, it does not compile.
DESTDIR="$rpmtop/BUILD/nevertube_qt2" cmake --install build-rpm --prefix /usr
sed "s|__VERSION__|$ver|g" packaging/linux/rpm/nevertube_qt2.spec > "$rpmtop/SPECS/nevertube_qt2.spec"
rpmbuild --define "_topdir $rpmtop" \
         --define "stage $rpmtop/BUILD/nevertube_qt2" \
         -bb "$rpmtop/SPECS/nevertube_qt2.spec"

mkdir -p dist
rpm="dist/nevertube-qt2-${ver}-1.x86_64.rpm"
cp "$rpmtop"/RPMS/x86_64/nevertube-qt2-*.rpm "$rpm"
# NOTE: grep without -q on purpose (see arch/build.sh): -q + pipefail = SIGPIPE.
rpm -qpl "$rpm" | grep -x /usr/bin/nevertube_qt2 >/dev/null
rpm -qpl "$rpm" | grep 'hicolor/256x256/apps/nevertube_qt2.png' >/dev/null
rpm -qp --requires "$rpm" | tee "$rpmtop/requires.txt"
grep -q '^mpv-libs' "$rpmtop/requires.txt"
grep -q '^qt5-qtbase' "$rpmtop/requires.txt"
echo "OK: $rpm ($(du -h "$rpm" | cut -f1))"
