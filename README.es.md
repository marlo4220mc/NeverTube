<div align="right">
  <a href="README.md">English</a> · <b>Español</b>
</div>

# nevertube_qt2

Un cliente ligero de YouTube para Windows (7 / 10 / 11) y Linux, hecho con Qt5 y C++17. Reproduce directamente desde la API web no oficial de YouTube — sin navegador, sin Electron y, en Linux, sin runtime empaquetado: los paquetes pesan ~900 KB y se traen sus dependencias de los repositorios de la propia distribución.

## Funciones

- **Dos motores de reproducción**: libmpv (Linux y Windows x64) y un motor FFmpeg propio desde cero para Windows x86 (Win7)
- Cuadrícula y lista, búsqueda con sugerencias en vivo, páginas de canal, playlists, historial
- Cola "A continuación" con siembra por radio / sugerencias, mini player, aleatorio y repetición
- Directos (insignia "EN VIVO" y modo de reproducción en directo) y AV1 sintético sobre HLS para los perfiles de códec modernos
- Sondeo de hardware al inicio (AV1 / VP9 / H.264) que elige el perfil que tu máquina decodifica de verdad
- Recuperación automática de sesiones gateadas por YouTube (403): re-resolución en una sesión nueva y, como último recurso, reinicio de la app con el último vídeo
- Cambio de idioma en caliente (español / inglés) y 7 colores de acento

## Paquetes

Compilados por objetivo en CI; en Linux solo x86_64.

| Formato | Destino | Fichero |
|---|---|---|
| `.deb` | Ubuntu 22.04 / 24.04 | `nevertube-qt2_<versión>_amd64.deb` |
| `.rpm` | Fedora (familia RHEL con esos sonames) | `nevertube-qt2-<versión>-1.x86_64.rpm` |
| pacman | Arch Linux | `nevertube-qt2-<versión>-x86_64.pkg.tar.zst` |
| setup.exe | Windows 10 / 11 (x64) | `nevertube-setup-x64.exe` |
| setup.exe | Windows 7 (x86) | `nevertube-setup.exe` |

### Instalación

- Debian / Ubuntu: `sudo apt install ./nevertube-qt2_<versión>_amd64.deb` — apt instala automáticamente las dependencias declaradas.
- Fedora: `sudo dnf install ./nevertube-qt2-<versión>-1.x86_64.rpm` — dnf resuelve los requisitos por sonames (`ffmpeg-free`, o el ffmpeg de RPM Fusion si ya está instalado).
- Arch: `sudo pacman -U nevertube-qt2-<versión>-x86_64.pkg.tar.zst`
- Windows: ejecuta el setup; instala el re-distribuible de VC++ que necesita.

### Compilar desde el código

```sh
cmake -B build && cmake --build build
```

Dependencias: Qt5 (Core, Gui, Widgets, Network, Sql), OpenGL, libmpv. La compilación de Windows x86 usa el paquete de FFmpeg incluido en `third_party/ffmpeg-win32/` (ver `BUILD_WINDOWS.md`).

## Limitaciones

- Windows x86 (Win7): sin AV1 (ni hardware ni software); el perfil de códec llega a `h264-hw` (DXVA2).
- Windows x64: la ruta por defecto `d3d11va-copy` consume más CPU; hay un modo experimental de zero-copy (`NEVERTUBE_ANGLE=1`, ANGLE + `d3d11-egl`) opt-in.
- La app usa la API web no oficial de YouTube para resolver streams y el estado en vivo. Este proyecto no está afiliado, respaldado ni conectado oficialmente con YouTube o Google.

## Licencia

GPL-3.0-or-later. Los componentes de terceros conservan sus propias licencias: Qt5 (LGPL-3.0), libmpv (LGPL-2.1+), FFmpeg (LGPL-2.1+), geometría de iconos Material Symbols (Apache-2.0), OpenSSL (Apache-2.0).