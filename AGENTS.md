# nevertube_qt2

Cliente ligero de YouTube en Qt5 (C++17 / CMake). Núcleo de reproducción
basado en un POC probado. Philosophy: ligero antes que completo, reproducir
antes que decorar, estabilidad y bajo consumo.

## Build

```sh
cmake -B build && cmake --build build
```

Dependencias: Qt5 (Core Gui Widgets Network Sql), OpenGL, libmpv.

### Build x86 de Windows (Win7 de 32 bits, doble motor)

La app tiene **dos motores** (`src/playback_backend.h`): `MpvBackend` (mpv,
Linux y Windows **x64**, pieza protegida intacta) y `FfmpegBackend`
(FFmpeg/libav sin libmpv, el **único** motor del build **x86** de Windows —
`NEVERTUBE_X86_FFMPEG_BUILD`). No existe build público de FFmpeg para Win32
(gyan/BtbN solo win64, MSYS2 retiró i686): el dev-package i686 se cross-compila
una vez y está **vendorizado** en `third_party/ffmpeg-win32/` (LGPL v2.1+,
schannel, DXVA2 h264, imports Win7-limpios, sin libwinpthread/libgcc). Receta
reproducible: `tools/build_ffmpeg_i686.sh`. El CMake resuelve `FFMPEG_DEV_DIR`
por defecto al paquete vendorizado. En x86 la app usa `Qt5 Multimedia`
(`QAudioOutput`) para el audio y el perfil de códec se fija a `h264-hw`
(techo DXVA2) sin `HwProbe` (guards `NEVERTUBE_NO_MPV` en hwprobe/mainwindow/
settings/factory). En x64/Linux todo queda como estaba (mpv). Detalles y
verificación: `third_party/ffmpeg-win32/README.md` y `BUILD_WINDOWS.md`.

El `FfmpegBackend` decide si adjunta el `hw_device_ctx` a un decodificador con
`codecSupportsHw` (`avcodec_get_hw_config`): SOLO si el códec tiene un hwaccel
compilado para ese device (el paquete i686 solo trae `h264_dxva2`). Es
obligatorio no adjuntarlo a AV1: con device adjunto, `ff_get_format` se invoca
y, sin un hwaccel av1, el decoder nativo de FFmpeg 9 aborta el clip entero con
`ENOSYS` ("Your platform doesn't support hardware accelerated AV1 decoding",
av1dec.c). Aviso (regla dura): el decoder NATIVO `av1` de FFmpeg 9 **no
decodifica AV1 por software en ningún caso** (el propio código dice "the av1
decoder doesn't support native decode"; con o sin device). El software AV1 de
FFmpeg lo dan `libdav1d`/`libaom`, que el paquete i686 NO trae → **en x86 AV1
es imposible** (ni HW ni SW); el build x86 fija el perfil `h264-hw` y nunca pide
AV1, así que no afecta a la reproducción. Por eso `--selftest-ffmpeg` salta el
clip av1 cuando no hay decoder sw de AV1 (`av1SoftwareDecodable`): en x86 se
salta; en Linux/x64 con libdav1d se sigue verificando (ahí `hw=no` es
libdav1d en software, no el nativo).

**Fallback a software del open HW**: `codecSupportsHw` solo refleja lo
COMPILADO (un hwaccel existe en el paquete), no lo que el hardware soporta.
`av_hwdevice_ctx_create` (DXVA2 en Windows) suele funcionar aunque la GPU no
decodifique h264 por hardware (el chequeo real ocurre al abrir el
decodificador). Si `avcodec_open2` con device adjunto FALLA — p. ej. una
plataforma sin h264 DXVA2 — `openDecoder` devuelve `nullptr` y `runLoop` lo
reintenta en SOFTWARE PURO antes de reportar "Sin decodificador de vídeo"
(error fatal). Sin ese retry, en esas máquinas cada clip h264 moriría con
error en vez de caer a SW (visto en la máquina de pruebas x86). Con el retry, el primer
frame sale en software → hwdec="no" → la escalera reactiva baja
`h264-hw → soft-all` (efímero) y re-resuelve.

## Límite de literal de cadena de MSVC (C2026): el QSS va partido

**MSVC no admite un literal de cadena de más de 16380 bytes** y avisa con
`error C2026: string too big, trailing characters truncated`. GCC no tiene ese
límite, así que un bloque que en Linux compila limpio puede no compilar en
Windows sin que nada en local lo delate.

El caso real: `nt::styleSheet()` (`src/ui/theme.cpp`) tenía el QSS entero —
30 627 bytes, 624 líneas — en un único literal crudo `R"QSS(…)QSS"`. El
build x86 usa **toolset v142** (VS2019, obligatorio para Win7) y ahí reventaba;
el x64 con el toolset de `windows-latest` lo aceptaba, así que **solo se veía
en el x86**. C2026 lo dispara el lexer al recorrer el literal, de modo que el
arreglo tiene que estar en el literal, no en una compilación condicional.

El QSS vive ahora en `static const char *const kTrozos[]` (cinco trozos, el
mayor de 8359 bytes) que `styleSheet()` une con `QString::fromUtf8`. Reglas:

- **NO volver a levantar el QSS en un solo literal.** Si crece, se añade otro
  trozo al array.
- El reparto cae **siempre entre reglas** (en línea en blanco), nunca dentro
  de una, y el resultado es el mismo qss de antes byte a byte.
- Se une con `fromUtf8` y no con `QStringLiteral` a propósito: así es el runtime
  quien decodifica UTF-8 y no depende del charset con el que MSVC leyó el
  fichero (aunque `/utf-8` sigue puesto en CMakeLists).
- Comprobado que no queda ningún literal de más de 16380 bytes en `src/`, y
  `--selftest-theme` sigue parseando la hoja completa y la valida igual.

Para detectarlo sin esperar al CI, basta con mirar el tamaño de los literales
crudos: cualquier `R"…(` de más de 16 KB es un C2026 en el x86.

## Icono de la app

La app tiene icono en cuatro superficies, todas desde los mismos ficheros:

| superficie | mecanismo |
|---|---|
| `.exe` de Windows | `resources/nt.rc` (`IDI_ICON1 ICON DISCARDABLE "icon/nt.ico"`), añadido con `target_sources` dentro del `if(WIN32)` de CMakeLists; rc.exe resuelve la ruta con el `target_include_directories` a `resources/` |
| Barra de tareas / Explorador | `nt.ico` multirres vía `IconFilename` en los accesos directos de `dist.iss` y `dist64.iss` |
| El propio `setup.exe` | `SetupIconFile` en los dos `.iss` |
| Ventana (Qt) | `app.setWindowIcon()` en `main.cpp` con los 7 PNG del `.qrc` |

- Los ficheros viven en `resources/icon/` y los genera
  `tools/gen_app_icon.py` desde el maestro `nt_icon.png` (1024, alfa real).
  El generador **falla ruidosamente** si el maestro pierde transparencia o si
  al `.ico` le falta alguna entrada, para que un cambio futuro no deje un
  `.ico` de una sola entrada (barra borrosa) sin que nada se entere.
- Las alturas son 16/24/32/48/64/128/256 porque **QIcon no escala**: elige la
  entrada que más se parezca a lo que pide cada superficie. La de 16 es la barra
  de tareas al 100 % y la más exigente: por eso el bisel del monitor es forma
  **rellena** y no un contorno de 1 px (medido a 16 px, un contorno de 1/16
  del lienzo da 0,33 px y desaparece).
- `nt.ico` está en la lista `$need` del gate de manifiesto de iscc del x64: si
  una edición de `[Files]` lo deja fuera, el CI lo dice en vez de crear los
  accesos directos con el icono por defecto de Windows.
- En Linux la ventana no muestra icono propio en el escritorio sin un
  `.desktop` instalado; hoy no hay ninguno porque la app no se instala en Linux.

## Pieza protegida (NO modificar sin razón técnica y prueba)

El reproductor funcional es una pieza protegida. Los siguientes elementos NO
se deben tocar sin una razón técnica respaldada por una prueba concreta:

- `QOpenGLWidget` (clase `PlayerWidget`) como superficie de video
- **Redibujado dirigido por eventos**: el motor notifica `frameReady` cuando
  hay un frame nuevo (update callback de mpv instalado en
  `MpvBackend::initGL` / `frameReady` por frame del FfmpegBackend) y
  `PlayerWidget::onFrameReady` fuerza `QWidget::update()`. NO hay timer de
  poll: en reposo (pausa, buffering, cubierto) no se repinta nada. Fue
  sustituido por el antiguo `QTimer` de 33 ms, que quedó redundante al
  existir la ruta de eventos (verificado: reproducción real + overlay
  presente/auto-ocultado con capturas de pantalla, selftest-player).
- `vo=libmpv` + `hwdec=auto` (funciona con y sin aceleración de hardware)
- Renderizado directo a FBO (`mpv_render_context_render`) en `paintGL`
- Cualquier cambio a la integración mpv (libmpv / mpv_render_context)

Contexto: el primer intento de separar los controles multimedia (overlay
flotante sobre el video) rompió la app y fue revertido. Hacer cambios de UI
que toquen el reproductor por incrementos pequeños y verificando cada uno.

## Layout de rejilla (vídeos) y "Tamaño de las miniaturas"

La rejilla del Home mantiene un **número de columnas FIJO** en cualquier ancho
de ventana (5 por defecto; 4 o 6 según el ajuste). Cada celda se dimensiona
según su contenido real (`VideoListView::applyGridGeometry`): `cellH =
miniatura 16:9 + bloque de texto fijo` (hueco 8 + título 34 + línea de canal).
NO usar un ratio fijo (`cellW * 258/320`) para la altura: al estrechar la
ventana el texto no escala y un vídeo tapa el nombre del canal del siguiente.
Ningún vídeo debe solaparse sobre otro, ni en vertical ni en horizontal.

El ajuste **"Tamaño de las miniaturas"** (Ajustes → Interfaz, clave
`thumb_size` = `small|normal|large`) sustituye al antiguo "Densidad de
cuadrícula" (`grid_density`), que se MIGRA al leerlo (`compact`→`small`,
`cozy`→`normal`; la clave vieja se queda inerte en la BD). Se traduce a DOS
efectos porque rejillas y listas no se dimensionan igual
(`MainWindow::applyThumbSize`, el único punto que lo aplica, tanto al arrancar
como en caliente):

| valor | Home (columnas) | Listas (escala de fila) |
|---|---|---|
| `small` "Pequeñas" | 6 | 0.88 (fila 76 px) |
| `normal` "Normales" | 5 (defecto) | 1.0 (fila 86 px) |
| `large` "Grandes" | 4 | 1.15 (fila 99 px) |

- **En las listas** (búsqueda, canal, historial, playlists y la cola "A
  continuación", todas `ListMode`) el factor `setDensity` sí se ve: la fila es
  `kListH * d` y la miniatura `136d × 76d`.
- **En las rejillas el factor NO hace nada visible** (por eso el ajuste antes
  no se notaba en el Home): la celda la dimensiona el ANCHO con las columnas
  fijas, así que a 903 px mide 175 px con densidad 1.0 y 175 px con 0.88. La
  única palanca real es el número de columnas (`HomePage::setCellColumns` →
  `setGridColumns` → `applyGridSizes`, que también recalcula la altura de cada
  sección con el tamaño de celda nuevo).
- **Tope por ancho**: `VideoListView::clampGridColumns` recorta las columnas
  pedidas para que la celda no baje de `kMinCellW` = 130 px (por debajo, el
  título de 2 líneas y la línea de canal se recortan). A 903 px caben 6
  columnas (celda 147 px), a 700 px solo 5 y nunca baja de 3. Se aplica en
  `HomePage::effectiveCellColumns` desde `applyGridSizes`, o sea también en
  cada `resizeEvent`. Función pura testeada en `--selftest-grid` (12 casos de
  columnas + 10 de texto + la monotonía de 100 a 900 px).

- **El texto de la celda se deriva de la celda** (ya no es fijo):
  `VideoListView::gridTitlePx(cellW)` da **13 px con celda de 140 px hasta 18 px
  con celda de 340 px** (lineal, con topes en los dos extremos), y
  `gridTitleLineH` = 1,28 × tamaño. Antes eran 13 px y una caja de 34 px fijos
  mientras la miniatura sí crecía, que en un monitor grande dejaba el texto
  pequeño y descoyente. Con celda de 177 px (5 columnas a 903 px de viewport)
  salen 13 px, y con 340 px o más 16 px (tope). El título va en **negrita**: se
  probó sin ella y el usuario pidió volver a ella. Lo que no le gustaba era el
  TAMAÑO, no el grosor (con la letra ya 1 px más pequeña la negrita solo ayuda a
  distinguir el título de la línea de canal). La **línea de meta** (canal ·
  visitas · fecha) va **2 px por debajo del tamaño del título** (suelo 10 px):
  el usuario la pidió más discreta que el título.
- **`kGridTitleLines` = 2 líneas, con la fuente 1 px más pequeña y sin
  negrita.** Son las tres cosas que pidió el usuario al ver 14 px en negrita a 3
  líneas: la celda queda bastante más compacta (175 px de alto en vez de 195 con
  celda de 177) a cambio de un poco de legibilidad. Medido con las métricas
  REALES de Qt (`QFontMetricsF`, misma fuente y mismo reparto voraz por palabras
  que `drawGridTitle`) sobre los 90 títulos de la caché y la caja de 161 px que
  sale a 5 columnas en una ventana de 941 px:

  | config | título entero | texto visible | de ellos, ≥5 líneas |
  |---|---|---|---|
  | 14 px negrita, 3 líneas | 13% | 72% | 51/90 |
  | **13 px negrita, 2 líneas (actual)** | 3% | 55% | 37/90 |
  | 13 px normal, 2 líneas | 5% | 58% | 22/90 |
  | 12 px normal, 2 líneas | 8% | 62% | 9/90 |

  La negrita cuesta algo de legibilidad (3% enteros frente al 5% sin negrita) y
  es el precio que el usuario eligió: la celda compacta le importa más que leer
  el título entero. La celda mide `thumbH + 6 + 8 + gridTextBlockH(cellW)` y el
  bloque `2*lineH + 14 + 20` (64 px a 12 px, o sea celda de 140); NO cambia el
  número de vídeos por fila. Si algún día se queja de que se cortan títulos, el
  primer experimento es **12 px** (medido: 8% enteros y 62% visible, MEJOR que
  13 px) y el segundo volver a 3 líneas.
  **Geometría y pintura comparten las mismas funciones puras**
  (`gridTitlePx`/`gridTitleLineH`/`kGridTitleLines`), así que la celda reserva
  exactamente lo que el título ocupa y no puede solaparse con la línea de canal.
  El reparto en N líneas es voraz por palabras y en la ÚLTIMA línea entra todo
  lo que queda desde el principio de esa línea, elidado: se ve el arranque del
  resto en vez de un corte en seco. El modo **lista** no se toca (sigue a 2
  líneas con el interlineado natural de la fuente, porque su altura de fila es
  fija y no deriva de la celda).
- **La celda tiene que medir lo mismo en los TRES sitios que la usan**: el
  `sizeHint` que lleva cada item del modelo, `gridSize()` (de donde la página
  saca la altura de la sección) y el rect que acaba pintando QListView. En
  `IconMode` **manda el `sizeHint` del MODELO por encima de `gridSize()`**, así
  que cuando divergen las filas ocupan una cosa y la sección se dimensiona con
  otra, y **la última fila se sale de la vista** (con ella la línea de canal):
  era un bug PREEXISTENTE con el `sizeHint` clavado en la constante `kGridH`
  (258) mientras `applyGridGeometry` calculaba otro `cellH` (195 a celda de
  177), o sea filas de 258 dentro de secciones de `rows*195`. La única fuente es
  `applyGridGeometry`, que escribe además el `sizeHint` de todos los items
  (`m_gridCell`, y `gridItemSize()` para los items nuevos); por eso se llama
  DESPUÉS de crearlos. La altura de sección sigue siendo `rows * cellH` porque
  en `IconMode` las filas van clavadas una detrás de otra: `setSpacing(4)` **no**
  suma nada (medido: celda 177x195 -> rect 177x195 y paso 195) y el hueco visual
  entre miniaturas es el margen interno de 6 px que pinta el delegate. Lo
  comprueba `--selftest-grid` con un `VideoListView` de verdad (19 vídeos, 5
  columnas): `sizeHint` == `gridSize` == rect real, la última fila cabe en
  `rows*cellH` y las filas no se solapan.
- OJO: el texto ya escala, pero el ajuste de la **columna** sigue siendo
  decision del usuario (4/5/6). Con celdas muy estrechas el título se cortará
  igual: ahí el que decide cuántas líneas caben es el ajuste, no la fuente.

## Fila de vídeo en listas: UNA forma en todas las páginas

La fila de `VideoListView` en `ListMode` (Búsqueda, Canal, Playlist,
Historial/"Ver todo", "A continuación" y las secciones de Biblioteca) es la
MISMA forma en todas partes: **miniatura + título + "canal • visitas • fecha" +
botón de opciones ⋮**. Lo único que cambia entre páginas es el TAMAÑO de la
miniatura y de la fila (Búsqueda/Canal/Playlist/cola a escala 1.0, Historial a
1.21, Biblioteca con su miniatura de 236x133): el usuario pidió "mismo estilo,
no mismo tamaño de letra o negrita", así que la tipografía es la que ya tenía
cada superficie (título en negrita con la fuente de la app y meta reducida en
las listas; 14/600 y 12 px en Biblioteca). NO unificar tamaños ni pesos.

- **`format::metaLine(v)`** (`src/ui/format.*`) es la única función que compone
  la línea de meta. La usan el delegate (rejilla y lista) y las filas de
  Biblioteca. Antes estaba triplicada (dos copias en el delegate y una en
  Biblioteca) y la fila de lista pintaba solo "canal • visitas": el mismo vídeo
  se leía distinto según la página.
- **El bloque de texto va CENTRADO en la fila**, y con la altura REAL del
  título (1 o 2 líneas): `wrapTitle` (la misma función que pinta
  `drawGridTitle`) da las líneas que ocupa y `paintList` centra con esa altura,
  así que la meta queda pegada a la última línea del título. Antes se
  reservaban 2 líneas siempre y con un título de una línea la meta caía un
  renglón entero por debajo (medido: hueco ~31 px; ahora ~12). Las medidas del
  bloque están en `VideosDelegate::listMetrics`, y la caja del título y la
  línea base de la meta salen de ahí.
- **Nada de texto debajo de los botones**: `textWidth(r, ...)` descuenta el
  hueco real de la X y/o el ⋮, con la misma cuenta que `removeButtonRect`/
  `moreButtonRect`. Medido: sin descontar el ⋮ el título llegaba a x=839 con los
  puntos del botón en 838-841.
- **El ⋮ va pegado al borde derecho cuando no hay X** (como el botón de
  Biblioteca) y a la izquierda de la X cuando la hay. Antes reservaba siempre el
  hueco de la X aunque no se pintara, y le robaba 34 px de texto a la fila (se
  nota sobre todo en la columna estrecha de "A continuación").
- Se borró el modo de meta de 2 bloques (`setMetaLines`/`paintRichMeta`/avatar
  de canal): no lo llamaba nadie y pintaba una fila distinta, justo la
  divergencia que se estaba quitando.

### Menú ⋮: uno solo para todas las superficies

`nt::addRowMenuActions` (`src/ui/row_menu.*`) añade las acciones estándar con
los mismos textos y en el mismo orden: **Ver más tarde** (toggle; etiqueta
"Guardar" o "Quitar de Ver más tarde" según el estado), **Añadir a playlist**
(con "+ Nueva playlist" si la superficie pasa el callback) y **Compartir** y
**Ver canal**. Cada superficie antepone su acción propia: Historial "Quitar del
historial", "A continuación" "Quitar de la cola". Las acciones que escriben en
la BD se ejecutan con `QTimer::singleShot(0)`: en el `triggered()` la lista de
la que nació el menú puede quedarse sin la fila con el popup abierto.

- Biblioteca, Historial y "A continuación" construyen el menú en la propia
  página (tienen los repos y, la cola, el `PlayerController`).
- Búsqueda, Canal y Playlist **no** tienen los repos: emiten
  `rowMenuRequested(video, pos)` y `MainWindow::showVideoRowMenu` monta el menú
  UNA vez (mismo helper) con los suyos. "Nueva playlist" sale por el diálogo
  compartido `nt::inputText`, al que entra por
  `LibraryPage::createPlaylistForVideo(videoId)` (el nombre ya no dice "popup"
  porque es el overlay de la app, no un popover de la página).
- "Quitar de la cola" usa `PlayerController::removeFromQueue(index)`, que NO
  admite el índice en reproducción (dejaría la cola apuntando al siguiente con
  el estado del que se fue) y reindexa las posiciones de reanudación
  (`m_indexStartMs`) al desplazarse.

## Página Canal: ficha en caja y pestañas propias

Lo que gobierna la página:

- **La ficha va dentro de una caja** (`QFrame#channelCard`, #181818 + borde
  #262626 + radio 12, como `#settingsCard` y `#watchDescCard`). Antes avatar,
  título, suscriptores y "Suscribirse" iban sueltos sobre el fondo de la
  página, y la descripción en una línea gris propia debajo. Ahora el avatar (80
  px, circular), el nombre (`#channelName` 19px/700), la línea de
  suscriptores (`#channelMeta` 12px `kMetaText`) y las dos acciones van en una
  fila, y la descripción (`#channelDesc`, mismo color/peso que `#watchMeta`)
  cierra la caja. El nombre es 19px porque es el H1 al que se entra; el resto de
  superficies conservan su tipografía (aquí NO se unifica nada con Buscar,
  Biblioteca o Player, solo la forma).
- **Suscriptores con `compactCount`** (`format.h`), no el `long long` en crudo:
  antes ponía tal cual "1234567 suscriptores".
- **El avatar va circular de verdad**: `nt::circularAvatar` (`src/ui/icons.*`)
  escala, recorta el centro y enmascara con un círculo. El `border-radius` del
  QSS solo redondea el fondo de un QLabel, y el `setScaledContents` deformaba
  cualquier avatar no cuadrado. Esta función sustituye a las dos copias idénticas
  que tenía `channel_row.cpp` y `watch_page.cpp` (la del Player además estiraba
  el pixmap).
- **La descripción se recorta por LÍNEAS, no por píxeles**: 3 (`kDescLines`),
  igual que el Player. El tope anterior eran 165 px, que dependen del
  interlineado de la fuente. El botón "…más"/"…menos" es `#descToggle` (enlace
  discreto, no cápsula) y el degradado `#descFade` (26 px arriba del borde
  inferior de la caja, `positionDescFade`, el mismo floats-out-of-layout que en
  el Player).
- **Las pestañas ya no son un `QTabWidget`**: eran lo ÚNICO de la app sin estilo
  (no hay regla `QTabWidget`/`QTabBar` en el QSS) y el "pane" pintaba una
  columna casi negra `#020303` bajo la lista, más oscura que el fondo de la
  página. Ahora son tres `QPushButton#tabBtn` dentro de una cápsula
  `#channelTabs`; la seleccionada lleva el acento en el texto (como la nav) y
  todas anulan el `:focus`. El contenido son tres widgets que `showTab()`
  muestra a mano (no un `QStackedWidget`, que volvería a traer el fondo del tema
  del escritorio). `#tabBtn` y su `:focus` los vigila `--selftest-theme`.
- **Contador de vídeos** de la pestaña activa como pastilla `#tabCount`
  (misma forma que `#queueCount`), oculto con 0 vídeos porque entonces ya sale
  el estado vacío.
- **Estado vacío por pestaña** = `#emptyState` (la misma caja punteada de la
  Biblioteca) con el texto de esa pestaña, y la lista se oculta a la vez. Con
  **alto FIJO de 96 px**, y NO con stretch: medido en un QVBoxLayout de 700 px,
  una etiqueta con stretch 0 cuyo único hermano con stretch está oculto se
  estira a los 700 px (una caja punteada gigante); con alto fijo queda en una
  banda de 96 px centrada verticalmente y de ancho completo (medido: y=543..638
  de un área de 258..920, x=17..923).
- **"Cargar más vídeos" es `#ghost` centrado** (alto 34). Era `#primary` con
  `setFixedWidth(180)`, una barra roja ancha bajo la lista; cargar más no es la
  acción principal de la página. Solo visible si la respuesta vino llena (25).
- **Añadido botón "Compartir"** (chip `#actionChip` con `nt::Icon::Share`): copia
  `https://www.youtube.com/channel/<id>`. Al re-tintar el acento hay que rehacer
  el icono a mano (`applyAccent`): los `QIcon` llevan el color embebido.

## Página Player: scroll único (vídeo + descripción + A continuación)

La página Player scrollea como YouTube: vídeo, descripción y "A continuación"
viven DENTRO de un único `QScrollArea` (`WatchPage::m_topScroll`) y el scroll
mueve todo junto — al bajar, el vídeo se va de la pantalla y descripción/cola
ganan espacio. Dentro del scroll, la descripción va a la izquierda y "A
continuación" a la DERECHA (sin límite vertical: crece con todas sus filas y
el scroll general la recorre). Ambos bloques se alinean ARRIBA y conservan su
altura natural (`midLay->setAlignment(..., Qt::AlignTop)`): la fila crece con
la cola, pero la descripción no se estira a esa altura — debajo de cada
bloque solo queda espacio vacío scrollable, no un widget estirado. El botón
"Volver" del Player está en una **barra fija ARRIBA del scroll**
(`m_backBar`, `splitOuter->insertWidget(0, …)`), o sea NUNCA encima de la
imagen, y se oculta en fullscreen (se oculta la barra, no solo el botón: en
fullscreen el scroll ocupa toda la página y la barra dejaría una franja).

- **"Volver" es el MISMO token en las cuatro superficies** con vuelta atrás
  (Canal, Historial, Playlist y el Player): `QPushButton#backLink`, que es un
  ENLACE (sin marco, `padding: 4px 8px`, 13 px/600, `@MUTED@` → `@TEXT@` al
  hover) y no una cápsula. Antes era `#ghost` (con borde) en las tres páginas
  y un botón flotante sobre el vídeo en el Player. Es el mismo criterio que
  `#sectionLink` de Biblioteca: "volver" es navegación, no un control de la
  página. El icono es `nt::icon(nt::Icon::Back, nt::kMuted, 13)` en las cuatro
  (dibujado con QPainter, nunca el glifo `◀`). Lo que sí cambia entre
  superficies es solo DÓNDE va el enlace: en las tres páginas dentro de su
  propio layout de raíz, y en el Player en la barra de arriba del scroll
  (medido: texto en `x=27..89`, el vídeo arranca en `y=38`; en las páginas el
  mismo texto cae en `y=23..32` por el margen de raíz de 16 px).

- `PlayerContainer::updateMaxHeight()` fija `min = max = min(width/aspect,
  kMaxPlayerHeight=560)` (no solo el máximo, como el split antiguo): dentro
  del scroll el layout debe darle al vídeo EXACTAMENTE su altura, ni estirado
  por políticas Expanding ni encogido por el contenido scrolleable. El tope
  absoluto **NO aplica en pantalla completa** (`setFullscreen(true)` →
  `capped = width/aspect`): si se re-aplica el 560 en fullscreen el vídeo
  quedaría en 560 px con la pantalla llena de negro (regresión vista y
  revertida en el layout split anterior). El tope sí aplica en ventana normal
  y maximizada.
- La cola "A continuación" va a la DERECHA de la descripción, SIN límite
  vertical: en `WatchPage::updateQueue()` se fija su altura al contenido real
  (`sizeHintForRow(0)` × nº de filas) para que el scroll general de la página
  la recorra entera, sin scroll interno duplicado.
- Fullscreen (solo vídeo): se ocultan botón Volver + descripción + cola
  (`m_backBar`, `m_watchBody`, `m_bottomWidget`); con `widgetResizable` el
  contenido queda = solo vídeo y se estira a toda la página (sin reparent del
  player). Al salir se restauran y el scroll vuelve a 0.
- Al cambiar de vídeo (`onCurrentChanged`) el scroll vuelve arriba (como
  YouTube); no en fullscreen (ahí está oculto).
- Verificación visual: QOpenGLWidget dentro de QScrollArea funciona en Qt
  5.15, pero hay que comprobar en la máquina real que no haya parpadeo/
  artefactos al scrollear y que el "salto al volver" del vídeo al volver
  arriba tras estar fuera del viewport sea aceptable (mpv acumula frames
  mientras no pinta).

## Pruebas de regresión

```sh
./build/nevertube_qt2 --selftest-db        # repos/SQLite
./build/nevertube_qt2 --selftest-player    # hwdec=auto + redibujado por eventos + render ok
./build/nevertube_qt2 --selftest-innertube # sw.js_data→visitorData→player→hlsManifestUrl
./build/nevertube_qt2 --selftest-discover  # radios guardadas: guardar/recuperar/re-seleccionar/persistencia/descarte
./build/nevertube_qt2 --selftest-av1hls    # HLS sintético AV1 (offline): MAP/BYTERANGE, /seg.mp4, &nt único
./build/nevertube_qt2 --selftest-hwprobe   # sondeo HW offline (micro-clips): av1-hw|vp9-hw|h264-hw|soft-all
./build/nevertube_qt2 --selftest-ssl       # backend TLS (OpenSSL 1.1 junto al exe) cargado
./build/nevertube_qt2 --selftest-tls       # handshake TLS REAL contra hosts públicos (no solo carga)
./build/nevertube_qt2 --selftest-search    # búsqueda Innertube REAL: vídeos con id no vacío
./build/nevertube_qt2 --selftest-browse    # endpoints Innertube: canal + tabs (videos/shorts/live) + detalle + playlist
./build/nevertube_qt2 --selftest-feeds     # canal por Innertube + trending (protocolo y cadena)
./build/nevertube_qt2 --selftest-grid      # rejillas: tope de columnas (puro) + celda coherente modelo/gridSize/seccion (monta un VideoListView; sin red ni GPU: va en el CI)
./build/nevertube_qt2 --selftest-theme     # QSS puro: selector duplicado, selector sin widget, paleta de selección y bordes de los botones: va en el CI
./build/nevertube_qt2 --selftest-glyphs    # todo glifo no ASCII de la UI existe en la fuente real de la app: va en el CI
```

El selftest del player verifica invariantes del núcleo protegido sin
reproducir ningún archivo. Ejecutar ambos antes de considerar estable un
cambio que toque el reproductor.

**Qué dispara cada workflow (importante para interpretar un tablero en rojo):**

| workflow | se dispara |
|---|---|
| `windows.yml` (x86 / Win7) | en **cada** push a `main` |
| `windows_x64.yml` (Win10+) | solo si el push toca **el propio fichero** del workflow (filtro `paths:`), o a mano con `gh workflow run windows_x64.yml --ref main` |

O sea que un `main` en rojo **solo por x86** es normal, y un `main` donde solo
corre el x64 significa que ese push tocó `windows_x64.yml`. Para validar un
cambio en el build x64 hay que dispararlo a mano o tocar su workflow.

Ojo también con el toolset: el x86 usa **v142** (VS2019) por el target Win7 y el
x64 el de `windows-latest` (más nuevo). No compilan lo mismo: un literal que el
x64 acepta puede dar C2026 en el x86 (ver la sección del límite de MSVC). Un
fallo que aparece solo en x86 no hay que buscarlo solo ahí.

## Recuperación ante sesión gateada (403 de YouTube)

YouTube puede "gatear" una sesión de URLs (audio `>403` y segmentos HLS `>403`
desde un punto concreto): mpv abre el vídeo mudo (`audio=eof`, sin `AO:`/sink)
y los segmentos a partir de ~1:00 fallan → EOF prematuro a la ~1:03 con
`Cache: 0.0s` y error, sin sonido durante todo el vídeo. Intermitente (~1/10
sesiones). La app ahora se recupera sola re-resolviendo con URLs frescas:

- `PlayerWidget::streamInterrupted()` se emite en dos casos: **EOF prematuro**
  (EOF no-natural con `duration > 0`, que no es final de vídeo ni live) y
  **sin pista de audio activa** (~10 s reproduciendo con `audio-params` de mpv
  ausente y `position > 2s`; `kAudioDetectMs`). El watchdog de audio NO depende
  del modo: con el gate también en HLS muxed mpv queda mudo (`audio=eof`), así
  que se dispara a los ~10 s (antes solo vigilaba el audio EXTERNO y el HLS
  agotaba ~60 s mudos por intento hasta el EOF). NO toca el render ni el
  redibujado: es lógica de eventos y del watchdog de `checkStall` (audio).
- `PlayerController::onStreamInterrupted()` re-resuelve (como el atasco):
  `reloadCurrent()` con `m_recoverAttempts` (≤3, recaída <8s corta y estabilidad
  >5 min reinicia; NO 60 s: el gate muere a los ~60 s y con 60 s el reinicio
  borraba el presupuesto justo antes de cada muerte → reload → 60 s → reload…
  bucle sin fin). Con el watchdog a ~10 s cada intento cuesta ~10-16 s
  (resolución + espera): 3 intentos ≈ 40-50 s antes del último recurso. En
  `soft-all`/HW el coste es un tramo de "loading" acotado; en funcionamiento
  normal ningún código nuevo corre por frame.
- **Rotación de sesión en la recuperación**: el gate es de SESIÓN, no de URL —
  re-resolver con `forceRefresh` pide un pot nuevo DENTRO de la misma sesión
  gateada y vuelve a fallar. Por eso `onStreamInterrupted()` y la recaída de
  `onStreamStalled()` llaman `Yt::rotateStreamSession()` (→
  `Innertube::rotateSession()`: descarta visitorData vigente + URLs firmadas
  cacheadas) ANTES de `reloadCurrent()`: la re-resolución sale de la sesión
  gateada con un visitorData nuevo (~9/10 de no estar gateada). Sigue acotado
  por el presupuesto: si la sesión nueva también muere, en ≤3 intentos se
  agota. Solo rota en la ruta de fallo.
- **Último recurso: reinicio de la app con el último vídeo.** Al AGOTAR la
  recuperación (`recoveryExhausted()` de `PlayerController`, tanto en el
  `streamInterrupted` como en la recaída del atasco) `MainWindow::onRecoveryExhausted`
  reinicia la app con el último vídeo: reiniciar SÍ sale del gate (estado del
  proceso nuevo — verificado por el usuario: al reiniciar se arregla el audio).
  Es el ÚNICO auto-reinicio de la app y SOLO en este error: relanza con la URL
  del último vídeo únicamente (la cola NO se restaura) y reanuda la posición.
  Guards anti-bucle: (1) en memoria `m_gateRestarting` — un mismo episodio no
  relanza dos veces (PlayerController puede reemitir en una cascada de EOF);
  (2) en settings `gate_restart_armed` — si la instancia YA vino de un
  reinicio y sigue gateada, se muestra el error y se desarma el guard (nunca
  se encadenan reinicios). El guard se desarma solo con
  `PlayerController::recoveryHealthy()` (≥5 min de reproducción estable, emit
  desde el `m_flushTimer`): tras un reinicio que funcionó, la próxima sesión
  gateada podrá volver a reiniciar. Claves: `gate_restart_vid`,
  `gate_restart_pos` (ms), `gate_restart_armed` (1/0). La instancia nueva
  arranca con `http://...` → `MainWindow::playVideo` (leído en el constructor).
- `hasActiveAudio()` consulta `audio-params` de mpv (`MPV_FORMAT_NODE`). Con
  audio sano la pista está activa y la detección no se dispara (verificado:
  tanda normal con `AO: [pipewire]` sin falsos `streamInterrupted`).
- Sin sonido + vídeo que "se va" a ~1:00 = síntoma de sesión gateada, no del
  sink de audio ni de la pieza protegida. Verificar con el log `E2E`
  (`NEVERTUBE_DEBUG=1`): `audio=eof`, `HTTP error 403` en segmentos, y
  `E2E streamInterrupted -> reloadCurrent`.

## Logging de depuración

El logging `E2E ...` está activado por variable de entorno (no imprime por
defecto, evitando ruido en el bucle de eventos del reproductor):

```sh
NEVERTUBE_DEBUG=1 ./build/nevertube_qt2
```

En Windows, `NEVERTUBE_DEBUG=1` también hace que un lanzamiento con doble clic
abra una consola propia para ese log; sin la variable, la app (subsistema GUI)
no abre ninguna ventana de terminal. Lanzada desde una shell, siempre hereda la
consola del proceso padre.

Usar `NT_DEBUG(...)` (src/debug.h) para trazas de depuración.

## Modo de "A continuación" (siembra de la cola)

El modo de siembra se elige con `NEVERTUBE_NEXT_MODE`:

- `radio` (defecto) → mix `RDAMVM<videoId>` del vídeo clicado (`/next` con
  `playlistId`, estilo YouTube Music); si el vídeo no trae panel de
  reproducción, se usa la mezcla de recomendaciones del mismo `/next`.
- `suggestions` → `/next` estático: ~20 recomendaciones del vídeo actual.
- `none` → sin siembra (la cola queda como vino de la entrada).

`radio` NO auto-siembra: la cola del mix se genera solo con un click explícito
desde fuera de "A continuación" (`MainWindow::openWatch` → `WatchPage::triggerRadio`),
es decir al abrir un vídeo desde search/home/channel/URL. Dentro de la lista
"A continuación", cliqueando sus ítems solo se reproduce (no se regenera radio).
`suggestions` sí auto-llena en `WatchPage::updateQueue` cuando quedan ≤5 ítems y
cada vídeo nuevo se regenera. Innertube: `fetchRadio` (panel + mezcla),
`fetchRadioMore` (continuación, si el cliente la emite). El selftest de
innertube cubre el panel RDAMVM y el fallback.

**Radio ya guardada (reuso sin red)**: los radios se guardan COMPLETOS en
`discover_radios`/`discover_radio_videos` al generarse. Si al abrir un vídeo
(`openWatchAt`, modo Radio) su radio ya está guardada, se reusa desde la BD
para "A continuación" (`MainWindow` con `DiscoverRepo::radioVideoIdsForSeed` +
`VideoCacheRepo::getMany`) y NO se vuelve a pedir a YouTube. Solo se llama
`WatchPage::triggerRadio` cuando no hay radio guardada (o sus metadatos no
están en la caché). Para Descubre, `hasRadio` ya evitaba re-pedir.

**Visitas y fecha de la cola (metadatos)**: el `compactVideoRenderer` del
cliente VISIONOS (la mezcla de recomendaciones del `/next`) trae las visitas
como texto localizado (`"85.805 visualizaciones"`, `"1,2 M de vistas"`) y NO
trae fecha. Por eso:

- Las visitas se leen con `countFromText` (el parser propio que había en
  `parseRelated` hacía `toLongLong` del texto entero y daba 0: la cola salía sin
  visitas). Lo mismo en `parseWatchQueue` (panel), que hacía `toInt()` directo.
- La fecha del lockup moderno viene ABREVIADA (`"hace 8 h"`, `"hace 3 d"`,
  `"hace 2 m"`): `relativeToIso` acepta esas unidades (y las inglesas
  `"2 months ago"`). Ojo, `m` = meses (medido: vídeos de millones de visitas no
  pueden ser de hace 2 minutos).
- El lockup da las visitas SIN la palabra "vistas" (`"21 K"`): `looksLikeCountText`
  las reconoce solo si el texto es número + multiplicador (así un canal que
  empieza por dígito, "3 Doors Down", no se confunde con un recuento). El lockup
  también da el CANAL (primer dato que no es visitas ni fecha) y su id (en
  `metadata.image.decoratedAvatarViewModel...browseEndpoint`).
- Para que la cola tenga fecha, `handleRadio` no emite la mezcla de VISIONOS
  directamente: pide UNA vez el mismo `/next` al cliente WEB
  (`requestRadioWeb`), cuyo layout moderno (`lockupViewModel`) sí trae visitas y
  fecha. Si esa petición falla, se emite la de VISIONOS (sin fecha). Es una
  petición extra, no la cascada completa (~12-15 s).
- **Refresco de radios viejas**: una radio guardada por un parser que no traía
  ni visitas ni fecha se detecta al abrirla (`openWatchAt`: canal + algún
  metadato) y se refresca UNA vez; `onRadioReadyDiscover` reescribe entonces su
  lista de ids (`clearForSeed` + `saveRadio`, porque `saveRadio` no sobrescribe).
  Así converge en una sola apertura y luego reusa sin red. Los vídeos que
  YouTube devuelve sin fecha (directos, estrenos) no fuerzan refrescos: el
  umbral es "algún metadato", no "todos".

## AV1 por HLS (sintético)

YouTube **no publica AV1 en su master HLS** (`hlsManifestUrl`): solo VP9/h264
(verificado: todo `av01` de YouTube es `https`/DASH, no aparece en el master HLS). Además, la URL
adaptativa AV1 directa (modo "edge") se **estrangula** (~150 KB/s) porque el
throttle es **por petición** y una descarga continua agota su presupuesto. En
cambio, cada petición `Range` corta (~2 MB) no está estrangulada, incluso por
la misma conexión keep-alive (≈14 MB/s).

`Yt::buildAv1Hls` (perfil `Modern`) sintetiza un HLS local sobre el
`adaptiveFormat` AV1: media playlist fMP4 con `#EXT-X-MAP` (de `initRange`) y
un segmento `#EXT-X-BYTERANGE` **por fragmento real del fMP4**. Claves para
que funcione:

- **la frontera de cada segmento sale del `sidx`** (caja del `indexRange`, ~4
  KB): `buildAv1Hls` baja esos bytes con `fetchRangeBlocking` y
  `av1ParseSidx` da tamaño+duración exactos por subsegmento. Es
  imprescindible: con trozos fijos (el respaldo antiguo de ~2 MB) la
  reproducción secuencial funciona, pero **el seek cae a mitad de fragmento y
  mpv no carga vídeo** (`Cache: 0.0s` + `Errors when loading file`, sin
  `VO:`). Verificar con `mpv --no-config --vo=null --ao=null --frames=2
  --start=<t> <m3u8>` para varios `t`;
- sufijo de ruta `/seg.mp4` tras `/videoplayback` (YouTube lo ignora) para que
  el demuxer HLS acepte la extensión sin `extension_picky`/whitelist;
- `&nt=<n>` distinto por segmento: sin él ffmpeg **fusiona** los rangos
  contiguos del mismo recurso en una sola petición y vuelve el throttle;
- la escritura del `.m3u8` es atómica (`.tmp` + `rename`): la recuperación
  reescribe la misma ruta mientras mpv puede estar leyéndola;
- audio aparte (`PlayerWidget::setAudioFile`, como el modo edge).

Se sirve desde `HlsServer` (`http://127.0.0.1:<port>/<videoId>.m3u8`) y gana en
`hlsReady` antes de `fetchMaster`; si no hay AV1 (u otro perfil), se mantiene el
HLS de YouTube (VP9/h264). Verificado en logs de mpv: `● Video (av1 …)`,
`Using hardware decoding (vulkan-copy)`, `paused-for-cache=0` y `cache-time`
= duración completa en pocos segundos. El selftest offline `--selftest-av1hls`
valida la playlist generada (`EXT-X-MAP`/`BYTERANGE`, sufijo `/seg.mp4`, `&nt`
distinto por segmento, selección de altura y el parseo del `sidx`) sin red ni
reproductor.

## Perfil de códec por hardware (HwProbe)

El perfil que decide qué códec se pide a YouTube (`settings.codec_profile`)
**no** se adivina durante la reproducción: se sondea **al arranque** con
`HwProbe` (`src/hwprobe.*`), que reproduce en headless (`vo=null`,
`hwdec=auto-copy`, sin audio ni config) tres micro-clips embebidos en
`resources/clips/` (AV1, VP9, H.264) en ese orden. Perfiles: `av1-hw` >
`vp9-hw` > `h264-hw` > `soft-all`. `MainWindow::applyCodecProfile` traduce el
perfil a `Yt::CodecProfile` (Modern/Vp9/Legacy), espera inicial de buffer y, en
`soft-all`, `m_noHw` + tope 1080p. Mientras se sondea no se reproduce: la
petición se encola (`m_hasPendingWatch`) y se lanza al terminar. El cartel
"Detectando hardware" solo aparece si el sondeo tarda (>300 ms, PC básico) o
si el usuario intenta reproducir mientras sondea.

**Veredicto por clip (el log de mpv manda; `hwdec-current` solo corrobora):**
con `msg-level=all=no,vd=v,ffmpeg=v` se capturan la línea del decodificador
(`Using hardware/software decoding`), los errores del intento HW (`av1_qsv:
unsupported`, `Error initializing…` → `sawHwFailed`) y los decodificadores
software. **Gracia de 400 ms tras el primer frame** (`video-params/h > 0`):
el veredicto del decodificador se encola 1–10 ms DESPUÉS del primer frame y
decidir antes era el origen del falso positivo `av1-hw` en i5/HD4600 (QSV
reporta `qsv-copy` pero no decodifica AV1; los errores llegaban 5 ms después
del veredicto). `hwprobe::clipVerdict` (función pura, testeada con 9 casos
offline en `--selftest-hwprobe`): `HW` solo con log de HW limpio y sin señales
en contra; `SW` con log de SW, intento HW fallido o `hwdec-current == "no"`;
`""` (indeterminado) si no hay señal → se **avanza a la siguiente códec** en
vez de abortar (así el i5 llega a H.264, que sí decodifica por HW). Un
`END_FILE` por error o un timeout sin frames también avanza como indeterminado.

- Persistencia: primer códec con veredicto `HW` → ese perfil (se persiste);
  si al final no hay HW pero hubo ≥1 `SW` decisivo → `soft-all` (veredicto
  real, se persiste); si todo fue indeterminado → perfil vacío (soft-all
  transitorio SIN persistir, se reintenta al arrancar). Solo
  `decoded && veredicto` concluye.
- Re-sondeo único al actualizar: la lógica del sondeo se versiona en
  `settings.codec_probe_v` (`kCodecProbeVersion`). Si el valor persistido es de
  una versión anterior (p.ej. un `av1-hw` falso positivo grabado por code viejo)
  o no existe, se re-sondea UNA vez al arrancar **usando el valor guardado como
  fallback** y el veredicto fresco lo reemplaza; al completarse se marca la
  versión y los arranques siguientes son rápidos (no se re-sondea en cada boot,
  crítico en la máquina de pruebas). Ajustes → "Volver a detectar hardware" borra
  `codec_profile` y relanza `HwProbe` en caliente.
- Nombres antiguos migrados al arrancar: `modern→av1-hw`, `vp9→vp9-hw`,
  `legacy→h264-hw`, `nohw→soft-all`.
- Escalera de descenso (`PlayerWidget::streamDecodeInfoReady` → `MainWindow`,
  `hwprobe::lowerProfile`): el perfil se decide en el arranque (HwProbe) y, si la
  reproducción demuestra que el perfil promete HW para el códec que suena pero
  mpv decodifica en **software**, se baja un peldaño
  (`av1-hw → vp9-hw → h264-hw → soft-all`) y se **re-resuelve** con el perfil
  nuevo (`reloadCurrent`, SIN rotar sesión: un problema de códec no es de red).
  Reglas duras: **solo evidencia de códec/HW, NUNCA por red** (un 403, un
  visitorData vencido o un atasco de caché no cambian `hwdec-current`; la señal
  solo existe con frames decodificados en software); `soft-all` es el suelo;
  presupuesto propio y separado del de sesión (`kDescendMaxPerSession=3`) con
  anti-rebote (`kDescendCooldownMs=15 s`) y reinicio del presupuesto con una
  racha decodificada en HW. El descenso es **EFÍMERO** (no se persiste): un fallo
  puntual no "condena" la máquina; "Volver a detectar hardware" restaura. La
  comparación usa el perfil **activo en memoria** (`m_activeProfile`), no
  `settings`, o tras un descenso efímero volvería a bajar en falso.
  `--selftest-hwprobe` cubre la tabla offline de `hwprobe::lowerProfile`.
- `Yt::codecRankedScore(altura, mime) = rank*100000 + altura`: la preferencia de
  códec pesa más que la altura, así el códec que la GPU decodifica gana aunque su
  variante sea más baja (p. ej. `h264-hw` prefiere h264 1080 antes que VP9 2160,
  que iría a software). Se usa en `edgeHeight`, `buildEdgeMaster` y
  `buildFilteredMaster`. El HLS sintético AV1 (`buildAv1Hls`) solo se genera con
  perfil `Modern` (`av1-hw`).
- El selftest `--selftest-hwprobe` verifica primero la tabla de decisión
  offline (9 casos, regresión determinista) y luego imprime el perfil
  detectado (dependiente de la GPU; no es un assert fijo). NO se corre en el
  CI: el runner de GitHub no tiene GPU, así que el sondeo HW se valida en las
  máquinas de pruebas (x86/x64). Por el mismo motivo `--selftest-ffmpeg` (gate del
  FfmpegBackend) tampoco va en el smoke de CI: la VM no puede ejercitar AV1/
  DXVA2 (av1 es imposible en x86, ver arriba); se corre en la máquina de pruebas, donde
  salta el clip av1 si no hay decoder sw.

## Experimento ANGLE (zero-copy x64, opt-in)

El CPU al 100% de la máquina x64 de pruebas es el coste INHERENTE de
`d3d11va-copy`: con GL de escritorio como superficie del widget, mpv no puede
importar las texturas D3D11 del decodificador y copia VRAM→RAM→textura GL por
frame. La vía de zero-copy real en Windows con libmpv+render es **ANGLE +
`d3d11-egl`**: el contexto GL del widget es ANGLE (GLES sobre D3D11), el frame
decodificado viaja por **EGLStream** de ANGLE a la textura sin pasar por CPU.
EXPERIMENTAL y MEDIBLE con `NEVERTUBE_ANGLE=1` (solo Windows; no existe en el
build x86, que sale por `NEVERTUBE_NO_MPV`):

- `main.cpp` fuerza `QT_OPENGL=angle` antes de `QApplication` (solo si el
  usuario no fijó ya `QT_OPENGL`). Con eso Qt crea el contexto del
  `QOpenGLWidget` con ANGLE/D3D11 (todo lo demás de la UI sigue por QPainter,
  que no usa GL).
- NO hace falta pasarle handles EGL a libmpv: mpv carga `LIBEGL.DLL` con la
  búsqueda de LoadLibrary (mismo directorio → el MISMO ANGLE que Qt ya cargó)
  y toma el display en `eglGetCurrentDisplay()` del hilo de render
  (`paintGL`). `hwdec=auto` se queda (invariante protegida): en
  `video/out/gpu/hwdec.c` el driver `d3d11-egl` va ANTES que `d3d11va`, así
  que con contexto ANGLE mpv elige solo el zero-copy; con GL de escritorio
  `eglGetCurrentDisplay()` es NULL y cae a `d3d11va-copy` (comportamiento
  actual, sin daño).
- `MpvBackend::initGL` loguea `E2E GL ctx es=… vendor=… renderer=…` para
  confirmar empíricamente qué contexto hay (ANGLE → vendor "Google … (ANGLE)";
  desktop → vendor del driver).
- **Cómo medir en la máquina x64 de pruebas** (antes/después con el MISMO vídeo y calidad):
  1. `NEVERTUBE_ANGLE=1 ./nevertube_qt2` (necesita `libEGL.dll`/`libGLESv2.dll`
     junto al exe; `windeployqt` las despliega y el CI x64 lo verifica en el
     `$missing` del portátil).
  2. Con `NEVERTUBE_DEBUG=1`, el veredicto es el log de mpv + `hwdec-current`
     en el E2E: `Using hardware decoding (d3d11-egl)` = zero-copy activo;
     `(d3d11va-copy)` = no cambió nada. Comparar CPU en el Administrador de
     tareas.
- **Caídas esperadas (inocuas)**: si el ANGLE de Qt 5.15.2 no expone las
  extensiones que exige `hwdec_d3d11egl.c` (`EGL_ANGLE_stream_producer_d3d_texture`,
  `EGL_ANGLE_d3d_share_handle_client_buffer`, `EGL_EXT_device_query`, GLES
  `GL_OES_EGL_image_external_essl3`/`GL_NV_EGL_stream_consumer_external`), el
  `init` del driver falla y mpv sigue con `d3d11va-copy`: ni mejora ni rompe.
  ANGLE usa WARP solo si la GPU no llega a D3D11 FL9_3 (la Apollo Lake de la
  máquina x64 de pruebas es FL11_1: hardware). Si el modo ANGLE diera parpadeo/negro en el
  widget, el redibujado por eventos (update callback) fuerza el repintado al
  llegar cada frame; aún así, el modo
  es reversible quitando la variable.
- En Linux el guard `Q_OS_WIN` lo deja todo como estaba (GL de escritorio;
  allí el zero-copy ya es posible por VAAPI/EGL sin ANGLE, ver `hwdec-current`).

## Puntos de entrada CLI

- `./build/nevertube_qt2` → app normal
- `./build/nevertube_qt2 --search <consulta>` → buscar al arrancar
- `./build/nevertube_qt2 --channel <channelId>` → abrir canal al arrancar
- `./build/nevertube_qt2 http://...` → reproducir una URL al arrancar
- `./build/nevertube_qt2 --selftest-db` / `--selftest-player` /
  `--selftest-innertube` / `--selftest-discover` / `--selftest-home` /
  `--selftest-feeds` / `--selftest-av1hls` / `--selftest-hwprobe` / `--selftest-search` /
  `--selftest-grid` / `--selftest-theme` / `--selftest-glyphs` → pruebas

## Color de acento configurable

El acento (`settings.ui_accent`, por defecto rojo NeverTube `#FF0033`) se cambia
en caliente desde Ajustes (Interfaz → Color de acento) y se persiste.

- Fuente única de la paleta: `nt::accentOptions()` en `src/ui/theme.*`
  (label visible + hex canónico + sufijo de fichero). `accentHex()`,
  `accentHoverHex()` (mezcla ~10% blanco, el `#FF1A47` histórico) y
  `accentPressedHex()` (`darker(115)`) se leen en CADA llamada: nada `static`
  que congele el acento al primer uso.
- El QSS global se genera con placeholders `@ACCENT@`/`@ACCENT_PRESSED@`
  sustituidos por llamada (`nt::styleSheet()`), así el re-polish en caliente
  repinta slider/progreso/primary/selección/scrollbar con el color nuevo.
- La navegación inferior usa **PNGs pre-pintados por acento**
  (`nav/<key>_<state>_<suffix>.png`), NO retint en runtime: el retint era el
  origen de dos bugs (cuadrado negro por `QColor(QRgb)` que forzaba alpha=255,
  y acento congelado por cachear el color en un `static`). NUEVO COLOR DE
  ACENTO: añadirlo a `nt::accentOptions()` y regenerar los PNG con
  `tools/gen_nav_accent.cpp` (el sufijo `rojo` se copia byte-idéntico de los
  PNG originales; el resto re-tinta preservando luminosidad y alfa).
- `MainWindow::accentChanged` aplica en caliente: stylesheet global, re-elección
  de iconos de la nav (solo re-evalúa el sufijo), repintado de `VideoListView`
  y `applyAccent()` en watch/channel/home/settings (suscribirse, acciones
  activas, enlaces de la descripción, empty state, botón reset).

## Color de SELECCIÓN: texto y sugerencia (del usuario)

Dosposites que se perdían de vista y que el usuario señaló:

- **Seleccionar texto va en el gris de la app** (`kSelection` #2D2D2D, el mismo
  token de los menús), no en el acento. Antes el `QLineEdit` pintaba
  `selection-background-color: @ACCENT@` (rojo) y quedaba como un error. La
  descripción del Player (un `QLabel` con `Qt::TextSelectableByMouse`) no la
  cubre el QSS: la pinta con `QPalette::Highlight`, que sin más salía con el
  azul del tema del escritorio. Por eso **la paleta se aplica en `main.cpp`**
  (`nt::applyTextSelectionPalette`, justo tras crear la `QApplication`, sobre
  `app.palette()`): es lo único que llega a los widgets seleccionables que el
  QSS no alcanza. Solo se tocan `Highlight` y `HighlightedText` (este a `kText`,
  11:1 sobre `kSelection`) para no cambiar nada más del tema del escritorio.
  Medido sobre un render real: banda de selección `#2D2D2D` sobre campo `#1F1F1F`,
  texto dentro en `#ECECEC`.
- **La fila de sugerencia resaltada lleva el acento.** Se perdía porque
  `QWidget#suggestRow[highlight="true"]` era `#1F1F1F`, MÁS oscuro que el popover
  `#202020`: era un oscurecimiento, no un resaltado. Ahora es una **barra de
  acento de 3 px a la izquierda** + un velo `@ACCENT_A16@`, sobre hover en
  `kSelection`. El texto se queda en `kTextSoft`/`kMuted` (siguen dando 8:1 y 4:1
  sobre el velo), así que no hay que tocar las etiquetas hijas. El borde
  izquierdo se reserva siempre (3 px transparentes en la fila normal) para que la
  barra no empuje el texto: medido, el texto arranca en `x=13` con la fila
  resaltada y sin ella. La propiedad dinámica la pone `SuggestRow::setHighlighted`
  (con re-polish, que es obligatorio: cambiar la propiedad no repinta solo).

`--selftest-theme` cubre las dos: paleta en `kSelection`/`kText`, que el QSS no
seleccione en el acento, y que el bloque `[highlight="true"]` lleve el acento.

## Selectores del QSS sin widget detrás (`--selftest-theme`)

Una regla cuyo `#nombre` no se lo pone **nadie** a ningún widget no pinta nada:
se aplicaba a la nada y no se notaba. Por eso `QPushButton#danger` puede estar
en la hoja sin que se note (se borró; además era malo por usar el acento para
decir "peligroso", que solo funciona si el acento es rojo).
`--selftest-theme` lo caza: extrae los ids del QSS y exige que cada uno aparezca
como **literal** en algún sitio de `src/`.

Dos decisiones que hacen que el gate sirva y no estorbe:

- **Se miran los literales de TODO `src/`, no solo los argumentos de
  `setObjectName`.** Hay helpers que reciben el nombre como parámetro y lo
  aplican por dentro (`makeSeg` en `watch_page.cpp` pone `#actionSegL` y
  `#actionSegR`): con el criterio estricto salían como muertos sin serlo, y un
  falso positivo enseña a ignorar el test. El trade-off es el inverso — que el
  mismo nombre se cite de verdad en otro sitio y tape un selector muerto —, pero
  eso es un falso **negativo**: avisa el CI y se mira, mientras que el falso
  positivo mata el gate.
- **Los comentarios se descartan antes de mirar los literales**, con los mismos
  `sinComentarios`/`literalesDe` que usa `glyphSelfTest`. Si no, bastaría con
  citar el nombre en un `// este botón se llama X` para que un selector muerto
  pasara por vivo, y el propio comentario del gate se autobloquearía.

El estado de bloque **no cruza de fichero a otro**: un `/*` sin cerrar dejaría el
fichero siguiente entero marcado como comentario y sus literales no se verían
(agujero silencioso). Si el árbol de fuentes no está junto al binario se salta
con un `[info]` y no falla, igual que hace `glyphSelfTest`.

Lo ha encontrado de verdad: `QLabel#muted` estaba en la hoja y el literal
`"muted"` no aparecía por ningún sitio (el color sigue disponible para quien lo
necesite como `@MUTED@`/`kMuted`).

## Glifos: la fuente por defecto NO tiene todos los caracteres (medido)

La UI pinta su texto con la fuente por defecto del sistema, y esa fuente
(Liberation Sans en Linux, la que resuelve fontconfig) **no cubre todos los
caracteres no ASCII**. Cuando el carácter no está, Qt pinta el *marcador de
glifo ausente*: medido en pantalla, son **2x5 px**, o sea un botón casi
invisible. Cuatro glifos de la UI estaban en esa situación:

| glifo | U+ | sitio | se ve como |
|---|---|---|---|
| `✕` | 2715 | botón X de limpiar del buscador | marco vacío con un punto |
| `◀` | 25C0 | botón "◀ Volver" del Player (además duplicaba el icono `Back` que ya tenía) | un punto antes de "Volver" |
| `▾` | 25BE | sufijo de los 3 botones de valor de Ajustes (`makeValueButton`) | un punto |
| `✔` | 2714 | etiqueta "ya está" del menú de playlists | un punto |

Cómo se arregla, por orden de preferencia:

1. **Icono de `nt::icon()`** (`src/ui/icons.*`): dibuja con `QPainter`, no
   depende de ninguna fuente. Es lo que se usa para el X de limpiar y para el
   "Volver" (que ya tenía el icono `Back`: el `◀` del texto era redundante).
2. **Un glifo que la fuente sí tenga** (comprobarlo antes, ver el selftest de
   abajo).
3. **Quitarlo**: si el texto ya dice lo mismo ("✔ ... (ya está)"), el glifo es
   decorativo y sobra. Es lo que acabó pasando con el `▾` de los botones de
   valor de Ajustes: el `▼` (U+25BC) que se puso por él SÍ está en la fuente,
   pero el usuario pidió quitarlo del todo — el botón ya es un control con su
   caja y el menú marca la opción con un check, así que el triángulo era
   decoración redundante. `makeValueButton` es hoy un envoltorio de
   `makeActionButton` con un comentario que prohíbe reintroducirlo.
4. Lo que **no** hay que hacer es confiar en que el glifo se ve: `✕`, `▾`, `◀` y
   `✔` no están en Liberation Sans y en pantalla salían como un punto.

`--selftest-glyphs` (`nt::glyphSelfTest` en `src/ui/theme.cpp`, en el smoke de
los dos CI) recorre los **literales del propio árbol de fuentes** —los
comentarios se descartan antes— y comprueba cada carácter no ASCII contra la
fuente real de la app: un glifo nuevo que no exista sale en el CI en vez de en
la pantalla del usuario. 66 ficheros, ~3360 literales, 17 glifos.

- **OJO: `QFontMetrics::inFont()` NO sirve para esto.** Consulta el *fallback*
  del sistema y devuelve true si **cualquier** fuente instalada tiene el
  carácter (aquí hay 25 que cubren U+2715), mientras la app seguía pintando el
  marcador de glifo ausente. Lo que decide es si lo tiene **el fichero** de la
  fuente de la app, y eso lo responde `QRawFont::fromFont(appFont)` +
  `supportsCharacter()`. Si la fuente no se puede abrir (una fuente compuesta
  del sistema), se cae a `inFont` y se avisa por pantalla.
- El `✕` que queda en los comentarios del código es a propósito: el test los
  descarta, y citarlos explica por qué están prohibidos.

## Caché de miniaturas y suscripciones

- `ThumbnailCache::pixmap()` **cae a disco** si el LRU desalojó la imagen de
  memoria. NO quitar ese fallback: el delegate de `VideoListView` pinta leyendo
  de `pixmap()` en cada paint y nadie vuelve a pedir un ítem ya mostrado, así
  que sin él las miniaturas "desaparecen" (search/canal/home) al compartir
  presupuesto entre páginas.
- Al suscribirse se persiste el avatar URL (`SubscriptionsRepo::add(..., avatar)`),
  y `SubscriptionsRepo::setAvatar` lo actualiza cuando llega `ChannelInfo`.
  `LibraryPage` pide `channelInfo` también cuando `s.avatar` está vacío (aunque
  ya tenga el conteo), para rellenar avatares de suscripciones antiguas.
- El feed "Nuevos de tus suscripciones" se invalida al entrar a Home comparando
  la firma de ids de suscripciones guardada en `settings.feed_subs_sig`. Sin esa
  comprobación el feed cacheado (TTL 30 min) seguiría mostrando solo las
  suscripciones previas tras suscribirse.

## "Continuar viendo" (duración + posición en el historial)

**La regla de "estado de reproducción" vive en UN sitio**:
`HistoryRepo::progressFraction(v, positionMs)` (src/db/repositories.cpp) devuelve
la fracción 0..1 de la barra, o 0 si no se puede mostrar (sin posición, sin
duración conocida, ya completado, o fracción fuera de (0,1)). La llaman los TRES
sitios donde se ve un vídeo con progreso, y no hay una cuarta copia:

| sitio | quién pinta la barra |
|---|---|
| Home, "Continuar viendo" | delegado de `VideoListView` (`drawProgressBar`) |
| página Historial ("Ver todo") | el mismo delegado, en `ListMode` |
| sección Historial de **Biblioteca** | `AsyncThumb::paintEvent` |

La sección de Biblioteca la pintaba a mano porque sus filas son `RowItem`
(miniatura `QLabel` + texto), no el delegado; por eso usa las MISMAS medidas que
`drawProgressBar` (2 px de margen, 3 px de alto, 1 px por encima del borde,
radio 1,5) recortada al rectángulo redondeado de la miniatura. Como se pinta a
mano con `nt::kAccent` (el QSS no alcanza un `paintEvent`), existe
`LibraryPage::applyAccent()` para repintar las miniaturas al cambiar el acento en
caliente, y `MainWindow` lo llama con las demás páginas. `buildRows` recibe un
`QHash<videoId, fracción>`; vacío = sin barra, y solo la sección de Historial lo
pasa.

`HomePage::loadContinueWatching` exige, para cada id reciente: `pos > 0`, fila
en `videos` (la caché), **`durationSeconds > 0`**, no completado y 0 < frac < 1
(las tres últimas cosas son lo que devuelve `progressFraction`).
Los dos últimos datos los escribe el reproductor, no la red:

- **Duración**: `PlayerWidget::durationUpdated` (lo emiten los DOS motores:
  `mpv_backend.cpp` observando la propiedad `duration` y `ffmpeg_backend.cpp`
  con el reloj de audio) → `PlayerController::onPlayerDuration` →
  `durationChanged(double)` → `WatchPage::onDurationKnown`, que la guarda en la
  caché. Es el dato AUTORITATIVO: `parseVideoDetail` (`/next`) no rellena
  duración y los renderizadores de origen (search/home/canal) a veces tampoco, así
  que sin esto un vídeo visto varios minutos se quedaba con `duration=0` y
  NUNCA aparecía en "Continuar viendo" (el filtro lo descartaba en silencio).
  `PlayerController` compara contra `m_reportedDuration` (reiniciado en cada
  `playIndex`), no contra `m_duration`: los ticks de posición también actualizan
  `m_duration` y podrían "consumirse" el aviso antes de que llegue la señal.
  `onDurationKnown` rellena SOLO la duración y respeta lo ya cacheado — el
  `upsert` es `INSERT OR REPLACE`, así que meter el ítem de cola tal cual
  borraría título/canal/miniatura. Por lo mismo `onDetailReady` conserva la
  duración buena cuando el `/next` la trae a 0.
- **Posición**: `kPositionFlushMs = 20000` (20 s, antes 60 s: era el retardo
  visible de la barra de progreso; YouTube guarda cada ~15-20 s). `savePosition()`
  descarta la escritura si es el mismo vídeo y la posición no ha avanzado 2 s,
  así que con el vídeo en pausa no se escribe nada. El coste por escritura es un
  `INSERT OR REPLACE` de una fila (SQLite local) más el `changed()`.
  Puntos en los que se guarda, todos ya en el código:
  `playPause` (pausa), `playIndex` (cambio de vídeo: click explícito en
  cualquier página, "A continuación" o auto-advance al terminar), `stop()`
  (botón "Cerrar y detener" del mini), `aboutToQuit`, y
  `MainWindow::savePositionNow()` al **entrar al Home** con algo reproduciéndose
  en el mini player (`onNavButton` case 0 y `showPreviousTab` con destino 0).
  Ese último es el que hace que la fila se pinte con el progreso REAL, y va
  **antes** de `refreshDynamic()` a propósito: en `onNavButton` el
  `setCurrentIndex` va después, así que la página sigue oculta y el `changed()`
  del guardado lo salta el guard de visibilidad de `HomePage`.
  `HomePage::refreshContinueWatching` (conectado a `HistoryRepo::changed`) hace
  early-return si la página está oculta: cada flush con la app en el mini player
  no debe reconstruir una rejilla que nadie ve.
- **Al verificar en BD, copia también `mini_tube.db-wal`**: la app usa WAL y
  copiar solo el `.db` da un snapshot obsoleto (parece que no se guarda nada).

## Convenciones

- UI, lógica y reproducción separadas. `PlayerController` (estado/cola) vs
  `PlayerWidget` (render) vs páginas (vistas).
- Repos SQLite en `src/db/`; infraestructura reutilizable en `src/ui/`.
- **UN solo diálogo para confirmar y UNO solo para pedir un texto**, y son
  `nt::confirm` / `nt::inputText` (`src/ui/confirm_dialog.*`). Los dos montan
  un overlay **hijo de la VENTANA** (`page->window()`), con velo, modal, sin
  `QDialog` y sin ventana flotante: una ventana propia secuestra el puntero.
  Regla dura: si aparece una acción destructiva o un campo de texto, se llama a
  uno de los dos; no se monta un `QFrame#popupDialog` por página. Hay siete
  sitios de confirmación (las tres de Ajustes, borrar el historial de búsquedas,
  borrar el historial de vídeos, y eliminar playlist en Biblioteca y en su
  página) y cinco de texto (crear playlist en Biblioteca, Historial y Player,
  y renombrar en Biblioteca y en su página). Los popovers que quedan son
  deliberados y **no** son diálogos: son listas con scroll (gestionar
  suscripciones, añadir a playlist, calidad). La caja mide 400 px en
  `nt::confirm` (el cuerpo es un párrafo que envuelve) y 340 px en
  `nt::inputText` (un campo suelto).
- Resolución de streams: Innertube (cliente VISIONOS, `src/yt_innertube.*`) como única vía; si falla, `Yt::resolveError`.
- Carga de canales: Innertube `fetchChannelVideos` (browse pestaña "Vídeos"), sin carrera ni fallback externo. Un resultado con 0 vídeos de id no vacío cuenta como vacío.
- Búsqueda, ficha de canal, pestañas y detalle: TODO por Innertube (`src/yt_innertube.*`):
  - `Yt::search` → POST `/search` (paginación por `continuation`; parser `compactVideoRenderer`).
  - `Yt::channelInfo` → browse pestaña "Información": `c4TabbedHeaderRenderer` (nombre, avatar, suscriptores) + `channelAboutMetadataRenderer` (descripción).
  - `Yt::channelTabVideos` → browse del canal con el cliente **WEB** para las tres pestañas (videos/shorts/live): VISIONOS solo expone "Vídeos" y su continuación devuelve páginas vacías, mientras que WEB sirve las tres con layout moderno (`lockupViewModel`/`shortsLockupViewModel`) y continuación funcional. El cliente WEB no necesita API key. `fetchChannelVideos` (feed de suscripciones, sin paginación) sigue por VISIONOS con `compactVideoRenderer`.
  - `Yt::getDetail` → `/next`: `videoMetadataRenderer` (título/descripción traducidos, vistas, fecha, canal) + `likeButtonRenderer` (me gusta). El **id del canal** sale de `owner.videoOwnerRenderer.navigationEndpoint.browseEndpoint.browseId` y, si falta (vídeos de COLABORACIÓN: el owner solo trae `avatarStack` + `subscribeButton`), de `owner.subscribeButton.subscribeButtonRenderer.channelId`: sin esa vía el `channelId` quedaba vacío y los botones "Canal" y "Suscribirse" no hacían nada (los dos salen temprano si está vacío). Los renderers de la cola (`compactVideoRenderer`/`playlistPanelVideoRenderer`) también sacan el id de `longBylineText`/`shortBylineText` → `runs[0].navigationEndpoint.browseEndpoint.browseId` (`channelIdFromByline`), que antes solo se leía el nombre.
  - `Yt::playlistDetail` → browse `VL<id>`: `playlistHeaderRenderer` + `playlistVideoRenderer`.
- **Buscar con Enter cierra el desplegable de sugerencias de verdad**: `doSearch()`
  para el debounce de 300 ms y marca la consulta como ya buscada
  (`SearchPage::m_suggestClosedFor`), y `onSuggestReady` la ignora. Sin eso, al
  escribir rápido y pulsar Enter la petición de sugerencias salía después y el
  panel reaparecía encima de los resultados (también con una petición en vuelo).
  Volver a escribir limpia la marca.
- Empaquetado Windows (Win7 x86): la UCRT NO se copia app-local desde la Windows SDK — esa versión importa `GetSystemTimePreciseAsFileTime` (W8+) y ucrtbase no carga en Win7. En su lugar el setup empaqueta `VC_redist.x86.exe` (descargado en CI) y lo instala con `Exec` (`/install /passive /norestart`, códigos 0/1638/3010 aceptados) con `PrivilegesRequired=admin`. El CRT (msvcp140/vcruntime140) sí va junto al exe (`windeployqt --compiler-runtime`). ORDEN del setup: 1) app completa → 2) iconos (menú + acceso directo al escritorio, casilla `[Tasks] desktopicon` marcada por defecto) → 3) `InstallVcRedist` desde `CurStepChanged` → `ssPostInstall` (no en `AfterInstall` de un `[Files]`, que correría en mitad de la copia). El `[InstallDelete]` sigue purgando la UCRT app-local de instalaciones antiguas. El setup x86 **cancela en Windows de 64 bits 10/11** (`InitializeSetup` + `IsWin64` + `GetWindowsVersion >= $0A000000`) y dirige al build x64 (el `MsgBox` nombra `nevertube-setup-x64.exe`: cancelar sin decir dónde está el instalador que sí vale deja al usuario sin salida): el x86 ahí quedaría capado a h264 HW + VP9 software y sin AV1. Se permite en 32 bits y en 64 bits < 10 (donde el x64/mpv moderno no corre). **TOOLSET y REDIST (Win7)**: el x86 compila con `-T v142` (VS2019/14.29) sobre `runs-on: windows-2022` y el setup instala `vc_redist.x86` **2015-2019** (`aka.ms/vs/16`): el target (Win7) necesita ese CRT. `windows-latest` ya es VS2026 (MSVC 14.5x) y su runtime NO es Win7-compatible (imports W8+); un binario 14.5x tampoco enlaza contra el vs/16. No quitar el pin de toolset/imagen ni cambiar el redist a `vs/17`+ sin prueba en el target. Además, el CI purga `qtaudio_wasapi.dll` del paquete antes del gate: Qt5 Multimedia la despliega pero importa `RoGetActivationFactory` (W8+) y el gate de imports lo aborta; en Win7 el audio va por `qtaudio_windows.dll` (WaveOut), no WASAPI.
- Empaquetado Windows x64 (Win10+): el entregable es un **setup.exe instalable**
  (`dist64.iss`, compilado con `iscc` en `windows_x64.yml`), NO un zip portátil.
  Es el gemelo de `dist.iss` (x86/Win7) con 5 diferencias, todas deliberadas:
  (1) `ArchitecturesInstallIn64BitMode=x64compatible` para que `{autopf}` sea
  `C:\Program Files` de 64 bits y no `Program Files (x86)`, y
  `ArchitecturesAllowed=x64compatible` (64 bits real y Win11 ARM emulado);
  (2) `MinVersion=10.0` (mpv moderno + Vulkan no existen antes de Win10). OJO:
  Win10 se reporta como 6.3 salvo el manifiesto de compatibilidad que el setup de
  Inno 6 sí lleva; con Inno 5.5.5 ese `MinVersion` rechazaría el propio Win10;
  (3) NO hay problema de UCRT (Win10+ la trae en el sistema) → sin
  `[InstallDelete]` de la UCRT app-local, y el redist es `VC_redist.x64.exe` de
  `aka.ms/vs/17` (el 2022+, sin la restricción de imports W8+ que obliga al x86
  a usar vs/16); (4) los ficheros vienen de `dist\` con `VC_redist.x64.exe`
  excluido y copiado a `{tmp}` para que lo lance `InstallVcRedist` desde
  `CurStepChanged`; (5) **NO hay `InitializeSetup`**: el guard de arquitectura
  del x86 cancelaba en 64 bits 10/11 y aquí el 64 bits ES el objetivo (un 32
  bits lo rechaza `ArchitecturesAllowed` antes de escribir en disco, con su
  propio mensaje). Lo que SÍ comparte con el x86: `[Tasks] desktopicon` marcado
  por defecto, `[UninstallDelete]` de la BD (`{userdocs}\mini_tube`) y de las
  cachés (`{localappdata}\nevertube\nevertube`), `PrivilegesRequired=admin` y el
  mismo orden app → iconos → redist. En `[Code]` los comentarios van con `//`:
  el `;` solo es válido en el resto de secciones (dentro de `[Code]` es
  separador de sentencias y `iscc` lo rechaza). El gate del workflow exige que
  el setup pese >5 MB y comprueba los 9 ficheros clave contra el manifiesto que
  `iscc` imprime (`Compressing: <ruta>` por fichero, guardado en `iscc.log`), no
  contra `dist\`: un patrón de `[Files]` mal escrito compila igual. NO se usa
  `innoextract` para eso: su última versión es 1.9 (2020) y no sabe leer el Inno
  6.7 que instala choco (loader revision 2), así que solo daría un aviso; el log
  del compilador es la fuente autoritativa (y ver `VC_redist.x64.exe` en la lista
  es la prueba de que se compiló `dist64.iss` y no el del x86).
- Overlay de los controles NO es un widget separado: vive dentro de
  `WatchPage::m_overlay`/`PlayerContainer` (src/ui/pages/watch_page.cpp).
  Cualquier cambio ahí que afecte el área de video requiere el selftest.
- **Los botones multimedia NO llevan marco** (regla del usuario). Ni en reposo
  ni **al pulsarlos**. El chip de calidad (`QPushButton#chip`, el que al pulsarlo
  abre el menú de resoluciones) lo tenía y se veía como una caja más **dentro**
  de la caja de controles. Su borde ahora es `1px solid transparent` y no
  `none`: no se ve ninguno, pero se conservan los 2 px para que el contenido no
  se desplace. **El `:focus` hay que anularlo a mano** (`border-color:
  transparent`): la regla global `QPushButton:focus { border-color: @ACCENT@; }`
  se le aplica igual y encendía un marco en el acento en cuanto se pulsaba el
  botón — que es justo cuando se abre el menú. El foco se marca SOLO con el
  relleno (`@HANDLE_HOVER@`), que es lo único que se puede tocar sin dibujar un
  borde. Lo que marca el chip como control es el **relleno**, no el marco (el
  resto de la barra, `#iconBtn`, nunca lo tuvo).
- **Los botones de acción de la página Player** (`#actionChip`: Compartir, Me
  gusta (favoritos/estrella), Ver más tarde y Añadir a playlist) siguen la misma
  regla que **Me gusta / No me gusta** (`#actionSegL`/`#actionSegR`, que ya
  llevaban `border: none`): **sin marco en ningún estado**. `#actionChip` no
  declaraba borde, así que heredaba el global `1px solid @BORDER@` y el
  `QPushButton:focus` lo encendía en el acento al pulsarlo (un aro rojo). Ahora
  declara `1px solid transparent` y su `:focus` se queda transparente; el estado
  sigue marcándose por el relleno (`active` y `:hover`) y por el color del icono.
  Lo vigila `--selftest-theme`: `#chip`, `#iconBtn`, `#actionChip` y los items de
  `#qualityMenu` no pueden declarar un borde visible **en ningún estado**
  (recursea por `:hover`/`:pressed`/`:focus`; comprobado que FALLA tanto si se
  devuelve el marco en la base como si se devuelve solo en `:focus`).

## Cobertura de ventanas y escala en x86 (FfmpegBackend)

Síntoma posible (Win7 x86): **drops de FPS al abrir una ventana flotante
encima del vídeo, con CPU/RAM de la app bajos**. Dos mecanismos:

1. Con DWM (Aero *o* tema Básico, que también compone) Windows **no le avisa a
   la app de que la tapan**: el `QOpenGLWidget` sigue recibiendo paint events y
   el FfmpegBackend seguía decodificando+convirtiendo+pintando frames que nadie
   ve. El cuello de botella es la **presentación** del compositor en la GMA 950
   (blend de la región superpuesta cada frame), no el cómputo de la app — por
   eso el CPU/RAM quedan bajos. El redibujado del reproductor no es el
   culpable.
2. El arreglo es quirúrgico y **solo aplica a FfmpegBackend**:
   - `PlayerWidget` arranca un timer de **250 ms** (solo si
     `occlusionSkipSupported()`) que recorre el **z-order de Windows**
     (`GetWindow` desde arriba hasta nuestra top-level; ventanas visibles, no
     `WS_EX_LAYERED`, que cubran por completo el rect del widget). Cubierto →
     `setVideoOccluded(true)` y: el hilo de decode deja de hacer copy-back y
     `swscale` (el frame se descarta sin tocar CPU/RAM, manteniendo el decode
     caliente y la **posición al día con el reloj de audio** — crítico: sin eso
     el watchdog de atasco vería la posición congelada > 5 s con el vídeo
     tapado y dispararía un falso `streamStalled`) y `paintGL` sale temprano
     (no sube texturas ni pinta). Es frame-driven: al destapar se muestra el
     frame más reciente, sin ráfaga. En **mpv/x64 NO aplica**: parar el render
     ahí acumula frames y produce el "salto al volver" (ver mini player);
     `MpvBackend` no sobreescribe los virtuales.
   - `frameToImage` escala **solo hacia abajo** hasta el tamaño del surface de
     render (px físicos, dados por `renderGL`), respetando el aspect del
     stream: menos píxeles en `swscale` y en la subida de textura (win también
     fuera del solapamiento: el mini bar 112×63 convierte casi gratis). Si el
     surface es mayor que el stream, se queda a resolución completa (el
     upscale lo hace GL, como antes). El guard del recorrido es `Q_OS_WIN`
     (los child widgets no aparecen en la cadena de `GetWindow`).

_Futuro (pendiente, decidido 2026-09-24):_ port de la detección de cobertura a
**X11 para Puppy Linux** (máquina x86 como banco de pruebas del FfmpegBackend
sin Windows): mismo timer de 250 ms + `XQueryTree` desde la raíz hasta nuestra
top-level (`map_state == IsViewable`, geometría + `XTranslateCoordinates` a
coordenadas de pantalla), misma regla → `setVideoOccluded`. Lo único de
plataforma es la DETECCIÓN (`occludedByTopWindow`, hoy `#ifdef Q_OS_WIN`): el
plumbing (timer, `setVideoOccluded`, descarte de frames del hilo) ya es neutro.
Con CMake "inteligente" (build Linux sin mpv → `NEVERTUBE_NO_MPV` en Linux, o
forzar `NEVERTUBE_BACKEND=ffmpeg`; el FfmpegBackend ya compila en Linux por
pkg-config). En **Wayland NO** es posible (no hay z-order global). La escala al
surface ya funciona en cualquier Linux sin tocar nada.

## Mini player con vídeo en vivo (reparent del PlayerWidget)

Al salir de la página Player con reproducción activa, el mini player muestra
**la miniatura estática** y, debajo de ella, el `PlayerWidget` sigue
renderizando de verdad. Motivo: si el widget se oculta, mpv deja de dibujar
y acumula frames que, al volver, reproduce en ráfaga para igualar el audio
(el "salto al volver", peor cuanto más tiempo en mini). `vid=no`/`framedrop`
fuerzan reconfig o descartes que se ven como parpadeo. En cambio, reparentar
el `PlayerWidget` al hueco del mini y dejarlo visible (aunque tapado por la
miniatura) mantiene el pipeline caliente SIN reconf del decodificador y SIN
frames acumulados.

- `WatchPage::detachPlayerTo(host)` reparenta el `PlayerWidget` al host del
  mini bar y oculta el overlay de controles; `attachPlayer()` lo devuelve al
  `PlayerContainer` (que lo reintegra en su layout y recalcula el overlay).
  El overlay es hijo del player y se mueve con él (no se re-parenta).
- Reparentar **dentro de la misma ventana top-level conserva el contexto GL y
  el `mpv_render_context`** (verificado: `initializeGL` solo 1 vez en todo el
  ciclo mini→volver). NO reparentar a otra ventana: destruiría el contexto.
- `MainWindow::updateMiniPlayer` gestiona las transiciones en los cambios de
  estado (guard `m_miniDetached`); `applyMiniVideoMode()` aplica el setting
  `mini_video` (`video` → el vídeo en vivo llena el hueco y la miniatura se
  oculta; `thumb` → la miniatura va POR ENCIMA con `raise()` y el vídeo
  renderiza invisible debajo a 112x63). En ambos modos el `PlayerWidget` sigue
  visible y renderizando: solo cambia qué tapa qué.
- Orden importante: `MainWindow::openWatchAt` cambia a la página Player ANTES
  de activar la cola; si se activa primero, `updateMiniPlayer` ve "reprodu-
  ciendo fuera de Player" y engancha/suelta el mini un frame (parpadeo al
  abrir un vídeo).
- `PlayerContainer` (antes en anónima) vive en `watch_page.cpp` a nivel global
  solo para poder referenciarlo desde `WatchPage` (miembro `m_playerContainer`).