# Build Windows

Windows **no** se puede compilar desde esta máquina (Linux sin toolchain mpv/Qt
mingw — solo `wine` para ejecutar, no compilar). El proyecto NO mantiene un
`.exe` propio: el build final se hace en tu equipo Windows y este documento es
la lista exacta de lo que necesita. La cuestión de "compilar en Windows" quedó
**cerrada en el CMake como costura**: en la rama `if(WIN32)` se usa
`find_package(unofficial-mpv CONFIG)` en vez de `pkg_check_modules(mpv)`, y el
resto sigue igual (Qt5, OpenGL, SQLite). Esta sección solo se ejecuta si
`WIN32`; en Linux/Unix el build queda **idéntico** (sin regresión).

## Requisitos

1. **Qt 5.15.x** (MSVC 2019 o MinGW) → al final `windeployqt` se encarga de las DLLs de Qt.
2. **mpv dev para Windows** (el equipo objetivo es Windows x86 de 32 bits, así que TODO el build es Win32). **Fuente: `mpv-player-windows` (sourceforge), `mpv-dev-i686-20210530-git-fd8c673.7z`** build MinGW i686 de 2021-05-30, ANTERIOR a que mpv dejara de soportar Windows 7: su tabla de imports solo usa DLLs y símbolos que existen en Win7 SP1. Los builds nuevos (media-kit 20241021, shinchiro 2026) importan ESTÁTICAMENTE símbolos W8+ (`AdjustWindowRectExForDpi`, `GetSystemMetricsForDpi` de USER32 W10, `GetDpiForMonitor` de SHCORE, api-ms-win-core-winrt|path|synch) y en Win7 el loader muere al arrancar con *"The procedure entry point AdjustWindowRectExForDpi could not be located in the dynamic link library USER32.dll"*. Igualmente NO usar `shinchiro/mpv-winbuild-cmake` de mayo 2026 en adelante: incrustan un OpenSSL 3 de master con el commit roto `1e1ea715` que aborta con `lock != NULL` al inicializar mpv. Este build MinGW compila ffmpeg+OpenSSL DENTRO del DLL (no trae dependencias aparte), usa `msvcrt` del sistema y no importa vulkan-1.dll:
   - `libmpv-1.dll` → renombrar a `libmpv-2.dll` (el CMake/workflow lo buscan así por compatibilidad con dist.iss y la import lib)
   - cabeceras `include/*.h` → reordenar a `include/mpv/*.h` (layout que espera CMake)
   - La rama `if(WIN32)` del CMake (`MPV_DEV_DIR`) detecta esto.
3. **yt-dlp.exe** en el directorio de salida (junto al `.exe`).
   - TODO este apartado es **SOLO para la máquina de pruebas Windows 7**. En versiones
     superiores (W8/W8.1/W10/W11) entran los **mpv y yt-dlp OFICIALES**: las
     funciones que faltan ahí (AddDllDirectory y el api-set de Windows 8)
     existen sin parches. El setup de Inno gatea por versión (`IsWindows7`):
     el KB solo se toca en Windows 7. El CI genera un único setup progresado
     para la máquina de pruebas.
   - Fuente: **fork `nicolaasjan/yt-dlp`**, asset **`yt-dlp_x86_win7.exe`** del
     último release (`2026.08.28.073407`). Es el build x86 con Python 3.14
     preparado para Windows 7; los builds OFICIALES (Python 3.10+) no cargan en
     la Win7.
   - Su `python314.dll` importa ESTÁTICAMENTE `api-ms-win-core-path-l1-1-0.dll`
     (`PathCchCanonicalize`/`PathCchCanonicalizeEx`…), un api-set que solo existe
     desde Windows 8 → en Win7 el loader falla con *"api-ms-win-core-path-l1-1-0.dll
     is missing from your computer"* al lanzar la app (la primera vez que el
     programa invoca yt-dlp). Arreglo: copiar junto a `yt-dlp.exe` el polyfill
     **x86** del repo `adang1345/api-ms-win-core-path` (releases → v1.0.0 →
     `api-ms-win-core-path.zip`, archivo `x86/api-ms-win-core-path-l1-1-0.dll`;
     implementación basada en Wine, LGPL-2.1, heredada de
     `nalexandru/api-ms-win-core-path-HACK`, sin imports W8+). En Win7 el loader
     de api-sets no reconoce ese nombre y cae a la búsqueda normal de DLLs, así
     que basta dejarlo en la misma carpeta. Lleva acompañadas sus licencias
     (`LICENSE.txt`, `COPYING.LIB`, `LICENSE_WINE.txt`).
   - Además importa `AddDllDirectory`/`GetFileInformationByHandleEx`/
     `GetFinalPathNameByHandle` (las añade la actualización **KB2533623**, que la
     el equipo de pruebas sin updates no tiene). El setup de Inno lleva empaquetado
     `Windows6.1-KB2533623-x86.msu` (repo `beny1226/Windows-7-KB2533623`) y lo
     instala con `wusa /quiet /norestart` cuando, SOLO en Windows 7
     (`IsWindows7`), detecta en caliente que `kernel32` no exporta
     `AddDllDirectory` (GetProcAddress). Antes de instalarla avisa al usuario
     con un cartel que explica que le **falta la actualización necesaria**
     (KB2533623). Se ejecuta AL FINAL de la instalación (`CurStepChanged` →
     `ssPostInstall`), ya con la app completa y los iconos creados, porque la
     update pide reiniciar (wusa devuelve 3010) y un reinicio en mitad de la
     copia dejaría la instalación a medias. Evita el error *"The procedure
     entry point AddDllDirectory could not be located in the dynamic link
     library KERNEL32.dll"* al lanzar yt-dlp en la máquina de pruebas.
4. **windeployqt** para copiar Qt al lado del ejecutable.

## Pasos (en Windows)

```sh
# en la carpeta del proyecto
cmake -B build -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2/msvc2019" -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release

# empaquetar en un solo dir
windeployqt build/nevertube_qt2.exe
# + copiar mpv-2.dll, mpv.lib?? no: solo la DLL + yt-dlp.exe al lado
```

> Detalle: `CMakeLists.txt` ya define `WIN32_EXECUTABLE TRUE` (`:84`); en
> Windows no se abre consola por defecto, así que los selftest E2E pueden no
> imprimir. Si no ves la salida de `--selftest-player`, ejecuta con una consola
> visible (`cmd` con `set WindowMode`…) o redirect a archivo; el rc se refleja
> igualmente.

## Verificación post-build (Windows)

```sh
./nevertube_qt2.exe --selftest-player   # debería salir SELFTEST OK
./nevertube_qt2.exe --selftest-db       # SELFTEST OK
```

El selftest del player verifica el núcleo protegido (hwdec + timer 33ms + render)
**sin reproducir archivos**. En Windows, el render va por `vo=libmpv` en un
`QOpenGLWidget` igual que en Linux; el watchdog de atasco usa micro-seek local
(idéntico al fix de `watch_page`).
