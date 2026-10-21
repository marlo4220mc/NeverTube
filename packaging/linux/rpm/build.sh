#!/usr/bin/env bash
#
# Build the RPM package for nevertube_qt2.
#
# MUST run inside Fedora (e.g. `docker run --rm -v "$PWD:/work" -w /work
# fedora:latest bash /work/packaging/linux/rpm/build.sh`). The binary is
# compiled with Fedora's own Qt5/mpv sonames: a package cross-built on Ubuntu
# would link libavcodec.so.58 and friends and would not run here.
#
# FFmpeg IS part of the build: Fedora base ships ffmpeg-free (libav*-free) since
# F36 and mpv-devel pulls its dev packages, so the FfmpegBackend compiles in and
# the exe gets NEEDED entries on libavcodec/libavformat/libavutil/libswresample/
# libswscale. rpm therefore auto-generates those runtime requires and dnf
# satisfies them with ffmpeg-free -- or with RPM Fusion's ffmpeg, same sonames,
# if the user already has it. Nobody is forced to enable RPM Fusion.
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
         --selftest-i18n --selftest-dates --selftest-av1hls \
         --selftest-variants --selftest-reels --selftest-download \
         --selftest-download-button --selftest-download-folder \
         --selftest-download-settings --selftest-settings-page; do
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
# --strip: strips the symbol table of the Release binary (same as the .deb and
# the pacman package, where makepkg strips by itself).
DESTDIR="$rpmtop/BUILD/nevertube_qt2" cmake --install build-rpm --prefix /usr --strip
# %license in the spec ships the GPL-3.0 text under /usr/share/licenses/<name>.
mkdir -p "$rpmtop/BUILD/nevertube_qt2/usr/share/licenses/nevertube-qt2"
cp LICENSE "$rpmtop/BUILD/nevertube_qt2/usr/share/licenses/nevertube-qt2/LICENSE"
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
# The license must be inside the package (%license in the spec).
rpm -qpl "$rpm" | grep -x /usr/share/licenses/nevertube-qt2/LICENSE >/dev/null
rpm -qp --requires "$rpm" | tee "$rpmtop/requires.txt"
grep -q '^mpv-libs' "$rpmtop/requires.txt"
grep -q '^qt5-qtbase' "$rpmtop/requires.txt"
# The soname requires rpmbuild adds by itself must carry the FFmpeg runtime (the
# FfmpegBackend is compiled in Fedora: mpv-devel pulls the libav dev packages) and
# the Qt GUI/Widgets libs. dnf resolves each soname to the package that provides
# it, so nothing is assumed about the system.
grep -q '^libavformat\.so' "$rpmtop/requires.txt"
grep -q '^libQt5Gui\.so' "$rpmtop/requires.txt"
grep -q '^libQt5Widgets\.so' "$rpmtop/requires.txt"

# Installability: install the package in the container and run offline
# selftests from the INSTALLED path (not the build tree): catches a package
# that builds and lists its files but does not start.
dnf remove -y nevertube-qt2 >/dev/null 2>&1 || true
dnf install -y --nogpgcheck "$rpm"
# ldd catches a package whose Requires do not cover a NEEDED library. grep WITHOUT
# -q: with pipefail, -q closes the pipe early and ldd dies with SIGPIPE (141).
if ldd /usr/bin/nevertube_qt2 | grep 'not found' >/dev/null; then
  echo "ERROR: missing shared libraries after install:" >&2
  ldd /usr/bin/nevertube_qt2 | grep 'not found' >&2
  exit 1
fi
QT_QPA_PLATFORM=offscreen /usr/bin/nevertube_qt2 --selftest-db
QT_QPA_PLATFORM=offscreen /usr/bin/nevertube_qt2 --selftest-i18n

echo "OK: $rpm ($(du -h "$rpm" | cut -f1))"
