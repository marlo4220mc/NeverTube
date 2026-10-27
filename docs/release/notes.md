# NeverTube @@VERSION@@

**NeverTube** is a lightweight YouTube client written in Qt5 and C++17: it talks
to YouTube directly and plays with libmpv, with no browser, no Electron and no
bundled web engine. The Linux packages are about 1 MB and take Qt, mpv and
FFmpeg from the distribution; the Windows installers carry their own copies.

Released on @@DATE@@ (tag `@@TAG@@`).

@@PRERELEASE_NOTE@@

## What it does

**Playback**

- Videos and live streams (`EN VIVO` badge, live progress bar), with resume
  where you left off and the progress painted on the thumbnails.
- Hardware decoding picked at startup by a probe (AV1 / VP9 / H.264): the codec
  that the machine really decodes wins, and the app steps down by itself if a
  profile ends up decoding in software.
- "Up next" queue seeded with YouTube's radio mix (Music style) or with static
  suggestions; shuffle and repeat work on the real queue.
- Mini player while you browse, and a shortcut set for the basics
  (`Space`/`K`, `J`/`L`, `←`/`→`, `N`, `P`, `↑`/`↓`, `M`, `F`, `D`).
- Automatic recovery from YouTube's gated sessions (403s): re-resolve in a fresh
  session and, as a last resort, restart with the last video.

**Browsing**

- Home with Continue watching, New from your subscriptions, Recommended and
  Discover (the radios you play are saved and reused, no network needed).
- Search with live suggestions; pasting a YouTube link opens that video instead
  of searching the text.
- Channel pages (videos / shorts / live), playlists, history, and a library with
  subscriptions, playlists and watch later.
- Shorts are kept out of the lists: the player is 16:9 and a reel in a grid
  breaks the row.

**Downloads**

- From the player button or the Downloads tab: resolution submenu with the
  codec variants and their real size, or audio only (MP3).
- Byte-range fragments in parallel and resumable; an expired URL (401/403) is
  re-resolved and the download continues from what is already on disk.
- Muxed with `ffmpeg` as a stream copy (no re-encoding): video as MKV, audio as
  MP3 (M4A when the ffmpeg has no MP3 encoder).

**Interface**

- Grid (4 / 5 / 6 columns) and list views, three thumbnail sizes, seven accent
  colors, English and Spanish with live switching.
- One instance per user: a second launch (a link, a `--download`) hands its
  arguments to the running window.
- No telemetry: nothing is sent anywhere except YouTube itself.

## Downloads

| File | Platform |
|---|---|
| `NeverTube-@@VERSION@@-Windows-10-11-x64-Setup.exe` | Windows 10 / 11, 64-bit |
| `NeverTube-@@VERSION@@-Windows-10-11-x86-Setup.exe` | Windows 10 / 11, 32-bit |
| `NeverTube-@@VERSION@@-Windows-7-SP1-x86-Setup.exe` | Windows 7 SP1, 32-bit |
| `NeverTube-@@VERSION@@-Ubuntu-22.04-amd64.deb` | Ubuntu 22.04, Linux Mint 21, Pop!_OS 22.04, Debian 12 |
| `NeverTube-@@VERSION@@-Ubuntu-24.04-amd64.deb` | Ubuntu 24.04+, Linux Mint 22, Debian 13 |
| `NeverTube-@@VERSION@@-Fedora-x86_64.rpm` | Fedora (and RHEL-family with those sonames) |
| `NeverTube-@@VERSION@@-Arch-Linux-x86_64.pkg.tar.zst` | Arch Linux |
| `NeverTube-@@VERSION@@-Linux-Flatpak.flatpak` | Any distribution, including the immutable ones |
| `NeverTube-@@VERSION@@-Linux-Flatpak-Repo.tar.gz` | The OSTree repo, for `flatpak update` |
| `SHA256SUMS.txt` | Checksums of everything above |

There are **two `.deb`** on purpose. They are the same program, built against
each base: 24.04 renamed the Qt5 runtime packages (`libqt5core5t64`...) and
ships FFmpeg 6.1, so a package built on 22.04 cannot install there and vice
versa. Take the one that matches your distribution.

## Install

**Windows.** Run the setup for your system. Each one refuses the other two and
tells you which to use, so picking the wrong file cannot break anything:
`Windows-7-SP1-x86` is 32-bit Windows 7 SP1, `Windows-10-11-x86` is 32-bit
Windows 10/11 and `Windows-10-11-x64` is 64-bit Windows 10/11. They install the Visual C++ runtime they need and
create a menu entry and a desktop shortcut. Once installed, the app checks for
new versions by itself (Ajustes -> "Buscar actualizaciones") and installs the
next one over this same copy: your history, subscriptions, settings and downloads
are outside the program's folder and are never touched. The installer is unsigned
(see the limitations), so Windows may warn the first time.

**Debian / Ubuntu.**

```sh
sudo apt install ./NeverTube-@@VERSION@@-Ubuntu-22.04-amd64.deb
```

The project also publishes an APT repository (`jammy` for 22.04, `noble` for
24.04) on GitHub Pages, signed with its own key, so that `apt update && apt
upgrade` keeps the app current. If it is not published yet, the `.deb` above is
the whole story:

```sh
curl -fsSL https://marlo4220mc.github.io/NeverTube/apt/key.asc -o /tmp/nevertube.asc
gpg --dearmor -o /tmp/nevertube.gpg /tmp/nevertube.asc        # then, as root:
install -Dm644 /tmp/nevertube.gpg /etc/apt/keyrings/nevertube.gpg
echo 'deb [signed-by=/etc/apt/keyrings/nevertube.gpg] https://marlo4220mc.github.io/NeverTube/apt/ jammy main' \
  > /etc/apt/sources.list.d/nevertube.list
apt update && apt install nevertube
```

**Fedora.**

```sh
sudo dnf install ./NeverTube-@@VERSION@@-Fedora-x86_64.rpm
```

**Arch.**

```sh
sudo pacman -U NeverTube-@@VERSION@@-Arch-Linux-x86_64.pkg.tar.zst
```

**Flatpak** (the way in on Silverblue / Kinoite / Bazzite, MicroOS / Aeon and
SteamOS, where an `rpm` or a `.deb` is not an option):

```sh
flatpak install --user NeverTube-@@VERSION@@-Linux-Flatpak.flatpak
flatpak run io.github.marlo4220mc.NeverTube
```

The Flatpak carries its own Qt, libmpv and ffmpeg, so it is the same binary on
every distribution. The `.flatpak` file is a bundle with no update channel: for
`flatpak update` to work you need the repository. Two ways:

```sh
# 1. the published repository (signed; the .flatpakrepo file carries the key)
flatpak remote-add --user --if-not-exists nevertube \
  https://marlo4220mc.github.io/NeverTube/flatpak/nevertube.flatpakrepo
flatpak install --user nevertube io.github.marlo4220mc.NeverTube

# 2. or the repository of THIS release (unpack and add it)
tar -xzf NeverTube-@@VERSION@@-Linux-Flatpak-Repo.tar.gz
flatpak remote-add --user --no-gpg-verify nevertube ./repo
```

Option 1 needs the repository to have been published (it is signed with the
project's GPG key, and `flatpak` verifies that signature). Option 2 works with
the file attached here, and it is the one that needs `--no-gpg-verify`: that
tarball travels before the signature is applied.

## Which engine runs where

It is not the same code on every Windows target, and the difference is visible:

| Target | Engine | What it decodes |
|---|---|---|
| Linux, Windows x64, Windows 10 x86 | libmpv (`hwdec=auto`) | AV1 / VP9 / H.264 in hardware when the GPU can, software otherwise |
| Windows 7 x86 | FFmpeg (no libmpv) | H.264 in hardware (DXVA2), VP9 in software. **No AV1**: there is no 32-bit decoder for it |

On 32-bit Windows the AV1 hardware decoders do not exist and there is no
software one either, so YouTube is asked for H.264/VP9 there.

## Known limitations (read before installing)

- **This is an alpha.** Features can change, some screens are rough, and a video
  can stop working because YouTube changed something on their side.
- YouTube has no public API: everything is an unofficial endpoint. An update of
  theirs can break playback, search or the queue until this app catches up.
- A download needs the `ffmpeg` **command line** at runtime. The Windows
  installers ship it; on Linux the packages declare it as a dependency.
- The `.flatpak` bundle and the Windows installers are **unsigned**: only their
  SHA-256 is published (`SHA256SUMS.txt`), which proves the file arrived whole
  and is the one that was built, not who built it. The repositories (APT and
  Flatpak) are signed, and that is what `apt` and `flatpak` verify. The app is
  not on Flathub (yet).
- On Windows the app updates itself from its own GitHub releases; on Linux the
  package manager does (Ajustes only shows the available version).
- Windows 7: the app is 32-bit only there, and its engine is the FFmpeg one (see
  the table above).
- The 32-bit Windows 10 build pins an older libmpv (2026-03) because newer
  builds abort on start-up with an OpenSSL 3 assertion in their 32-bit path. It
  will move forward when the fix reaches those builds.
- Shorts are filtered out of the queue, Discover and Continue watching.
- One instance per **user**, not per session: with two graphical sessions, a
  second launch brings the window of the first one to the front.

## Verify the download

```sh
sha256sum -c SHA256SUMS.txt
```

`SHA256SUMS.txt` covers every file attached to this release.

## Reporting bugs

Open an issue at <https://github.com/marlo4220mc/NeverTube/issues> and include
the version, the platform and, if playback is involved, the log:

```sh
NEVERTUBE_DEBUG=1 nevertube
```

(On Windows, set `NEVERTUBE_DEBUG=1` before launching the `.exe`; a console opens
with the log. Without it the app opens no terminal at all.)

## Licence

GPL-3.0-or-later. libmpv in the Flatpak and in the Windows builds is a GPL
build (it needs `gpl` for X11 support); FFmpeg stays LGPL v2.1+.

<details>
<summary><b>Español</b></summary>

## NeverTube @@VERSION@@

**NeverTube** es un cliente ligero de YouTube escrito en Qt5 y C++17: habla
directamente con YouTube y reproduce con libmpv, sin navegador, sin Electron y
sin motor web embebido. Los paquetes de Linux pesan ~1 MB y toman Qt, mpv y
FFmpeg de la distribución; los instaladores de Windows llevan sus propias
copias.

Publicado el @@DATE@@ (tag `@@TAG@@`).

@@PRERELEASE_NOTE@@

### Qué hace

- **Reproducción**: vídeos y directos (marca `EN VIVO` y barra sin duración
  final), reanudar donde lo dejaste, progreso pintado en las miniaturas,
  decodificación por hardware elegida al arrancar con una sonda (AV1 / VP9 /
  H.264) y descenso automático si el perfil acaba decodificando en software.
- **Cola "A continuación"** con la radio de YouTube (estilo Music) o con
  sugerencias estáticas; mezclar y repetir funcionan sobre la cola real.
- **Mini reproductor** mientras navegas, y atajos: `Espacio`/`K`, `J`/`L`,
  `←`/`→`, `N`, `P`, `↑`/`↓`, `M`, `F`, `D`.
- **Navegación**: Inicio con Continuar viendo, Novedades de tus suscripciones,
  Recomendado y Descubre (las radios que reproduces se guardan y se reutilizan);
  búsqueda con sugerencias; pegar un enlace abre ese vídeo; canales (vídeos /
  shorts / directos), playlists, historial y biblioteca.
- **Descargas**: submenú de resolución con los códecs y su tamaño real, o solo
  audio en MP3. Fragmentos por rango en paralelo, reanudables, y una URL
  caducada (401/403) se vuelve a resolver sin perder lo descargado. El mux va
  con `ffmpeg` y `-c copy` (sin recodificar).
- **Interfaz**: cuadrícula (4 / 5 / 6 columnas) y lista, tres tamaños de
  miniatura, siete colores de acento, inglés y español con cambio en vivo, una
  instancia por usuario y sin telemetría.

### Descargas

Mismos ficheros que la tabla de arriba: `Windows-10-11-x64-Setup.exe`
(Windows 10/11 64 bits), `Windows-10-11-x86-Setup.exe` (Windows 10/11 32
bits), `Windows-7-SP1-x86-Setup.exe` (Windows 7 SP1 32 bits), los dos `.deb`
(`Ubuntu-22.04` para 22.04 / Mint 21 / Debian 12 y `Ubuntu-24.04` para 24.04+ /
Mint 22 / Debian 13: son el mismo programa compilado contra cada base), el
`.rpm` de Fedora, el paquete de Arch Linux, el bundle `Linux-Flatpak` (para
cualquier distribución, incluidas las inmutables) y el tar
`Linux-Flatpak-Repo` del repositorio OSTree para `flatpak update`. Todos
llevan por delante `NeverTube-<versión>-`, la misma convención en los nueve
ficheros.

### Instalación

```sh
# Debian / Ubuntu
sudo apt install ./NeverTube-@@VERSION@@-Ubuntu-22.04-amd64.deb

# Fedora
sudo dnf install ./NeverTube-@@VERSION@@-Fedora-x86_64.rpm

# Arch
sudo pacman -U NeverTube-@@VERSION@@-Arch-Linux-x86_64.pkg.tar.zst

# Flatpak
flatpak install --user NeverTube-@@VERSION@@-Linux-Flatpak.flatpak
flatpak run io.github.marlo4220mc.NeverTube
```

En Windows, ejecuta el setup de tu sistema: cada uno rechaza los otros dos y te
dice cuál usar. Una vez instalado, la aplicación busca actualizaciones por su
cuenta (Ajustes -> "Buscar actualizaciones") e instala la siguiente encima de la
misma copia: el historial, las suscripciones, los ajustes y las descargas están
fuera de la carpeta del programa y no se tocan.

También hay repositorios publicados en GitHub Pages, firmados con la clave del
proyecto, para que actualicen `apt` y `flatpak`: el de APT tiene dos suites
(`jammy` para 22.04, `noble` para 24.04) y el de Flatpak se añade con su
`.flatpakrepo`. Si todavía no están publicados, los ficheros de arriba son todo
lo necesario.

### Límites conocidos

- **Es una alfa.** Puede haber fallos, algunas pantallas están sin pulir y un
  vídeo puede dejar de funcionar porque YouTube cambió algo.
- YouTube no tiene API pública: si cambian sus endpoints, la reproducción, la
  búsqueda o la cola pueden romperse hasta que la app se adapte.
- Las descargas necesitan el **ejecutable** `ffmpeg` en tiempo de ejecución (los
  instaladores de Windows lo llevan; en Linux es una dependencia del paquete).
- Windows 7 es de 32 bits y usa el motor FFmpeg: H.264 por hardware (DXVA2),
  VP9 en software y **nada de AV1**.
- El build de 32 bits de Windows 10 lleva un libmpv de 2026-03 porque los
  posteriores abortan al arrancar por un fallo de OpenSSL en su ruta de 32 bits.
- Los Shorts se filtran de la cola, de Descubre y de Continuar viendo.
- El `.flatpak` y los instaladores de Windows **no están firmados**: solo se
  publica su SHA-256 (`SHA256SUMS.txt`), que prueba que el archivo llegó entero y
  es el que se compiló, no quién lo compiló. Los repositorios (APT y Flatpak) sí
  están firmados, y es lo que verifican `apt` y `flatpak`. La aplicación no está
  en Flathub (todavía).
- En Windows la aplicación se actualiza sola desde sus propios releases de
  GitHub; en Linux actualiza el gestor de paquetes (Ajustes solo muestra la
  versión disponible).
- Una instancia por **usuario/sesión**: con dos sesiones gráficas, la segunda
  apertura trae al frente la ventana de la primera.

### Verificar y reportar

`sha256sum -c SHA256SUMS.txt` comprueba todo lo descargado. Para reportar un
fallo, abre un issue en <https://github.com/marlo4220mc/NeverTube/issues> con la
versión, la plataforma y el log (`NEVERTUBE_DEBUG=1`).

### Licencia

GPL-3.0-or-later. libmpv va como build GPL (lo necesita para X11) y FFmpeg sigue
siendo LGPL v2.1+.

</details>
