# NeverTube on Linux, the immutable way (Flatpak)

This directory builds **NeverTube as a Flatpak**: one bundle that runs on every
distribution, including the immutable ones (Fedora Silverblue / Kinoite /
Bazzite, openSUSE MicroOS / Aeon, SteamOS), where `rpm`/`deb` installs are not
an option and an AppImage is not either (see [Why not an
AppImage](#why-not-an-appimage-measured)).

## The three ways to ship it (and which one is used now)

| tier | what the user does | updates | when |
|---|---|---|---|
| **Native package** | `rpm-ostree install nevertube.rpm` (Fedora Atomic), `dnf`, `apt`, `pacman` | with the OS / the distro repo | already works, no code needed. Only for the traditional distros; on an immutable one it needs a reboot and it is layered over the image |
| **Flatpak bundle** | download `NeverTube-<ver>-Linux-Flatpak.flatpak` and `flatpak install` it | `flatpak update`, automatic (the bundle embeds the repo URL and GPG key) | **now** — this directory |
| **Flatpak + own OSTree repo** | `flatpak install --user <url>/io.github.marlo4220mc.NeverTube.flatpakref` (adds the signed remote and installs) | `flatpak update`, automatic | **now**, published on GitHub Pages by `publish-repos.yml` |
| **Flathub** | search in the software center | automatic | later: it needs a human review, the manifest rebuilt from source in their infra, and `flatpak-builder-lint` clean |

Shipping a Flatpak does **not** require Flathub. The bundle and the OSTree repo
are produced here and can be handed out from a release page.

## Which systems and which command

| system | recommendation |
|---|---|
| Fedora Silverblue / Kinoite / Bazzite / Bluefin | **Flatpak** (or `rpm-ostree install nevertube.rpm`, which layers it and needs a reboot) |
| openSUSE MicroOS / Aeon / Kalpa | **Flatpak** (`transactional-update pkg install` works but needs a reboot) |
| SteamOS (Steam Deck) | **Flatpak only**: the root filesystem is read-only and reset on updates |
| Ubuntu / Debian / Fedora Workstation / Arch | the native package (`.deb`, `.rpm`, pacman) is lighter: it links the distro's Qt/mpv instead of shipping its own |

## Install

### The bundle (configures the remote repository)

```sh
flatpak install --user NeverTube-1.0.0-Linux-Flatpak.flatpak
flatpak run io.github.marlo4220mc.NeverTube
```

Because the bundle is built with `--repo-url` and `--gpg-keys`, installing it
automatically configures the project's repository and imports the signing key.
After installation, `flatpak update` receives updates automatically.

### The OSTree repository (with updates)

The published repository lives on GitHub Pages and is signed with the project's
key. **One command adds the remote and installs the app**, because the
`.flatpakref` carries the URL, the branch and the key:

```sh
flatpak install --user https://marlo4220mc.github.io/NeverTube/flatpak/io.github.marlo4220mc.NeverTube.flatpakref
flatpak update --user io.github.marlo4220mc.NeverTube
```

The `.flatpakref` names the branch that really exists in the OSTree repository
(today `master`, because the manifest does not set `branch:`; the publish job
reads it from the refs instead of assuming it) and it is validated by installing
it before it is published.

The same thing in two steps, for a remote you want to keep separated from the
suggested name:

```sh
flatpak remote-add --user --if-not-exists nevertube \
  https://marlo4220mc.github.io/NeverTube/flatpak/nevertube.flatpakrepo
flatpak install --user nevertube io.github.marlo4220mc.NeverTube
flatpak update --user io.github.marlo4220mc.NeverTube
```

A `.flatpakrepo` file is just:

```ini
[Flatpak Repo]
Title=NeverTube
Url=https://<host>/repo
GPGKey=<base64 of the public key>   # omit only for an unsigned repo
```

and a `.flatpakref` adds the app, the branch and the runtime repository on top
of that:

```ini
[Flatpak Ref]
Version=1
Name=io.github.marlo4220mc.NeverTube
Branch=master
Title=NeverTube
Url=https://<host>/repo
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
IsRuntime=false
GPGKey=<base64 of the public key>
SuggestRemoteName=nevertube
```

**What "signed" has to cover in a Flatpak repository** (it was measured the hard
way): the SUMMARY signature is not enough. `flatpak install` verifies the commit
of every ref it pulls — or the static delta that carries it — against the remote
key too, so a repository with a signed summary and unsigned commits fails with
`Commit ...: GPG verification enabled, but no signatures found`. The publish job
(`.github/workflows/publish-repos.yml`) therefore signs every app/runtime ref
with `flatpak build-sign`, deletes the deltas built from the unsigned commits
and regenerates them, and then signs the summary. The `Repo.tar.gz` of a release
is still **unsigned** (it comes out of a build without secrets): unpacking it and
adding it by hand needs `--no-gpg-verify`, which is what the release notes say.

**If you added the remote before that change** (`flatpak remote-add --user
--no-gpg-verify nevertube ...`), that remote is configured to skip the
signature. Remove it once and install from the `.flatpakref`, which imports the
key and verifies the summary and the commits:

```sh
flatpak remote-delete --user nevertube
flatpak install --user https://marlo4220mc.github.io/NeverTube/flatpak/io.github.marlo4220mc.NeverTube.flatpakref
```

### Permissions

The manifest asks for exactly what the app uses:

| permission | why |
|---|---|
| `--share=network` | YouTube |
| `--socket=wayland`, `--socket=fallback-x11` | the window |
| `--share=ipc` | X11 shared memory |
| `--socket=pulseaudio` | mpv's `pulse` audio output (PipeWire is behind it) |
| `--device=dri` | VAAPI hardware decoding. Without it mpv decodes in software |
| `--filesystem=xdg-download:create` + `~/Downloads` | the downloader writes `<downloads>/NeverTube` |
| `--filesystem=xdg-documents:create` + `~/Documents` | the database is `<documents>/mini_tube/mini_tube.db` |

The last four are not decoration: without them the sandbox starts with an
**empty** database (no history, no subscriptions) on every launch and the
downloader writes nowhere. There are two entries per folder, for two different
reasons:

- **Two spellings, because the app resolves the folder from the session's
  mapping.** `XDG_CONFIG_HOME` is `~/.var/app/<app-id>/config`, and flatpak
  **generates** the `user-dirs.dirs` of that directory from the real folders of
  the host, so the app asks for the translated one (`~/Descargas`, `~/Documentos`)
  and `xdg-download`/`xdg-documents` expose exactly those: the downloads land in
  the same folder the rest of the system uses, in any language. The English
  literals are the net for a host without a usable mapping (or with an English
  one): then the app resolves `~/Downloads`/`~/Documents`, and without the grant
  it would write inside `~/.var/app/<id>`, where no file manager looks. Those two
  entries carry **no** `:create`, precisely so that a localized host does not get
  an empty English folder created next to its real one; on an English host they
  are the same directory as the `xdg-*` entry anyway.
- **`:create`, because the folder may not exist yet.** On a fresh immutable
  install (Silverblue/Kinoite/Bazzite, MicroOS/Aeon, SteamOS) `~/Documents` may
  never have been created; without `:create` flatpak has nothing to expose, the
  app writes into its sandbox-private home and the history is recreated empty on
  every launch. `:create` (on the `xdg-*` entries) makes flatpak create the host
  directory on first run.

Hardware decoding on **Intel** also needs the VAAPI driver extension:

```sh
flatpak install --user flathub org.freedesktop.Platform.VAAPI.Intel//25.08
```

AMD and NVIDIA get their drivers from the runtime/Mesa; if VAAPI is not
available mpv falls back to software and the app's codec ladder notices it
(`soft-all`). Extra folders can always be granted with
`flatpak override --user --filesystem=... io.github.marlo4220mc.NeverTube`.

## Build it

```sh
packaging/linux/flatpak/build.sh
```

It needs `flatpak`, `flatpak-builder` **>= 1.4** and `appstreamcli` **>=
1.0 with its compose component** — the `appstream` package, plus
`appstream-compose` when the distribution splits it (Ubuntu 24.04 does: the CLI
only dispatches `compose` to `/usr/libexec/appstreamcli-compose`). The script
checks the three before starting and refuses with the reason instead of
discovering it at the end of the build. It adds Flathub as a user remote,
installs
`org.kde.Platform//5.15-25.08` and `org.kde.Sdk//5.15-25.08` if they are
missing, builds, validates the desktop integration, runs the **offline
selftests inside the sandbox** and leaves:

```
dist/flatpak/repo                                  the OSTree repo
dist/flatpak/nevertube-<ver>-x86_64.flatpak        the bundle
dist/flatpak/nevertube-repo-<ver>-x86_64.tar.gz    the repo in one file
```

The tarball is not a convenience: an OSTree repo is thousands of objects plus
symlinks in `refs/heads`, and the things that carry it around (a CI artifact, a
plain upload to a web server) do not preserve symlinks. `tar -xzf` on the other
end gives back the repo `flatpak remote-add` expects, and that is what the GitHub
artifact holds.

The branch it downloads is verified to exist: `org.kde.Platform//5.15-25.08`
(~367 MB) and its matching `org.kde.Sdk//5.15-25.08` (~800 MB) are both published
on Flathub for x86_64 and aarch64 (checked against the Flathub API on
2026-10-26). It is the last Qt 5.15 KDE line, and it is the reason the manifest
stays on Qt 5.

Signing is opt-in: `FLATPAK_GPG_KEY=<key id> packaging/linux/flatpak/build.sh`
signs both the bundle and the repo. Without a key the repo has to be added with
`--no-gpg-verify`.

The build takes a while (FFmpeg, libplacebo and mpv are compiled from source):
about 12 minutes on a CI runner. Every run rebuilds those three modules
(`--force-clean`; ccache is not enabled), so what is reused between runs is the
download of the runtime and the SDK, which flatpak keeps in
`~/.local/share/flatpak`.

## What is inside (and why it is built here)

The runtime gives glibc, Qt 5.15 and the graphics stack; the manifest adds the
five libraries the app needs on top:

| module | why it is not in the runtime |
|---|---|
| `ffmpeg` (LGPL, shared) | the runtime ships **no** ffmpeg CLI. This one serves both consumers: libmpv links the libraries to demux/decode, and the **downloader** runs `/app/bin/ffmpeg` to mux (`-c copy`) and to encode the mp3 |
| `lame` | the mp3 encoder for "audio only" downloads (LGPL v2, not GPL). It is 4.0: 3.101 does not compile with the GCC of the 25.08 SDK |
| `libass` | a **hard** dependency of mpv 0.41: it cannot be disabled |
| `libplacebo` | the other hard dependency of mpv 0.41 (GPU renderer) |
| `libXpresent` | the only piece of mpv's X11 support the SDK does not ship (its `features['x11']` needs x11, xscrnsaver, xext, xpresent and xrandr, and the four others are in the SDK). Without it mpv dies with `Dependency "xpresent" not found` |
| `mpv` | built as **libmpv only** (`-Dlibmpv=true -Dcplayer=false`), with pulse and VAAPI. It is a **GPL** build on purpose: mpv gates its X11 support behind `gpl`, and `vaapi-x11` (the VA display it opens from `MPV_RENDER_PARAM_X11_DISPLAY`) needs it -- with `-Dgpl=false` meson aborts with `Feature x11 cannot be disabled: the build is not GPL!`, and the app would decode in software on X11. The app is GPL-3.0-or-later, so the bundle is consistent; FFmpeg stays LGPL |

The FFmpeg component list mirrors `tools/build_ffmpeg_i686.sh` (the Win7
package), so both builds expose the same protocols, demuxers, decoders and
muxers. The Linux-only additions are `--enable-gnutls` (TLS), `--enable-libdav1d`
(software AV1) and `--enable-vaapi` + the three VAAPI hwaccels.

The app itself is compiled with `cmake-ninja` from `../../..`. On Linux CMake
also compiles the `FfmpegBackend` when `pkg-config` finds the libav `.pc` files
(they are there), so `NEVERTUBE_BACKEND=ffmpeg` and `--selftest-ffmpeg` work;
the **default** backend stays `MpvBackend`, which is the protected piece.

The last module installs the desktop file, the metainfo and the 7 icons under
the **app id** and removes the native `nevertube.*` copies that CMake installs.
That is not cosmetic: under Flatpak the Wayland `app_id` is the app id, and the
compositor looks up `<app_id>.desktop` to associate the window with the
launcher. `src/main.cpp` reads `FLATPAK_ID` for exactly this reason (outside a
sandbox it keeps `nevertube.desktop`).

### DRM and Vulkan: left on `auto` on purpose

The `mpv` module passes neither `-Ddrm` nor `-Dvulkan`, and that is a decision,
not an omission:

- **DRM** (`-Ddrm`) only builds `vo_drm` and `drmprime`, the two output paths the
  app never uses: the video is rendered by libmpv into the `QOpenGLWidget`
  (`vo=libmpv`, the protected piece). Forcing it also needs libdrm +
  libdisplay-info + a VT header, so it adds a build-failure mode for a code path
  nothing calls. What the app **does** need is the GL dmabuf interop
  (`dmabuf_interop_gl.c`), and in mpv 0.41 that is gated by
  `features['egl'] and drm.found()` — that is `libdrm.pc` alone, which the SDK
  ships and meson finds by itself. Hardware decode works without touching the
  option.
- **Vulkan**: libplacebo's `vulkan` option defaults to `auto` and publishes
  `pl_has_vulkan`; mpv's `vulkan` option is `auto` too and keys off that variable
  plus `vulkan.pc`. So the SDK decides, which is what we want: `hwdec=auto`
  tries VAAPI first on Linux and Vulkan only matters as the fallback (NVIDIA).
  Forcing `enabled` would break the build on a runtime branch without
  `vulkan.pc` for a path that is not the primary one; with `auto`, such a branch
  still builds and only loses the Vulkan fallback.

The one thing that is not `auto` is the device the sandbox exposes: without
`--device=dri` in `finish-args` VAAPI cannot open the GPU and mpv falls back to
software, which is why that permission is part of the manifest.

## Why not an AppImage (measured)

Two independent reasons, both measured:

1. **An AppImage type 2 needs `libfuse.so.2`, and the immutable distributions
   do not ship it.** Silverblue 40+ dropped fuse2 with `rpm-ostree` 2024.7
   (`fuse-libs` is no longer in the base), so the file does not even start:
   *"AppImages require FUSE to run"*. Ubuntu 24.04 renamed the package to
   `libfuse2t64` and it is not installed by default either. The only workaround
   is `--appimage-extract-and-run`, which means asking the user to run an
   unpacked directory by hand. On SteamOS the same thing.
2. **An AppImage still takes the GL stack, the VA-API drivers and (if it does
   not bundle it) libmpv from the host.** That is the old failure mode: the
   previous AppImage worked on one distribution and failed on the next because
   the host's libmpv ABI and GPU stack were different. Bundling libmpv does not
   fix it either: the bundle then carries a Mesa/LLVM it does not control, and
   the hardware decode path depends on the host's kernel driver.

A Flatpak solves both **structurally**: the runtime brings a fixed glibc and Qt,
the GL/VAAPI extensions bring the driver stack, and `--device=dri` is the only
thing that comes from the host (the kernel driver, which is the one thing that
*must* match the hardware).

"One AppImage per distro" is not the answer either: the axis that matters is not
the distribution name, it is the **glibc baseline** and the **GPU stack**, and
those cut across distros (two "different" distros can share both, and two
versions of the same distro can differ).

## CI

`.github/workflows/linux.yml` has a `build-flatpak` job, next to the `.deb`,
`.rpm` and pacman ones:

- **Runner**: a plain `ubuntu-24.04` with `flatpak`, `flatpak-builder`,
  `elfutils`, `appstream`, `appstream-compose` and `desktop-file-utils`
  installed from apt, then `bash packaging/linux/flatpak/build.sh`. There is
  **no** container image and no
  privileged mode: the script passes `--disable-rofiles-fuse` itself, so no FUSE
  setup is needed either.
  (The `ghcr.io/flathub-infra/flatpak-builder` image was the first attempt and it
  was dropped: the registry answers `403 invalid token` for its tags, so the tag
  could not be verified from here, and a job that dies on an unverifiable image
  teaches nothing.)
- **Trigger**: manual (`gh workflow run linux.yml --ref main`) or a push that
  touches the workflow file itself — the same policy as the other Linux jobs. It
  does not run on every push to `main`.
- **What it does**: builds the manifest, validates the `.desktop` and the
  metainfo, checks the installed tree and runs the **offline selftests inside the
  sandbox** with `flatpak-builder --run`. That last part is the one that proves
  libmpv, libav and the Qt sqlite plugin resolve from the runtime and not from a
  host package; at the end the bundle and the OSTree repo (with static deltas)
  are produced.
- **Artifacts**: `linux-flatpak`, with `dist/flatpak/*.flatpak` and
  `dist/flatpak/nevertube-repo-*.tar.gz`, kept 21 days. The repo travels as a tar
  and not as its directory on purpose: `upload-artifact` does not preserve
  symlinks, so a directory upload comes back as something `flatpak remote-add`
  cannot read. It is **unsigned** (CI has no key), so the repo is added with
  `--no-gpg-verify`, exactly as documented above.
- **Timeout**: 120 minutes. The first build compiles FFmpeg, libplacebo and mpv
  from source and nothing is cached between runs.

The runner is **24.04 and not 22.04**, and that is measured, not taste. The
export step composes the AppStream metadata, and the tool that does it changed
owner: 22.04 ships `flatpak-builder` 1.2.2, which runs `appstream-compose`
**inside the SDK sandbox**, and freedesktop-sdk 24.08+ (the base of
`org.kde.Sdk//5.15-25.08`) no longer ships that binary -- the first CI run died
in the *last* step, with FFmpeg, libplacebo, mpv and the app already built:
`bwrap: execvp appstream-compose: No such file or directory`. `flatpak-builder`
1.4 (24.04 ships 1.4.2) calls `appstreamcli compose` on the host instead, and
`appstreamcli` 1.0 arrived with that subcommand. `build.sh` checks both versions
before starting, so the failure is a sentence instead of 15 minutes.

24.04 has one cost, handled in the workflow: it restricts unprivileged user
namespaces through AppArmor (`kernel.apparmor_restrict_unprivileged_userns=1`)
and every module of the build runs inside `bwrap`, which is built on exactly
that. The `build-flatpak` job lifts the restriction on the throwaway runner
(conditionally: the sysctl is absent on kernels without it) before building.

A full `flatpak run` of the installed bundle is **not** part of CI: `bwrap`
inside a container is fragile and there is no display. It is a manual check on a
real machine (or on the immutable test box):

```sh
flatpak install --user NeverTube-<ver>-Linux-Flatpak.flatpak
flatpak run io.github.marlo4220mc.NeverTube
# hardware decode check:
flatpak run io.github.marlo4220mc.NeverTube --selftest-player   # needs a GPU
```

That launch is the only thing this directory cannot prove by itself. What it has
to confirm, in this order:

1. The window opens (Qt finds its platform plugin in the runtime) and the
   launcher shows the NeverTube icon, not a generic one — that is the `app_id`
   plus `FLATPAK_ID` pair working.
2. `mini_tube.db` appears **on the host** and is still there with its contents on
   the second launch. It is in the documents folder of the host, translated:
   `ls ~/Documents/mini_tube` on an English host and `ls ~/Documentos/mini_tube`
   on a Spanish one (the sandbox carries the host's mapping, and a database that
   a native install left in another folder is moved there on the first launch).
   If the history is empty every time, the `--filesystem` grant did not take and
   the app is writing into `~/.var/app/<id>`.
3. A download lands in the downloads folder of the host, also translated
   (`~/Descargas/NeverTube` in Spanish, `~/Downloads/NeverTube` in English), and
   **no empty English folder appears** next to the real one — that is the
   `systemDownloadsFolder()` resolution (the `XDG_DOWNLOAD_DIR` line of the
   mapping) doing its job.
4. Hardware decode: with `--device=dri` and the VAAPI extension installed,
   `--selftest-player` has to resolve `hwdec=auto` to VAAPI; without them it has
   to fall back to software and still play (the codec ladder does that).
5. "Abrir carpeta" opens the host file manager. The app asks `QDesktopServices`
   first, which is the documented portal path for Qt/KDE inside a sandbox
   (`org.freedesktop.portal.OpenURI`); `gio open` is only the fallback and inside
   the sandbox it cannot launch a host file manager, so this is the one item that
   has to be seen on a real machine.
6. A second `flatpak run` while it is open does not open a second window.

## Flathub (later)

Not required to ship. When it is attempted:

- the app id is already reverse-DNS (`io.github.marlo4220mc.NeverTube`), which is
  what Flathub requires;
- they **rebuild from source** in their infrastructure: the sources must be
  pinned (they are: every module has a `sha256`, and libplacebo a commit) and the
  build must not need the network;
- `flatpak-builder-lint` has to be clean and the permissions minimal (the list
  above is already minimal for what the app does);
- the metainfo needs screenshots (`<screenshots>`), which are not there yet.

## Known limits

- The **first** build compiles FFmpeg + libplacebo + mpv: on a laptop it is
  several minutes. CI caches nothing yet.
- The runtime is the **last Qt5 KDE line** (`5.15-25.08`). A future Qt6 port
  would move to `org.kde.Platform//6.x`.
- The downloader's `ffmpeg` is `/app/bin/ffmpeg` (on `PATH` inside the sandbox),
  so `NEVERTUBE_FFMPEG` is not needed. Outside the Flatpak it is still the
  system's `ffmpeg`.
- Live streams, AV1 over HLS and the downloader are the same code paths as the
  native packages: nothing is disabled in the Flatpak build.
- Single instance: the app hands the arguments of a second launch to the running
  one through a `QLocalServer` socket in `/tmp`. Since Flatpak 1.12 `/tmp` is a
  private tmpfs **shared by the instances of the same app id**, so that keeps
  working inside the sandbox; on Flatpak older than 1.12 each instance got its
  own `/tmp` and a second window would open (no current immutable distribution
  ships that old).
