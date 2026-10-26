<div align="right">
  <a href="README.md">English</a> · <b>Español</b>
</div>

# NeverTube

**NeverTube** es un cliente ligero de YouTube para Windows (7 / 10 / 11) y Linux, hecho con Qt5 y C++17. Reproduce directamente desde la API web no oficial de YouTube — sin navegador, sin Electron y, en Linux, sin runtime empaquetado: los paquetes pesan ~1 MB y se traen sus dependencias de los repositorios de la propia distribución.

## Funciones

**Reproducción**

- **Dos motores de reproducción**: libmpv en Linux y Windows x64, y un motor FFmpeg/libav propio para Windows x86 (Win7)
- Sondeo de hardware al inicio (AV1 / VP9 / H.264) que elige el perfil de códec que la máquina decodifica de verdad, y baja un escalón solo si un perfil termina decodificando por software
- Directos (insignia en la miniatura y modo de reproducción en directo), AV1 sintético sobre HLS, retoma donde lo dejaste
- Cola "A continuación" sembrada con la radio de YouTube (mezcla estilo Music) o con sugerencias estáticas, aleatorio y repetición, mini player
- Atajos: `Espacio` / `K` reproducir-pausa, `J` / `L` ±10 s, `←` / `→` ±5 s, `N` siguiente, `P` anterior (reinicia primero si pasaste los 5 s), `↑` / `↓` volumen, `M` silencio, `F` pantalla completa, `D` menú de descarga
- Recuperación automática de sesiones gateadas por YouTube (403): re-resolución en una sesión nueva y, como último recurso, reinicio con el último vídeo

**Navegación**

- Inicio con Continuar viendo, Nuevos de tus suscripciones, Recomendado y Descubre (las radios que se generan de lo que reproducís quedan guardadas y se reutilizan)
- Búsqueda con sugerencias en vivo; pegar un enlace de YouTube abre ese vídeo en vez de buscar el texto
- Páginas de canal (pestañas vídeos / shorts / directos), playlists, historial
- Biblioteca con suscripciones, playlists e historial; el menú de la fila cubre Ver más tarde, añadir a playlist, compartir y ver canal
- Los Shorts quedan fuera de las listas (cola, continuar viendo, descubre): el reproductor es 16:9 y un vertical en una cuadrícula rompe la fila

**Descargas** — desde el botón del reproductor o la pestaña Descargas

- Submenú de resolución con las variantes de códec y su tamaño real, o solo el audio
- Fragmentos pedidos por rango de bytes en paralelo y reanudables; un 401/403 re-resuelve las URLs y continúa con lo que ya está en disco
- Unido con `ffmpeg` como copia de flujo (sin recodificar): el vídeo como MKV, el audio como MP3 (M4A cuando el ffmpeg no trae codificador MP3). Los instaladores de Windows llevan su propio ffmpeg
- Una descarga a la vez, con vídeo y audio como filas separadas; progreso y velocidad por fila, pausar / continuar, reintentar, borrar (opcionalmente el fichero), abrir carpeta, pausar todo / reanudar todo
- Carpeta por defecto `Descargas/NeverTube` (configurable), de 1 a 16 conexiones (8 es el óptimo medido) y, mientras hay un vídeo reproduciéndose, la descarga baja la prioridad de sus propios hilos para que el reproductor no pierda fotogramas

**Interfaz**

- Cuadrícula (4 / 5 / 6 columnas) y lista, tres tamaños de miniatura, siete colores de acento, cambio de idioma en caliente (español / inglés; un idioma nuevo es un fichero JSON en `localization/`)
- Una instancia por usuario: un segundo lanzamiento (un enlace, un `--download`) le pasa sus argumentos a la ventana que ya está abierta en vez de abrir una segunda copia de la base

## Paquetes

Compilados por objetivo en CI; en Linux solo x86_64.

| Formato | Destino | Fichero |
|---|---|---|
| `.deb` | Ubuntu 22.04 / 24.04 | `nevertube_<versión>_amd64.deb` |
| `.rpm` | Fedora (familia RHEL con esos sonames) | `nevertube-<versión>-1.x86_64.rpm` |
| pacman | Arch Linux | `nevertube-<versión>-x86_64.pkg.tar.zst` |
| setup.exe | Windows 10 / 11 (x64) | `nevertube-setup-x64.exe` |
| setup.exe | Windows 7 (x86) | `nevertube-setup.exe` |

### Instalación

- Debian / Ubuntu: `sudo apt install ./nevertube_<versión>_amd64.deb` — apt instala automáticamente las dependencias declaradas.
- Fedora: `sudo dnf install ./nevertube-<versión>-1.x86_64.rpm` — dnf resuelve los requisitos por sonames (`ffmpeg-free`, o el ffmpeg de RPM Fusion si ya está instalado).
- Arch: `sudo pacman -U nevertube-<versión>-x86_64.pkg.tar.zst`
- Windows: ejecuta el setup; instala el re-distribuible de VC++ que necesita.

El binario es `nevertube`. En Linux la entrada `.desktop` y los iconos se instalan en el tema `hicolor`, así que la app aparece en el menú como **NeverTube**.

### Compilar desde el código

```sh
cmake -B build && cmake --build build
```

Dependencias: Qt5 (Core, Gui, Widgets, Network, Sql), OpenGL, libmpv. La compilación de Windows x86 usa el paquete de FFmpeg incluido en `third_party/ffmpeg-win32/` (ver `BUILD_WINDOWS.md`); las descargas necesitan el **ejecutable** `ffmpeg` en tiempo de ejecución (los paquetes de Linux lo declaran, los instaladores de Windows lo llevan).

Tests de regresión offline (sin red ni GPU) — 23, uno por subsistema:

```sh
./build/nevertube --selftest-db        # repositorios y SQLite
./build/nevertube --selftest-grid      # geometría de la cuadrícula
./build/nevertube --selftest-theme     # QSS y paleta de selección
./build/nevertube --selftest-i18n      # catálogos de idioma
./build/nevertube --selftest-download  # plan de descarga
```

La lista completa, con lo que cubre cada uno, está en `AGENTS.md`.

## Línea de comandos

```
nevertube                            # la app
nevertube --search <consulta>        # buscar al arrancar
nevertube --channel <channelId>      # abrir un canal
nevertube --playlist <playlistId>    # abrir una playlist de YouTube
nevertube <url-de-youtube>           # reproducir un vídeo
nevertube --download <videoId> [altura|audio]
nevertube --pause-download <videoId>
nevertube --resume-download <videoId>
nevertube --play-local <fichero>     # reproducir una descarga
nevertube --downloads | --library    # abrir esa página
```

## Limitaciones

- Windows x86 (Win7): sin AV1 (ni hardware ni software); el perfil de códec llega a `h264-hw` (DXVA2). El setup x86 se niega a instalarse en Windows 10/11 de 64 bits y remite a la compilación x64, donde esa misma máquina rinde mejor.
- Windows x64: la ruta por defecto `d3d11va-copy` consume más CPU; hay un modo experimental de zero-copy (`NEVERTUBE_ANGLE=1`, ANGLE + `d3d11-egl`) opt-in.
- Los paquetes de Linux son solo x86_64.
- La app usa la API web no oficial de YouTube para resolver streams y el estado en vivo. Este proyecto no está afiliado, respaldado ni conectado oficialmente con YouTube o Google.

## Licencia

GPL-3.0-or-later. Los componentes de terceros conservan sus propias licencias: Qt5 (LGPL-3.0), libmpv (LGPL-2.1+), FFmpeg (LGPL-2.1+), LAME (LGPL-2.0, el codificador MP3 del ffmpeg de Windows), geometría de iconos Material Symbols (Apache-2.0), OpenSSL (Apache-2.0).
