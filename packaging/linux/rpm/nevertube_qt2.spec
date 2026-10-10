Name: nevertube-qt2
Version: __VERSION__
Release: 1%{?dist}
Summary: Lightweight YouTube client (Qt5 + libmpv)
License: GPL-3.0-or-later
URL: https://github.com/marlo4220mc/nevertube_qt2
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
# Wayland session (Fedora Workstation's default): without this plugin Qt falls
# back to xcb through XWayland. Recommends, not Requires: on a pure X11 desktop
# (or with XWayland present) the app works without it.
Recommends: qt5-qtwayland
BuildArch: x86_64

%description
A lightweight YouTube client built with Qt5 and the mpv playback core.

%prep

%build

# The workflow (packaging/linux/rpm/build.sh) compiles the app and leaves the
# install tree under BUILD/nevertube_qt2/ (usr/bin, usr/share). This spec only
# packages it; it does not compile. The staging path arrives as the `stage`
# macro (rpmbuild runs %install in its own subdirectory, so a relative path is
# not safe).
%install
mkdir -p %{buildroot}
cp -a %{stage}/. %{buildroot}/

%files
%{_bindir}/nevertube_qt2
%{_datadir}/applications/nevertube_qt2.desktop
%{_datadir}/icons/hicolor/16x16/apps/nevertube_qt2.png
%{_datadir}/icons/hicolor/24x24/apps/nevertube_qt2.png
%{_datadir}/icons/hicolor/32x32/apps/nevertube_qt2.png
%{_datadir}/icons/hicolor/48x48/apps/nevertube_qt2.png
%{_datadir}/icons/hicolor/64x64/apps/nevertube_qt2.png
%{_datadir}/icons/hicolor/128x128/apps/nevertube_qt2.png
%{_datadir}/icons/hicolor/256x256/apps/nevertube_qt2.png

%changelog
* Sat Oct 10 2026 marlo4220mc <marlo4220mc@users.noreply.github.com> - __VERSION__-1
- Initial package
