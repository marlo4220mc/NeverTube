#!/usr/bin/env bash
#
# Build the RPM package for nevertube.
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
# The version shown by the app (Ajustes, updater): the display form of the
# release when the workflow passes it (1.0.0-alpha.1), the one of
# CMakeLists.txt otherwise.
extra=()
if [ -n "${NEVERTUBE_VERSION:-}" ]; then
  extra+=("-DNEVERTUBE_VERSION=$NEVERTUBE_VERSION")
fi
cmake -S . -B build-rpm -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr "${extra[@]}"
cmake --build build-rpm --parallel "$(nproc)"

echo "== offline selftests (gate) =="
export QT_QPA_PLATFORM=offscreen
for t in --selftest-db --selftest-discover --selftest-ssl \
         --selftest-grid --selftest-theme --selftest-glyphs \
         --selftest-i18n --selftest-dates --selftest-av1hls \
         --selftest-variants --selftest-reels --selftest-download \
         --selftest-download-button --selftest-download-folder \
         --selftest-download-settings --selftest-download-transport \
         --selftest-download-resume --selftest-single-instance \
         --selftest-video-url \
         --selftest-search-url --selftest-update \
         --selftest-settings-page; do
  echo "SELFTEST $t"
  ./build-rpm/nevertube "$t"
done

# Release builds pass the version of the tag (NEVERTUBE_PKG_VERSION, see
# .github/workflows/release.yml); a development build reads CMakeLists.txt.
# The value comes already normalized for rpm: a pre-release is "1.0.0~alpha.1"
# because a hyphen is the Version/Release separator and rpm rejects it there.
ver="${NEVERTUBE_PKG_VERSION:-$(grep -oP 'project\(nevertube VERSION \K[0-9.]+' CMakeLists.txt | head -1)}"
# The app's own version (display spelling) is not the rpm one when there is a
# suffix; it is the one the binary shows and the updater compares.
appver="${NEVERTUBE_VERSION:-$ver}"
echo "== packaging nevertube $ver (x86_64) =="

rpmtop="$repo/rpmbuild"
rm -rf "$rpmtop"
mkdir -p "$rpmtop"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

# The install tree ends up in BUILD/nevertube/ (usr/bin, usr/share); the
# spec only copies it into %{buildroot}, it does not compile.
# --strip: strips the symbol table of the Release binary (same as the .deb and
# the pacman package, where makepkg strips by itself).
DESTDIR="$rpmtop/BUILD/nevertube" cmake --install build-rpm --prefix /usr --strip
# %license in the spec ships the GPL-3.0 text under /usr/share/licenses/<name>.
mkdir -p "$rpmtop/BUILD/nevertube/usr/share/licenses/nevertube"
cp LICENSE "$rpmtop/BUILD/nevertube/usr/share/licenses/nevertube/LICENSE"
# The spec is used AS IT IS (it is the published one, with its real default
# version): the version of the release arrives as a macro. The `stage` macro is
# what tells it that the tree is already compiled (the CI path); without it the
# same spec builds from source, which is the Copr path.
cp packaging/linux/rpm/nevertube.spec "$rpmtop/SPECS/nevertube.spec"
rpmbuild --define "_topdir $rpmtop" \
         --define "pkg_version $ver" \
         --define "stage $rpmtop/BUILD/nevertube" \
         -bb "$rpmtop/SPECS/nevertube.spec"

mkdir -p dist
rpm="dist/nevertube-${ver}-1.x86_64.rpm"
cp "$rpmtop"/RPMS/x86_64/nevertube-*.rpm "$rpm"
# NOTE: grep without -q on purpose (see arch/build.sh): -q + pipefail = SIGPIPE.
rpm -qpl "$rpm" | grep -x /usr/bin/nevertube >/dev/null
rpm -qpl "$rpm" | grep 'hicolor/256x256/apps/nevertube.png' >/dev/null
# The license must be inside the package (%license in the spec).
rpm -qpl "$rpm" | grep -x /usr/share/licenses/nevertube/LICENSE >/dev/null
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
# The CLI of ffmpeg (NOT the libraries): it is what the downloader runs to
# join the two tracks. The soname requires above do NOT cover it. It is a
# dependency by PATH (Fedora's ffmpeg-free and RPM Fusion's ffmpeg both
# provide /usr/bin/ffmpeg and they conflict with each other).
grep -q '/usr/bin/ffmpeg' "$rpmtop/requires.txt"

# Installability: install the package in the container and run offline
# selftests from the INSTALLED path (not the build tree): catches a package
# that builds and lists its files but does not start.
dnf remove -y nevertube >/dev/null 2>&1 || true
dnf install -y --nogpgcheck "$rpm"
# ldd catches a package whose Requires do not cover a NEEDED library. grep WITHOUT
# -q: with pipefail, -q closes the pipe early and ldd dies with SIGPIPE (141).
if ldd /usr/bin/nevertube | grep 'not found' >/dev/null; then
  echo "ERROR: missing shared libraries after install:" >&2
  ldd /usr/bin/nevertube | grep 'not found' >&2
  exit 1
fi
QT_QPA_PLATFORM=offscreen /usr/bin/nevertube --selftest-db
QT_QPA_PLATFORM=offscreen /usr/bin/nevertube --selftest-i18n

echo "OK: $rpm ($(du -h "$rpm" | cut -f1))"
