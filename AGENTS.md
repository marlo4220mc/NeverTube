# nevertube_qt2

Cliente ligero de YouTube en Qt5 (C++17 / CMake). Núcleo de reproducción
basado en un POC probado. Philosophy: ligero antes que completo, reproducir
antes que decorar, estabilidad y bajo consumo.

## Build

```sh
cmake -B build && cmake --build build
```

Dependencias: Qt5 (Core Gui Widgets Network Sql), OpenGL, libmpv, yt-dlp.

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

## Pruebas de regresión

```sh
./build/nevertube_qt2 --selftest-db        # repos/SQLite
./build/nevertube_qt2 --selftest-player    # hwdec=auto + timer=33ms + render ok
./build/nevertube_qt2 --selftest-innertube # sw.js_data→visitorData→player→hlsManifestUrl
./build/nevertube_qt2 --selftest-discover  # radios guardadas: guardar/recuperar/re-seleccionar/persistencia/descarte
./build/nevertube_qt2 --selftest-av1hls    # HLS sintético AV1 (offline): MAP/BYTERANGE, /seg.mp4, &nt único
```

El selftest del player verifica invariantes del núcleo protegido sin
reproducir ningún archivo. Ejecutar ambos antes de considerar estable un
cambio que toque el reproductor.

## Logging de depuración

El logging `E2E ...` está activado por variable de entorno (no imprime por
defecto, evitando ruido en el bucle de 33 ms):

```sh
NEVERTUBE_DEBUG=1 ./build/nevertube_qt2
```

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
(verificado con `yt-dlp -F`: todo `av01` es `https`/DASH). Además, la URL
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

## Puntos de entrada CLI

- `./build/nevertube_qt2` → app normal
- `./build/nevertube_qt2 --search <consulta>` → buscar al arrancar
- `./build/nevertube_qt2 --channel <channelId>` → abrir canal al arrancar
- `./build/nevertube_qt2 http://...` → reproducir una URL al arrancar
- `./build/nevertube_qt2 --selftest-db` / `--selftest-player` /
  `--selftest-innertube` / `--selftest-discover` / `--selftest-home` /
  `--selftest-feeds` → pruebas

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
- Resolución de streams: Innertube (cliente VISIONOS, `src/yt_innertube.*`) primero y yt-dlp como fallback (`Yt::resolveWithYtDlp`).
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
  estado (guard `m_miniDetached`); la miniatura va POR ENCIMA (`raise()`), el
  vídeo renderiza invisible debajo a 112x63.
- Orden importante: `MainWindow::openWatchAt` cambia a la página Player ANTES
  de activar la cola; si se activa primero, `updateMiniPlayer` ve "reprodu-
  ciendo fuera de Player" y engancha/suelta el mini un frame (parpadeo al
  abrir un vídeo).
- `PlayerContainer` (antes en anónima) vive en `watch_page.cpp` a nivel global
  solo para poder referenciarlo desde `WatchPage` (miembro `m_playerContainer`).