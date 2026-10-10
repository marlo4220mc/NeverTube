<div align="right">
  <b>English</b> · <a href="README.es.md">Español</a>
</div>

# nevertube_qt2

A lightweight YouTube client for Windows (7 / 10 / 11) and Linux, built with Qt5 and C++17. It streams directly from YouTube's unofficial web API — no browser, no Electron, and on Linux no bundled runtime: the packages are ~900 KB and pull their dependencies from the distribution's own repositories.

## Features

- **Two playback engines**: libmpv (Linux and Windows x64) and a from-scratch FFmpeg engine for Windows x86 (Win7)
- Grid and list views, search with live suggestions, channel pages, playlists, watch history
- "Up next" queue with radio / suggestions seeding, mini player, shuffle and repeat
- Live streams ("LIVE" badge and live player mode) and synthetic AV1-over-HLS for modern codec profiles
- Startup hardware probe (AV1 / VP9 / H.264) that picks the codec profile your machine actually decodes
- Automatic recovery from gated YouTube sessions (403s): re-resolve in a fresh session, last-resort app restart with the last video
- Hot language switching (English / Spanish) and 7 accent colors

## Packages

Built per target in CI; Linux packages are x86_64 only.

| Format | Target | File |
|---|---|---|
| `.deb` | Ubuntu 22.04 / 24.04 | `nevertube-qt2_<version>_amd64.deb` |
| `.rpm` | Fedora (RHEL-family with matching sonames) | `nevertube-qt2-<version>-1.x86_64.rpm` |
| pacman | Arch Linux | `nevertube-qt2-<version>-x86_64.pkg.tar.zst` |
| setup.exe | Windows 10 / 11 (x64) | `nevertube-setup-x64.exe` |
| setup.exe | Windows 7 (x86) | `nevertube-setup.exe` |

### Install

- Debian / Ubuntu: `sudo apt install ./nevertube-qt2_<version>_amd64.deb` — apt installs the declared dependencies automatically.
- Fedora: `sudo dnf install ./nevertube-qt2-<version>-1.x86_64.rpm` — dnf resolves the soname requirements (`ffmpeg-free`, or RPM Fusion's ffmpeg if already installed).
- Arch: `sudo pacman -U nevertube-qt2-<version>-x86_64.pkg.tar.zst`
- Windows: run the setup; it installs the VC++ redistributable it needs.

### Build from source

```sh
cmake -B build && cmake --build build
```

Dependencies: Qt5 (Core, Gui, Widgets, Network, Sql), OpenGL, libmpv. The Windows x86 build uses the vendored FFmpeg package in `third_party/ffmpeg-win32/` (see `BUILD_WINDOWS.md`).

## Limitations

- Windows x86 (Win7): no AV1 (neither hardware nor software decoding); the codec profile tops out at `h264-hw` (DXVA2).
- Windows x64: the default `d3d11va-copy` path uses more CPU; an experimental zero-copy mode (`NEVERTUBE_ANGLE=1`, ANGLE + `d3d11-egl`) is opt-in.
- The app uses YouTube's unofficial web API to resolve streams and live status. This project is not affiliated with, endorsed by, or officially connected to YouTube or Google.

## License

GPL-3.0-or-later. Third-party components keep their own licenses: Qt5 (LGPL-3.0), libmpv (LGPL-2.1+), FFmpeg (LGPL-2.1+), Material Symbols icon geometry (Apache-2.0), OpenSSL (Apache-2.0).