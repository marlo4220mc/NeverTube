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

## Pieza protegida (NO modificar sin razón técnica y prueba)

El reproductor funcional es una pieza protegida. Los siguientes elementos NO
se deben tocar sin una razón técnica respaldada por una prueba concreta:

- `QOpenGLWidget` (clase `PlayerWidget`) como superficie de video
- `QTimer` de redibujado a **33 ms** forzando `QWidget::update()`
  (`initializeGL`)
- `vo=libmpv` + `hwdec=auto` (funciona con y sin aceleración de hardware)
- Renderizado directo a FBO (`mpv_render_context_render`) en `paintGL`
- Cualquier cambio a la integración mpv (libmpv / mpv_render_context)

Contexto: el primer intento de separar los controles multimedia (overlay
flotante sobre el video) rompió la app y fue revertido. Hacer cambios de UI
que toquen el reproductor por incrementos pequeños y verificando cada uno.

## Layout de rejilla (vídeos)

La rejilla del Home mantiene **5 columnas fijas** en cualquier ancho de
ventana. Cada celda se dimensiona según su contenido real
(`VideoListView::applyGridGeometry`): `cellH = miniatura 16:9 + bloque de
texto fijo` (hueco 8 + título 34 + línea de canal). NO usar un ratio fijo
(`cellW * 258/320`) para la altura: al estrechar la ventana el texto no
escala y un vídeo tapa el nombre del canal del siguiente. Ningún vídeo debe
solaparse sobre otro, ni en vertical ni en horizontal.

## Altura del área de vídeo (WatchPage/PlayerContainer)

`PlayerContainer::updateMaxHeight()` capa el área del vídeo con
`min(width/aspect, kMaxPlayerHeight=560)` para que descripción + "A
continuación" no se queden sin espacio en ventanas grandes/maximizadas. Ese
tope absoluto **NO aplica en pantalla completa** (`setFullscreen(true)` →
`capped = width/aspect`): si se re-aplica el 560 en fullscreen el vídeo queda
en 560 px con la pantalla llena de negro (regresión vista y revertida). El
tope sí debe seguir aplicando en ventana normal y maximizada.

## Pruebas de regresión

```sh
./build/nevertube_qt2 --selftest-db        # repos/SQLite
./build/nevertube_qt2 --selftest-player    # hwdec=auto + timer=33ms + render ok
./build/nevertube_qt2 --selftest-innertube # sw.js_data→visitorData→player→hlsManifestUrl
./build/nevertube_qt2 --selftest-discover  # radios guardadas: guardar/recuperar/re-seleccionar/persistencia/descarte
./build/nevertube_qt2 --selftest-av1hls    # HLS sintético AV1 (offline): MAP/BYTERANGE, /seg.mp4, &nt único
./build/nevertube_qt2 --selftest-hwprobe   # sondeo HW offline (micro-clips): av1-hw|vp9-hw|h264-hw|soft-all
./build/nevertube_qt2 --selftest-ssl       # backend TLS (OpenSSL 1.1 junto al exe) cargado
./build/nevertube_qt2 --selftest-tls       # handshake TLS REAL contra hosts públicos (no solo carga)
./build/nevertube_qt2 --selftest-search    # búsqueda Innertube REAL: vídeos con id no vacío
./build/nevertube_qt2 --selftest-browse    # endpoints Innertube: canal + tabs (videos/shorts/live) + detalle + playlist
./build/nevertube_qt2 --selftest-feeds     # canal por Innertube + trending (protocolo y cadena)
```

El selftest del player verifica invariantes del núcleo protegido sin
reproducir ningún archivo. Ejecutar ambos antes de considerar estable un
cambio que toque el reproductor.

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
  agotaba ~60 s mudos por intento hasta el EOF). NO toca el render ni el timer
  de 33 ms: es lógica de eventos y del watchdog de `checkStall` (audio).
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
defecto, evitando ruido en el bucle de 33 ms):

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
  offline (9 casos, regresión determinista en CI) y luego imprime el perfil
  detectado (dependiente de la GPU; no es un assert fijo).

## Puntos de entrada CLI

- `./build/nevertube_qt2` → app normal
- `./build/nevertube_qt2 --search <consulta>` → buscar al arrancar
- `./build/nevertube_qt2 --channel <channelId>` → abrir canal al arrancar
- `./build/nevertube_qt2 http://...` → reproducir una URL al arrancar
- `./build/nevertube_qt2 --selftest-db` / `--selftest-player` /
  `--selftest-innertube` / `--selftest-discover` / `--selftest-home` /
  `--selftest-feeds` / `--selftest-av1hls` / `--selftest-hwprobe` / `--selftest-search` → pruebas

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

## Convenciones

- UI, lógica y reproducción separadas. `PlayerController` (estado/cola) vs
  `PlayerWidget` (render) vs páginas (vistas).
- Repos SQLite en `src/db/`; infraestructura reutilizable en `src/ui/`.
- Resolución de streams: Innertube (cliente VISIONOS, `src/yt_innertube.*`) como única vía; si falla, `Yt::resolveError`.
- Carga de canales: Innertube `fetchChannelVideos` (browse pestaña "Vídeos"), sin carrera ni fallback externo. Un resultado con 0 vídeos de id no vacío cuenta como vacío.
- Búsqueda, ficha de canal, pestañas y detalle: TODO por Innertube (`src/yt_innertube.*`):
  - `Yt::search` → POST `/search` (paginación por `continuation`; parser `compactVideoRenderer`).
  - `Yt::channelInfo` → browse pestaña "Información": `c4TabbedHeaderRenderer` (nombre, avatar, suscriptores) + `channelAboutMetadataRenderer` (descripción).
  - `Yt::channelTabVideos` → browse del canal con el cliente **WEB** para las tres pestañas (videos/shorts/live): VISIONOS solo expone "Vídeos" y su continuación devuelve páginas vacías, mientras que WEB sirve las tres con layout moderno (`lockupViewModel`/`shortsLockupViewModel`) y continuación funcional. El cliente WEB no necesita API key. `fetchChannelVideos` (feed de suscripciones, sin paginación) sigue por VISIONOS con `compactVideoRenderer`.
  - `Yt::getDetail` → `/next`: `videoMetadataRenderer` (título/descripción traducidos, vistas, fecha, canal) + `likeButtonRenderer` (me gusta).
  - `Yt::playlistDetail` → browse `VL<id>`: `playlistHeaderRenderer` + `playlistVideoRenderer`.
- Empaquetado Windows (Win7 x86): la UCRT NO se copia app-local desde la Windows SDK — esa versión importa `GetSystemTimePreciseAsFileTime` (W8+) y ucrtbase no carga en Win7. En su lugar el setup empaqueta `VC_redist.x86.exe` (descargado en CI) y lo instala con `Exec` (`/install /passive /norestart`, códigos 0/1638/3010 aceptados) con `PrivilegesRequired=admin`. El CRT (msvcp140/vcruntime140) sí va junto al exe (`windeployqt --compiler-runtime`). ORDEN del setup: 1) app completa → 2) iconos (menú + acceso directo al escritorio, casilla `[Tasks] desktopicon` marcada por defecto) → 3) `InstallVcRedist` desde `CurStepChanged` → `ssPostInstall` (no en `AfterInstall` de un `[Files]`, que correría en mitad de la copia). El `[InstallDelete]` sigue purgando la UCRT app-local de instalaciones antiguas. Además, el CI purga `qtaudio_wasapi.dll` del paquete antes del gate: Qt5 Multimedia la despliega pero importa `RoGetActivationFactory` (W8+) y el gate de imports lo aborta; en Win7 el audio va por `qtaudio_windows.dll` (WaveOut), no WASAPI.
- Overlay de los controles NO es un widget separado: vive dentro de
  `WatchPage::m_overlay`/`PlayerContainer` (src/ui/pages/watch_page.cpp).
  Cualquier cambio ahí que afecte el área de video requiere el selftest.

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