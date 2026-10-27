<div align="right">
  <b>English</b> · <a href="README.es.md">Español</a>
</div>

# NeverTube

**NeverTube** is a lightweight YouTube client for Windows (7 / 10 / 11) and Linux, built with Qt5 and C++17. It streams directly from YouTube's unofficial web API — no browser, no Electron, and on Linux no bundled runtime: the packages are about 1 MB and pull their dependencies from the distribution's own repositories.

## Features

**Playback**

- **Two playback engines**: libmpv on Linux, Windows x64 and Windows 10 x86, and a from-scratch FFmpeg/libav engine for Windows x86 (Win7)
- Startup hardware probe (AV1 / VP9 / H.264) that picks the codec profile the machine really decodes, and steps down by itself when a profile ends up decoding in software
- Live streams (badge on the thumbnail and live player mode), synthetic AV1 over HLS, resume where you left off
- "Up next" queue seeded with YouTube radio (Music-style mix) or static suggestions, shuffle and repeat, mini player
- Shortcuts: `Space` / `K` play-pause, `J` / `L` ±10 s, `←` / `→` ±5 s, `N` next, `P` previous (restarts first if you are past 5 s), `↑` / `↓` volume, `M` mute, `F` fullscreen, `D` download menu
- Automatic recovery from gated YouTube sessions (403s): re-resolve in a fresh session and, as a last resort, restart with the last video

**Browsing**

- Home with Continue watching, New from your subscriptions, Recommended and Discover (the radios generated from what you play are saved and reused)
- Search with live suggestions; pasting a YouTube link opens that video instead of searching for the text
- Channel pages (videos / shorts / live tabs), playlists, watch history
- Library with subscriptions, playlists and history; the row menu covers watch later, adding to a playlist, sharing and the channel
- Shorts are kept out of the lists (queue, continue watching, discover): the player is 16:9 and a reel in a grid breaks the row

**Downloads** — from the Player button or the Downloads tab

- Resolution submenu with the codec variants and their real size, or audio only
- Byte-range fragments downloaded in parallel and resumable; a 401/403 re-resolves the URLs and continues with what is already on disk
- Muxed with `ffmpeg` as a stream copy (no re-encoding): video as MKV, audio as MP3 (M4A when the ffmpeg has no MP3 encoder). The Windows installers ship their own ffmpeg
- One download at a time, with video and audio as separate entries; per-row progress and speed, pause / resume, retry, delete (optionally the file), open folder, pause all / resume all
- Default folder `Downloads/NeverTube` (configurable), 1–16 connections (8 is the measured optimum), and while a video plays the download lowers its own thread priority so the player keeps its frames

**Interface**

- Grid (4 / 5 / 6 columns) and list views, three thumbnail sizes, seven accent colors, hot language switching (English / Spanish; a new language is one JSON file in `localization/`)
- One instance per user: a second launch (a link, a `--download`) hands its arguments to the running window instead of opening a second copy of the database

## Download

The builds are on the [Releases](https://github.com/marlo4220mc/NeverTube/releases) page, all from the same commit, with `SHA256SUMS.txt` next to them (`sha256sum -c SHA256SUMS.txt` verifies a download). Every asset is produced by `.github/workflows/release.yml`; what the pipeline does and how a release is cut is in [`docs/release/README.md`](docs/release/README.md).

Linux packages are x86_64 only.

| Format | Target | File |
|---|---|---|
| `.deb` | Ubuntu 22.04 / Mint 21 / Debian 12 | `NeverTube-<version>-Ubuntu-22.04-amd64.deb` |
| `.deb` | Ubuntu 24.04 / Mint 22 / Debian 13 | `NeverTube-<version>-Ubuntu-24.04-amd64.deb` |
| `.rpm` | Fedora (RHEL-family with matching sonames) | `NeverTube-<version>-Fedora-x86_64.rpm` |
| pacman | Arch Linux | `NeverTube-<version>-Arch-Linux-x86_64.pkg.tar.zst` |
| Flatpak | any distribution, including the immutable ones | `NeverTube-<version>-Linux-Flatpak.flatpak` |
| OSTree repo | `flatpak update` from a hosted repo | `NeverTube-<version>-Linux-Flatpak-Repo.tar.gz` |
| setup.exe | Windows 10 / 11 (x64) | `NeverTube-<version>-Windows-10-11-x64-Setup.exe` |
| setup.exe | Windows 10 / 11 (x86) | `NeverTube-<version>-Windows-10-11-x86-Setup.exe` |
| setup.exe | Windows 7 SP1 (x86) | `NeverTube-<version>-Windows-7-SP1-x86-Setup.exe` |

There are **two `.deb`** on purpose: 24.04 renamed the Qt5 runtime packages to the `t64` ones and ships FFmpeg 6.1, so a package built on 22.04 cannot install there and vice versa. They are the same program built against each base.

### Install

- Debian / Ubuntu: `sudo apt install ./NeverTube-<version>-Ubuntu-24.04-amd64.deb` — apt installs the declared dependencies automatically.
- Fedora: `sudo dnf install ./NeverTube-<version>-Fedora-x86_64.rpm` — dnf resolves the soname requirements (`ffmpeg-free`, or RPM Fusion's ffmpeg if already installed).
- Arch: `sudo pacman -U NeverTube-<version>-Arch-Linux-x86_64.pkg.tar.zst`
- Flatpak: `flatpak install --user NeverTube-<version>-Linux-Flatpak.flatpak` — the deliverable for the distributions where an `rpm`/`deb` is not an option (Silverblue / Kinoite / Bazzite, MicroOS / Aeon, SteamOS): the bundle brings its own Qt, libmpv and ffmpeg, so it is the same binary everywhere. It can also be shipped as its own OSTree repository so `flatpak update` works (unsigned: `--no-gpg-verify`); permissions, updates and the manual checks are in `packaging/linux/flatpak/README.md`.
- Windows: run the setup for your system — `NeverTube-<version>-Windows-7-SP1-x86-Setup.exe` (32-bit Windows 7), `NeverTube-<version>-Windows-10-11-x86-Setup.exe` (32-bit Windows 10/11) or `NeverTube-<version>-Windows-10-11-x64-Setup.exe` (64-bit Windows 10/11). Each one refuses the other two and tells you which to use; they install the VC++ redistributable they need. The installers are not code-signed, so SmartScreen warns on first run.

The binary is `nevertube`. On Linux the `.desktop` entry and the icons are installed in the `hicolor` theme, so the app shows up in the menu named **NeverTube**.

### Build from source

```sh
cmake -B build && cmake --build build
```

Dependencies: Qt5 (Core, Gui, Widgets, Network, Sql), OpenGL, libmpv. The Windows 7 x86 build uses the vendored FFmpeg package in `third_party/ffmpeg-win32/` (see `BUILD_WINDOWS.md`); downloads need the `ffmpeg` **command line** at runtime (the Linux packages declare it, the Windows installers ship it).

Offline regression tests (no network, no GPU) — 23 of them, one per subsystem:

```sh
./build/nevertube --selftest-db        # repositories and SQLite
./build/nevertube --selftest-grid      # grid geometry
./build/nevertube --selftest-theme     # QSS and selection palette
./build/nevertube --selftest-i18n      # language catalogs
./build/nevertube --selftest-download  # download plan
```

The full list, with what each one covers, is in `AGENTS.md`.

## Command line

```
nevertube                            # the app
nevertube --search <query>           # search at startup
nevertube --channel <channelId>      # open a channel
nevertube --playlist <playlistId>    # open a YouTube playlist
nevertube <youtube-url>              # play a video
nevertube --download <videoId> [height|audio]
nevertube --pause-download <videoId>
nevertube --resume-download <videoId>
nevertube --play-local <file>        # play a downloaded file
nevertube --downloads | --library    # open that page
```

## Limitations

- Windows x86 (Win7): no AV1 (neither hardware nor software decoding); the codec profile tops out at `h264-hw` (DXVA2). The x86 setup refuses to install on 64-bit Windows 10/11 and points to the x64 build, where the same machine would do better.
- Windows x64: the default `d3d11va-copy` path uses more CPU; an experimental zero-copy mode (`NEVERTUBE_ANGLE=1`, ANGLE + `d3d11-egl`) is opt-in.
- Linux packages are x86_64 only.
- The app uses YouTube's unofficial web API to resolve streams and live status. This project is not affiliated with, endorsed by, or officially connected to YouTube or Google.

## License

GPL-3.0-or-later. Third-party components keep their own licenses: Qt5 (LGPL-3.0), libmpv (LGPL-2.1+), FFmpeg (LGPL-2.1+), LAME (LGPL-2.0, the MP3 encoder of the Windows ffmpeg), Material Symbols icon geometry (Apache-2.0), OpenSSL (Apache-2.0).
