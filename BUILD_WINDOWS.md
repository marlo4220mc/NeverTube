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
2. **mpv dev para Windows** (descarga de `https://mpv.io/installation/` → mpv-dev-i686; el equipo objetivo es Windows x86 de 32 bits, así que TODO el build es Win32):
   - `mpv.lib` / `mpv-2.dll` (libmpv)
   - cabeceras `mpv/client.h` etc.
   - La rama `find_package(unofficial-mpv CONFIG)` del CMake detecta esto.
3. **yt-dlp.exe** en el directorio de salida (junto al `.exe`).
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
