Name: nevertube
# The version is a REAL one by default (not a placeholder): this .spec is
# published with the repository and a Copr build takes it as it is. The release
# pipeline overrides it with the version of the tag (--define "pkg_version …",
# see packaging/linux/rpm/build.sh), which is where the pre-release suffix and
# the ~ of rpm live. Bump this default together with CMakeLists.txt.
Version: %{?pkg_version}%{!?pkg_version:1.0.0}
Release: 1%{?dist}
Summary: NeverTube - lightweight YouTube client (Qt5 + libmpv)
License: GPL-3.0-or-later
URL: https://github.com/marlo4220mc/NeverTube
Vendor: marlo4220mc
Packager: marlo4220mc
Group: Applications/Multimedia
# The binary is built inside Fedora (see packaging/linux/rpm/build.sh), so the
# sonames it links (Qt5, mpv, pulse, GL, openssl) are the ones in these runtime
# packages.
#
# FFmpeg is a REAL runtime dependency, not an optional one: the FfmpegBackend is
# compiled into the binary (mpv-devel pulls Fedora's ffmpeg-free dev packages),
# so libavcodec/libavformat/libavutil/libswresample/libswscale must be present or
# the exe does not even start.
#
# It is NOT listed by hand: rpm's automatic generator adds the requires from the
# exe's NEEDED entries ("libavcodec.so.62()(64bit)" and friends). That is
# provider-agnostic on purpose: on Fedora base dnf resolves them to ffmpeg-free;
# if the user already has RPM Fusion's ffmpeg (same sonames) they are satisfied by
# it and nothing gets installed; and it does not force RPM Fusion on anyone. The
# explicit list below is only for the packages we want by name.
Requires: mpv-libs, qt5-qtbase, pulseaudio-libs, mesa-libGL, openssl-libs
Requires: hicolor-icon-theme
# ffmpeg (the CLI, NOT the libraries) is a dependency of its own: the
# automatic generator only sees the sonames the exe links (libavformat.so.62
# and friends), not the /usr/bin/ffmpeg that the DOWNLOADER runs to join the
# two tracks. Without the CLI the download finishes but the mux fails
# ("ffmpeg was not found").
#
# It is a dependency BY PATH and not by name on purpose: Fedora's own
# "ffmpeg-free" and RPM Fusion's "ffmpeg" both provide /usr/bin/ffmpeg, and
# they CONFLICT with each other. Requiring "ffmpeg-free" by name would
# break the installation of the people who already have RPM Fusion (dnf
# would want to remove their ffmpeg). The path is satisfied by whichever of
# the two the user has, and it forces nobody to enable RPM Fusion.
Requires: /usr/bin/ffmpeg
# ── Two ways of building this spec, and they are not interchangeable ─────────
#   1. The CI (packaging/linux/rpm/build.sh) compiles the tree OUTSIDE and
#      passes the install tree with --define "stage <dir>": this spec only
#      packages it. That is what the Fedora job of the pipeline does.
#   2. FROM SOURCE (a source RPM, Copr building this spec from the GitHub
#      repository): no `stage` is defined, so the release tarball is unpacked
#      here and cmake configures and compiles it inside the build environment,
#      against Fedora's own Qt5/mpv/libav.
%global staged %{?stage:1}%{!?stage:0}

%if !%{staged}
# The tarball of a GitHub tag extracts to NeverTube-<version> (the repository
# name) and the one rpkg generates from a checkout to nevertube-<version>;
# %prep accepts both (see below).
Source0: https://github.com/marlo4220mc/NeverTube/archive/v%{version}.tar.gz
BuildRequires: cmake gcc-c++ make
BuildRequires: qt5-qtbase-devel mpv-devel
BuildRequires: ffmpeg-free-devel libavformat-free-devel libavcodec-free-devel
BuildRequires: pulseaudio-libs-devel mesa-libGL-devel openssl-devel
%endif
# Wayland session (Fedora Workstation's default): without this plugin Qt falls
# back to xcb through XWayland. Recommends, not Requires: on a pure X11 desktop
# (or with XWayland present) the app works without it.
Recommends: qt5-qtwayland
BuildArch: x86_64

%description
A lightweight YouTube client built with Qt5 and the mpv playback core.

%prep
%if !%{staged}
# -c -T: create the build directory and DO NOT untar; the tar below unpacks
# whatever the top directory of the archive is called inside it (--strip).
%setup -q -c -T
%{__tar} -xzf %{SOURCE0} --strip-components=1
%endif

%build
%if !%{staged}
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build
%endif

# 1) The workflow (packaging/linux/rpm/build.sh) compiles the app and leaves the
#    install tree under BUILD/nevertube/ (usr/bin, usr/share). The staging path
#    arrives as the `stage` macro (rpmbuild runs %install in its own
#    subdirectory, so a relative path is not safe).
# 2) From source, cmake installs into %{buildroot} and the license is copied
#    where %files declares it (the CMake install does not put it there).
%install
%if %{staged}
mkdir -p %{buildroot}
cp -a %{stage}/. %{buildroot}/
%else
%cmake_install
install -D -m 0644 LICENSE %{buildroot}%{_datadir}/licenses/nevertube/LICENSE
%endif

%files
%license %{_datadir}/licenses/nevertube/LICENSE
%{_bindir}/nevertube
%{_datadir}/applications/nevertube.desktop
%{_datadir}/icons/hicolor/16x16/apps/nevertube.png
%{_datadir}/icons/hicolor/24x24/apps/nevertube.png
%{_datadir}/icons/hicolor/32x32/apps/nevertube.png
%{_datadir}/icons/hicolor/48x48/apps/nevertube.png
%{_datadir}/icons/hicolor/64x64/apps/nevertube.png
%{_datadir}/icons/hicolor/128x128/apps/nevertube.png
%{_datadir}/icons/hicolor/256x256/apps/nevertube.png

%changelog
* Sat Oct 10 2026 marlo4220mc <marlo4220mc@users.noreply.github.com> - 1.0.0-1
- Initial package
- The spec can also build from source (no `stage`): that is the Copr path
