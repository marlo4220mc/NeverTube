# Downloader de video: botón en el Player, pestaña en la barra inferior

**Fecha:** 2026-10-19
**Estado:** hoja de ruta con las decisiones tomadas. **Nada construido todavía.**
**Revisión:** pasó un builder check (un revisor fresco leyó el documento contra
el árbol y devolvió 14 preguntas). Las tres que bloqueaban están contestadas
con evidencia en §3; el resto quedó resuelto en §4 (D11-D17).

---

## 1. La intención

Hoy la app **mira** YouTube. Esta feature la hace **traer** el video: un botón
de descarga en la página del Player y una pestaña "Descargas" en la barra
inferior, con la lista de lo descargado, su progreso, y la posibilidad de
reproducirlo offline dentro de la app.

Por qué ahora: la app ya tiene **toda la maquinaria de red** (Innertube,
adaptiveFormats, master HLS, HLS sintético con `sidx`) y **medio camino de la
infraestructura**: la tabla `downloads` existe en la base desde el principio y
nadie la usa, y el icono `Icon::Download` está dibujado en `icons.cpp` y no se
usa nunca. Falta la pieza de descarga.

## 2. Lo que se midió antes de decidir (no es teoría)

Todo lo de abajo se midió en esta máquina con cuota real, el 2026-10-19.

### 2.1 El throttle de YouTube es un presupuesto POR REQUEST de ~8 MB

| rango pedido | velocidad |
|---|---|
| 2 MB | 5,18 MB/s |
| 4 MB | 5,62 MB/s |
| **8 MB** | **14,30 MB/s** |
| **16 MB** | **0,36 MB/s** |
| 32 MB | 0,37 MB/s |
| **sin `Range` (sostenido)** | **0,36 MB/s desde el primer byte** |

Un request transfiere ~8 MB a plena velocidad y **después** cae. Sin `Range` no
hay tramo rápido. **Conclusión: descargar fragmentado no es una optimización,
es la única forma viable.** El HLS sintético de AV1 ya lo exploitaba
(fragmentos de ~2 MB); el downloader tiene que hacer lo mismo.

### 2.2 La fragmentación rinde, y en paralelo rinde más

Con los 38 segmentos del master 1080p de YouTube:

| estrategia | velocidad |
|---|---|
| sostenida (sin fragmentar) | **0,37 MB/s** |
| fragmentada ×1 | 12,09 MB/s |
| fragmentada ×2 | 26,48 MB/s |
| fragmentada ×4 | 43,99 MB/s |
| **fragmentada ×8** | **60,12 MB/s** |

**160× de diferencia.** (El 1,27 MiB/s que se vio con `yt-dlp` era su
descarga en serie, no el techo.)

**Ojo con la unidad**: esa medición es **a nivel de segmento** (38 URLs del
master, cada uno de ~1,3 MB), no a nivel de rango de bytes. Es la distinción
que resuelve el bloqueador 2 (§3.2).

### 2.3 YouTube entrega el video y el audio POR SEPARADO

`streamingData.formats` (los muxed) **solo ofrece itag 18 = 640x360** en
todos los clientes (ANDROID, IOS, TVHTML5, TVHTML5_SIMPLY, WEB, MWEB). Todo lo
demás viene partido en `adaptiveFormats`.

El master HLS multivariant declara el audio en `EXT-X-MEDIA` (7 entradas) y
`CODECS="avc1.640028,mp4a.40.2"` lo confirma. Pero **el segmento de video es
solo video** (medido con `ffprobe`: `ts_id=1`, un stream) y **el audio va en su
propia media playlist** (`itag/233`). La app **nunca** descarga esa segunda
playlist: `buildFilteredMaster` reescribe el master para mpv, y `HlsServer`
solo sirve archivos `.m3u8` que existen en disco.

### 2.4 El master no tiene AV1; el adaptive sí

| | master | adaptive |
|---|---|---|
| codecs | 9 variantes **VP9**, 7 h264, **ningún AV1** | AV1 hasta **2160p60** (`av01.0.13M.08`), VP9, h264 |
| contenedor | MPEG-TS | mp4 (ISO-BMFF) |
| fragmentado por | YouTube | no (adaptive es un archivo) |
| tamaño 1080p (medido) | 343 MB | 118,62 MB |

El master **sí** trae 4K (variante VP9 3840x2160, 343 MB medidos con
`ffmpeg -c copy` sobre el master completo).

### 2.5 El mp4 del adaptive trae las dos pistas

`yt-dlp` con `-f 399+140` produce un mp4 con `av1,video,1920,1080` +
`aac,audio,44100`. Los itags 394-401 (AV1) son mp4, pero su URL entrega las
pistas separadas.

### 2.6 La unión con ffmpeg es instantánea

`ffmpeg -c copy` sobre 30 s de video + audio: **0,05 s**. Sin recodificar, sin
pérdida. Y `ffmpeg -i <master> -c copy` produce un archivo con **VP9 4K + aac**
(343 MB, 21 s) porque el master trae el audio en `EXT-X-MEDIA`.

### 2.7 El ffmpeg vendoreado **NO tiene muxer** (verificado sobre las DLLs)

El paquete `third_party/ffmpeg-win32/` se compila con `--disable-everything` y
solo demuxers/decoders/parsers/bsf (`tools/build_ffmpeg_i686.sh:70-81`). Su
propio README lo dice: *"no encoders/muxers/avfilter/avdevice"*.

Comprobado sobre los binarios:

| | |
|---|---|
| `avformat_alloc_output_context2` | exportada |
| `avformat_write_header` | exportada |
| **`avformat_write_frame`** | **no exportada** |
| `movflags` / `use_editlist` / `MP4 (MPEG-4 Part 14)` | **ausentes** → no hay muxer MP4 registrado |

Es decir: **el ffmpeg que hay en el bundle puede LEER pero no puede ESCRIBIR
mp4.** "Meter ffmpeg en x64" no es "copiar 5,8 MB de DLLs": es traer un
`ffmpeg.exe` o un build con `--enable-muxer=mov`.

Los bsf que un muxer mp4 necesitaría **sí están** (`aac_adtstoasc`,
`extract_extradata`, `h264_mp4toannexb`).

### 2.8 `mpv --o` no sirve como muxer

Medido: `mpv --o=archivo.mp4 --ovc=copy` **no copia el video** (solo el audio,
`[encode] codec 'copy' not found`), y con `--ovc=libx264` recodifica. En x64
**no hay ruta de muxing sin ffmpeg**.

### 2.9 MPEG-TS no se puede "concatenar" para unir audio

Concatenar v1+a1+v2+a2 deja **un solo stream** (`nb_streams=1`): los dos
flujos quedan en PID distinto y hay que renumerar. Unir TS es multiplexar, no
concatenar. TS **nunca** va a ser la salida final.

### 2.10 Cómo lo resuelve la industria (búsqueda)

`yt-dlp`, `warpdl`, `BBDown-go`, `kkdai/youtube`, `GoTube`: **todos** hacen
`video + audio` → `ffmpeg -c copy`. ffmpeg es la dependencia dura. Los intentos
de evitarla (`puremux` en Go, `mkvgo`) no resuelven MP4→MP4 con dos pistas sin
reescribir el `moov`. `ytv1` (Go puro) declara explícitamente "no adaptive
muxing yet".

**AB Download Manager** (el que sugirió el usuario) se clonó y se leyó: resuelve
bien la **fragmentación** (`splitToRange`, `SparseFile`, `PartSplitSupport` para
dividir una parte que falla) pero **no** la unión: su `assemble()` concatena
bytes y no tiene ffmpeg (grep de 1196 archivos → nada). Su HLS solo acepta
`.ts`. Es un downloader genérico de URLs, no de YouTube.

### 2.11 El transporte de la app es serial y no soporta Range

`Ipv4Https` (`src/yt_innertube.cpp:344`) es **un** `QSslSocket` con una cola
FIFO: `pump()` sale temprano si `m_inFlight`. Su API es
`ResultFn = void(int status, const QByteArray &body)`: **no expone headers de
respuesta** (ni `Content-Range`, ni `Content-Length`) y **no tiene forma de
mandar un `Range`**.

Existe por una razón documentada: *"YouTube blocks certain videos over IPv6
from this network"* (`yt_innertube.cpp:341-343`). El único camino concurrente
que hay en el código es `fetchRangeBlocking` (`src/yt.cpp:956`), que crea un
`QNetworkAccessManager` **local** por llamada y bloquea con un `QEventLoop`
anidado; se usa **una sola vez**, para los ~4 KB del `indexRange`.

**Consecuencia: el downloader necesita su propio transporte.** No puede
reusar `Ipv4Https` (no tiene Range ni headers) y no debe competir con el
`QNetworkAccessManager` del player.

### 2.12 La app no sabe reproducir un archivo local

`PlayerController` solo reproduce **por video id**: `playIndex()` →
`startResolve()` → `Yt::resolveStream(videoId)`. No hay API para un archivo
local. Los dos motores sí lo abrirían (`MpvBackend::playUrl` hace `loadfile`
con la ruta; `FfmpegBackend::playUrl` → `avformat_open_input` con el
protocolo `file`, que el paquete habilita), pero **hay que dejarles llegar
esa ruta**.

### 2.13 El tamaño estimado sale gratis del master

Cada `EXT-X-STREAM-INF` trae `BANDWIDTH`. Con la duración del video:

| | |
|---|---|
| `BANDWIDTH=4688074` × 634,6 s | **354,6 MB estimados** |
| tamaño real medido (`ffmpeg -c copy` sobre el master) | **343 MB** |

**1,7 % de error.** El tamaño se puede mostrar **antes** de empezar, sin un
solo request extra.

### 2.14 La carpeta de descargas se detecta sola

`QStandardPaths::DownloadLocation` (Qt 5.15) da la carpeta de descargas del
sistema, y viene **traducida**:

| | |
|---|---|
| Linux (xfce, medido) | `/home/marlo4220/Descargas` |
| Windows | la "Known Folder" de Descargas del usuario (`%USERPROFILE%\Downloads`, o la de OneDrive si está redirigida) |

Comprobado que existe en el Qt 5.15 del bundle de Windows (los dos vendors:
`qt-win64` y `qt-win86` lo declaran en `qstandardpaths.h:72`), o sea que
funciona igual en los dos builds sin código extra.

## 3. Los tres bloqueadores, contestados

### 3.1 ¿Qué es "ffmpeg"?

**No** son las DLLs actuales: no tienen muxer (§2.7). Las opciones reales:

| opción | qué es | costo | veredicto |
|---|---|---|---|
| **(a) `ffmpeg.exe` con muxer** | un binario CLI (BtbN lgpl, o un build propio) | el zip de BtbN son ~35 MB; un build propio con `--enable-muxer=mov` es menos | simple; no toca el CMake de x86 |
| **(b) libavformat con muxer, en proceso** | recompilar el paquete con `--enable-muxer=mov` y linkearlo | el `avformat` crece (0,71 → varios MB) y hay que rehacer i686 **y** x64 | más limpio, más trabajo |
| **(c) la app escribe el mp4** | escribir el `moov` a mano | el muxer mov de ffmpeg son ~5800 líneas | cero dependencias, pero no trivial |

**Decisión: (a) `ffmpeg.exe`**, y por razones concretas:

- Es **un solo camino de código** para los dos builds: la app lo invoca con
  `-i video.ts -i audio.ts -c copy salida.mp4`.
- **No toca el código protegido ni el CMake de x86**: `FfmpegBackend` sigue
  igual.
- Se **busca en el sistema** al vuelo (`QStandardPaths::findExecutable`), así
  que el downloader **no exige ffmpeg como dependencia para existir** (ver D12).
- La licencia es LGPL, como el resto de lo que ya se distribuye.

**Consecuencia**: el paso 5 de la implementación es "traer el binario" (o
"buscarlo"), no "recompilar ffmpeg".

### 3.2 ¿Cuál es la unidad de descarga?

El builder check encontró una contradicción real: §2.2 mide **segmentos**
(38 URLs), pero el diseño hablaba de `splitToRange` sobre "el tamaño total" de
un recurso, que **no existe** en HLS (hay N URLs de segmentos, cada una con su
propio tamaño).

**La unidad es el SEGMENTO del master.**

- El master ya está fragmentado por YouTube, con segmentos de ~1,3 MB
  (medido) — muy por debajo del techo de 8 MB, así que **cada segmento entra
  en un request sin throttle** (§2.1).
- El paralelismo es **entre segmentos**: 8 conexiones bajando 8 segmentos a la
  vez. Es lo que midió 60 MB/s (§2.2).
- **No hace falta `splitToRange`**: no hay un archivo gigante que partir.

Consecuencias:

- **El `Range` no es necesario** para los segmentos (cada uno es un request
  completo). El `Range` sigue haciendo falta para el **adaptive** (fase 2),
  que es un archivo único de 118 MB.
- **El transporte sigue necesitando ser propio**: aunque no haga falta
  `Range`, hace falta **N conexiones simultáneas** y leer los **headers**
  (`Content-Length` de cada segmento). `Ipv4Https` no da ninguno de los dos.
- **La reanudación es por segmento**: el estado es "qué segmentos están
  bajados", un conjunto de índices, no un rango global.

### 3.3 ¿Dónde viven los bytes parciales?

La contradicción era: D6 decía "temporal", §5.1 decía "escritura dispersa en
el archivo final", y §5.2 decía que `file_path` guarda lo parcial.

**Son dos archivos, y la tabla guarda los dos:**

| | |
|---|---|
| `file_path` | el **parcial** (un `.ts` por pista, con prefijo del id) |
| `final_path` | el **resultado** (el mp4 final) |

El downloader escribe **disperso** en el parcial (como el `SparseFile` de AB:
el archivo se crea vacío y cada segmento se escribe en su offset, así un
archivo de 300 MB no se reserva de golpe y una descarga a medias conserva lo
bajado). Al final, ffmpeg lee los parciales y escribe el mp4.

**La tabla `downloads` se extiende**, porque el esquema actual no puede
sostener la reanudación ni la posición local (D16):

```sql
ALTER TABLE downloads ADD COLUMN final_path TEXT;        -- el mp4 terminado
ALTER TABLE downloads ADD COLUMN bytes_done INTEGER;      -- bytes bajados
ALTER TABLE downloads ADD COLUMN bytes_total INTEGER;     -- tamaño total
ALTER TABLE downloads ADD COLUMN quality TEXT;            -- "1080p", "vp9", ...
ALTER TABLE downloads ADD COLUMN parts TEXT;              -- JSON: índices de segmentos hechos
ALTER TABLE downloads ADD COLUMN position_ms INTEGER;     -- posición de reproducción local
ALTER TABLE downloads ADD COLUMN updated_at INTEGER;
```

`id` sigue siendo **el video id** (una descarga por video: un segundo clic
muestra el estado, no reencola — §6.4). `type` = `"video"`. `status` es un
vocabulario **fijo en código, nunca texto traducido** (regla de AGENTS.md):
`pending`, `downloading`, `paused`, `muxing`, `done`, `error`.

## 4. Decisiones

| # | Decisión | Por qué |
|---|---|---|
| D1 | **Fragmentado siempre**, y **en paralelo** | el throttle es por request; 160× de diferencia (§2.1, §2.2) |
| D2 | Un **archivo mp4 con video y audio** | lo pidió el usuario; es lo que se abre en VLC |
| D3 | La fuente es el **master HLS** | trae video **y** audio, y viene fragmentado por YouTube (§2.3, §2.6) |
| D4 | La **unidad de descarga es el segmento** | el master ya está fragmentado; no hay recurso gigante que partir (§3.2) |
| D5 | Los parciales van a **archivos dispersos**; el mp4 final se genera al final | permite reanudar y no reserva el archivo entero (§3.3) |
| D6 | La tabla `downloads` **se extiende** con 7 columnas | el esquema actual no puede sostener la reanudación ni la posición (§3.3) |
| D7 | El downloader tiene **su propio transporte** | `Ipv4Https` es serial y no tiene Range ni headers (§2.11) |
| D8 | El **adaptive (AV1) queda para la fase 2** | es la única fuente de AV1, pero no trae audio unido y su URL es un archivo único (§2.4) |
| D9 | ffmpeg **se busca en el sistema**, no se embebe | la app ya linkea libavformat en x86 pero sin muxer; el binario no obliga a reconstruir nada (§3.1) |
| D10 | La app reproduce el archivo con el motor que ya tiene | falta API en `PlayerController` (§2.12) |
| **D11** | **El muxing lo hace `ffmpeg.exe` del sistema**, invocado con `-c copy` sobre los dos parciales | un solo camino para los dos builds; no toca el CMake de x86 ni la pieza protegida (§3.1) |
| **D12** | **Sin ffmpeg, el downloader sigue funcionando**: los segmentos se bajan y se conservan, y el mux se reintenta cuando aparezca ffmpeg. Antes de empezar, el botón **avisa** que falta ffmpeg y ofrece continuar o cancelar | no se pierde trabajo; el usuario decide con información. Descargar sin ffmpeg NO es inútil: los `.ts` quedan y el mux es un paso aparte |
| **D13** | La carpeta de descargas es **`QStandardPaths::DownloadLocation`**, con un subdirectorio `NeverTube` dentro, y **se puede cambiar en Ajustes** | el sistema ya sabe cuál es la carpeta de descargas del usuario, y en Linux viene traducida (`Descargas`, `Downloads`) (§2.14). El subdirectorio `NeverTube` evita mezclarse con los archivos de la persona. La ruta **se muestra** en la UI y se puede abrir con "Abrir carpeta" |
| **D14** | **Solo se ofrece lo que el master tiene** (VP9/h264 hasta 4K), y la UI lo dice: "YouTube no publica AV1 en su master" | en un perfil `av1-hw` lo que se ve en pantalla es AV1 del adaptive, y el master no lo tiene (§2.4). Prometer AV1 en la fase 1 sería falso. La AV1 llega en la fase 2, con su propio aviso |
| **D15** | **Una descarga a la vez**, y el player **no** se pausa automáticamente | el throttle es por request y 8 conexiones + el player se comen el ancho de banda; el gate 403 de AGENTS.md es un riesgo real. Pausar el player sin avisar sería una sorpresa. Si el usuario abre un video mientras descarga, ambos conviven a medias de ancho de banda |
| **D16** | La reproducción local **no entra en el historial**: su posición se guarda en `downloads.position_ms` | `HistoryRepo` y `onDurationKnown` asumen un video de YouTube (escriben la duración en la caché `videos`, que para un archivo local no corresponde). Guardarlo en la fila de la descarga mantiene el historial limpio y no toca `PlayerController` más de lo necesario |
| **D17** | **El tamaño se muestra antes de empezar**, y sale gratis: `BANDWIDTH` × duración | error medido: 354,6 MB estimados vs 343 MB reales = **1,7 %** (§2.13). Sin un request extra |

## 5. Lo que NO es el objetivo

- **No** es un reemplazo de yt-dlp: sin subtítulos, capítulos, playlists,
  metadatos ni `--download-sections`.
- **No** se toca la pieza protegida del player (mpv/render/frameReady).
- **No** se implementa el adaptive (AV1) en esta fase (D8, D14).
- **No** se toca ningún workflow sin aprobación.
- **No** se descarga en vivo (los live no tienen duración fija).
- **No** se reproducen dos descargas a la vez (D15).

## 6. Arquitectura

```
 Player (botón Descargar)
      │
      ▼
 DownloadManager  (QObject, hilo principal)
      ├── DownloadJob  (uno por descarga)
      │     ├── DownloadTransport   (propio: N sockets, headers, IPv4 forzado)
      │     ├── baja los segmentos del master (video) y del EXT-X-MEDIA (audio)
      │     │     en paralelo, escribiendo disperso en los .ts parciales
      │     └── al final: ffmpeg -c copy -> el mp4
      ├── DownloadRepo  (tabla `downloads`, extendida)
      └── emite changed() -> la página se repinta
```

### 6.1 El `DownloadJob`

1. **Resolver**: `Yt` pide el master HLS (`hlsManifestUrl`). Se elige la
   variante con el mismo criterio que hoy usa el player (`codecRankedScore`
   sobre la línea `STREAM-INF` + altura máxima), y de ella se saca la lista
   de segmentos de video. Del `EXT-X-MEDIA` se elige la pista de audio (por
   idioma, como `selectAudioTracks` pero **conservando la URI**, que hoy se
   descarta) y se descarga **esa** media playlist, que la app nunca ha bajado.
   El tamaño estimado sale de `BANDWIDTH` × duración (D17).
2. **Bajar**: los segmentos de video y de audio, en paralelo (4 u 8
   conexiones), cada uno a su archivo parcial, escritos en su offset. El
   progreso es `bytesDone / bytesTotal`.
3. **Unir**: con todo descargado, `ffmpeg -i video.ts -i audio.ts -c copy
   salida.mp4`. Si ffmpeg no está, la descarga queda en `error` **con los
   parciales intactos** y el mux se reintenta después (D12).
4. **Reportar**: `progress` a la base y a la UI.

### 6.2 Reanudación y fallo

- Un segmento que falla se reintenta hasta 3 veces; si insiste, la descarga
  queda en error y conserva lo bajado.
- Al reabrir la app, `parts` (el JSON de índices) dice qué segmentos están:
  se sigue con los que faltan. **La unidad es el segmento**, así que no hay
  que mapear offsets.
- Si la URL expiró: se re-resuelve el master y **se vuelve a derivar la lista
  de segmentos**. Los índices de los segmentos ya bajados siguen valiendo
  **si el master no cambió de layout**; si cambió, se reinicia la descarga.
  (La re-resolución cambia las URLs firmadas pero no el contenido: el layout
  de segmentos es estable para un mismo video.)

### 6.3 La página de Descargas

Página nueva (`src/ui/pages/downloads_page.*`), con el mismo patrón que las
otras: `QStackedWidget` en `MainWindow`, un botón más en la barra inferior
(`nav.downloads`), entrada en el `QButtonGroup`.

- **La página se AGREGA AL FINAL** de `m_pages` (índice 8) y el grupo de la
  barra toma el id 4. Esto es lo seguro: `showPreviousTab()` tiene un mapeo
  inverso hardcodeado (`mainwindow.cpp:1769`) y hay un chequeo de "está
  mirando un video" con `currentIndex() == 3` (`mainwindow.cpp:1403`), y unas
  dozen de asignaciones de `m_currentTab`. Insertar en medio rompería todo
  eso; **agregar al final no toca nada**.
- Lista con: miniatura, título, canal, **barra de progreso**, estado
  (descargando / pausado / listo / error) y acciones (reintentar, cancelar,
  reproducir, borrar, abrir carpeta).
- Empty state con `nt::emptyStateHtml` (el logo), como las otras páginas.
- La barra inferior pasa de 4 a 5 botones. La cápsula es de **ancho fijo
  (104 px)** con etiqueta de 11 px: "Descargas" (9 caracteres) es la misma
  longitud que "Biblioteca", que ya cabe. **Ojo**: el orden visual es el de
  `m_navButtons`; el id del grupo no tiene que coincidir con el índice de
  página.
- **La carpeta** sale de `QStandardPaths::DownloadLocation` + `/NeverTube`
  (D13). Si el sistema no la tiene, se cae a `DocumentsLocation` y, si
  tampoco, a `HomeLocation`.

### 6.3.1 El ajuste de la carpeta

Es un valor de `SettingsRepo` (clave `download_dir`), vacío por defecto
(cuya lectura significa "usar el valor del sistema"). La página de Ajustes
gana una fila con un botón que abre un `QFileDialog::getExistingDirectory`.

Dos detalles de la app que hay que respetar:

- `SettingsPage` se **reconstruye** en cada cambio de idioma, así que el
  valor tiene que volver a leerse de la base (no quedar en una variable de la
  UI).
- El texto del botón con la ruta es **dato, no cromo**: se compone con
  `nt::tr` y un `%1`, no con un `bindText` de ruta fija.

**Es la primera vez que la app usa `QFileDialog`**: no hay ninguno en el
código (`grep QFileDialog` → nada), así que hay que comprobar que el diálogo
hereda el tema oscuro. Si no, se usa un `QFileDialog` con la hoja de estilo
de la app aplicada al diálogo, o un `QFileDialog` nativo con
`QFileDialog::DontUseNativeDialog` + las reglas de QSS que apliquen.

### 6.4 El botón en el Player

`WatchPage` ya tiene la fila de acciones (`makeAction`): Compartir, Favorito,
Ver más tarde, Añadir a playlist. El botón de descarga entra ahí como
**quinto `#actionChip`** con `nt::Icon::Download` (ya existe, sin usar), con
tooltip **y** nombre accesible atados a la misma clave i18n (`makeAction` hace
las dos cosas).

Al pulsarlo: se encola la descarga y **el botón cambia de estado** para
mostrar "Descargando…" / "✓ Descargado" / "Reintentar". **No hay sistema de
toast en la app** (no existe: `nt::confirm`/`nt::inputText` son para
confirmar y para pedir texto, y AGENTS.md prohíbe montar un `QFrame#popupDialog`
por página), así que el aviso es **el propio botón** y la **pestaña de
Descargas**.

Si ffmpeg no está, el botón avisa **antes** de empezar y ofrece continuar o
cancelar (D12).

### 6.5 Reproducir lo descargado

Falta API. Lo más limpio: un `PlayerController::playLocalFile(path, info)` que
**no** pasa por el resolver y llama a `m_player->playUrl(path)`. Como la cola
es `QVector<VideoInfo>` y `playIndex` siempre resuelve, hace falta **un
concepto de entrada de cola local**.

La posición se guarda en `downloads.position_ms` (D16), no en el historial, así
que no se toca `HistoryRepo` ni `onDurationKnown`.

### 6.6 i18n

Claves nuevas en `es.json` y `en.json` (mismo conjunto de claves: es lo que
exige `--selftest-i18n`):

- `nav.downloads`
- `watch.download` (tooltip **y** nombre accesible del botón)
- `downloads.*`: `title`, `empty`, `pending`, `downloading`, `paused`, `muxing`,
  `done`, `error`, `retry`, `cancel`, `delete`, `play`, `openFolder`, `quality`,
  `size`, `speed`, `noFfmpeg`, `downloadedTo`, `av1Unavailable`
- `settings.downloadFolder` (la etiqueta del ajuste) y `settings.browseFolder`
  (el botón)

Cada clave usada tiene que existir **en el idioma base** (`es.json`), porque
`--selftest-i18n` falla duro si una clave usada en el código no está en la
base, y los catálogos tienen que tener el mismo conjunto de claves.

## 7. Cambios por archivo

| Archivo | Qué |
|---|---|
| `src/downloader/download_manager.{h,cpp}` | **nuevo**: orquesta las descargas y el pool de segmentos |
| `src/downloader/download_job.{h,cpp}` | **nuevo**: una descarga (resolver, bajar, muxear) |
| `src/downloader/download_transport.{h,cpp}` | **nuevo**: transporte propio (N conexiones, headers, IPv4) |
| `src/downloader/segment_plan.{h,cpp}` | **nuevo**: elegir variante del master + pista de audio, y planear los segmentos. Función pura → selftest offline |
| `src/downloader/muxer.{h,cpp}` | **nuevo**: encontrar ffmpeg e invocarlo con `-c copy` |
| `src/db/repositories.{h,cpp}` | `DownloadRepo` + las 7 columnas nuevas |
| `src/db/app_database.cpp` | las migraciones `ALTER TABLE` |
| `src/ui/pages/downloads_page.{h,cpp}` | **nuevo**: la página de la pestaña |
| `src/ui/pages/settings_page.cpp` | la fila de la carpeta de descargas (D13) |
| `src/ui/pages/watch_page.cpp` | el botón de descarga y su estado |
| `src/mainwindow.{h,cpp}` | la página (índice 8), el botón de la barra (id 4) |
| `src/main.cpp` | el flag del selftest nuevo |
| `src/ui/theme.cpp` | los ids nuevos del QSS de la página (**si** se estiliza con ids propios) |
| `src/player_controller.{h,cpp}` | `playLocalFile` (§6.5) |
| `resources/nav/*.png` + `resources/resources.qrc` | los sprites del botón nuevo |
| `tools/gen_nav_accent.cpp` | la clave "downloads" en su lista de `keys` |
| `localization/{es,en}.json` | las claves nuevas |
| `CMakeLists.txt` | los .cpp nuevos |
| los jobs `windows-win7-x86` y `windows-win10-x64` de `.github/workflows/release.yml` | el selftest nuevo en el smoke |
| `packaging/linux/{deb,rpm,arch}` | el selftest nuevo en el smoke |

### 7.1 Los sprites de la barra (un detalle que se pasó por alto)

`NavButton::updateIcon()` busca **seis** estados: `inactive`, `disabled`,
`pressed`, `hover`, `active`, `circular`. El generador
(`tools/gen_nav_accent.cpp`) solo produce **tres** (`active`, `hover`,
`pressed`) × 7 acentos = 21 archivos por clave; los otros tres se commitean a
mano. Para la clave nueva hay que producir **los seis estados**, y **alguien
tiene que dibujar el PNG base** (el generador solo re-tiñe, no dibuja), en el
mismo lenguaje visual y con el rojo nativo `#FF0033`.

**Y hace falta un gate**: hoy no hay ningún selftest que compruebe que existan
todos los sprites de la barra. Si falta uno, Qt pinta un icono vacío y el
agujero solo se ve en la máquina. Va como un check nuevo en `--selftest-theme`
(que ya existe y ya vigila los ids del QSS).

## 8. Verificación

| Qué | Cómo |
|---|---|
| La fragmentación no se throttlea | medir velocidad con 4 y 8 conexiones; por encima de 10 MB/s |
| La unión sale bien | `ffprobe` del resultado: 1 video + 1 audio, duraciones que coinciden |
| La app reproduce el archivo | abrir el mp4 con el motor real; es una prueba **de máquina, no de CI** |
| El gate offline sigue verde | `--selftest-db --selftest-theme --selftest-glyphs --selftest-i18n --selftest-dates --selftest-grid --selftest-discover --selftest-ssl` |
| El selftest **nuevo** del downloader | `--selftest-download` (a crear): el plan de segmentos es una función pura → casos offline |
| La página nueva no rompe la barra | `--selftest-theme` (ids nuevos) + el **gate nuevo de los sprites** (§7.1) |
| i18n completo | `--selftest-i18n` |
| Reanudación | pausar a la mitad, cerrar la app, reabrir, seguir |
| El tamaño estimado es honesto | `BANDWIDTH` × duración contra el tamaño real (medido: 1,7 %) |
| La carpeta de descargas se detecta | `DownloadLocation` en Linux (`Descargas`) y en Windows (Known Folder); el fallback a `DocumentsLocation` si no existe |

**`--selftest-av1hls` no se toca**: valida el HLS sintético de AV1, que es la
ruta del *adaptive*, y el downloader de la fase 1 no lo usa (D8).

**Los selftests online y los de GPU quedan fuera del gate**, como ya está
establecido: un runner sin GPU no puede ejecutarlos.

## 9. Orden de implementación

1. **`segment_plan`** (puro): elegir variante del master + pista de audio +
   plan de segmentos. Con su selftest offline. **No toca red.**
2. **`DownloadRepo` + las migraciones**: la base primero, que es lo que
   permite mostrar la lista vacía y persistir.
3. **`DownloadTransport`**: N conexiones, headers, IPv4. Es la pieza que
   más riesgo tiene (es donde está el IPv6 y el paralelismo).
4. **`DownloadJob`**: bajar en paralelo, escribir disperso, progreso.
   **Medir la velocidad real contra YouTube** antes de seguir.
5. **`muxer`**: encontrar ffmpeg y llamarlo con `-c copy` sobre los dos
   parciales.
6. **La página de Descargas** + el botón de la barra (con los sprites y el
   gate nuevo).
7. **El botón del Player** + i18n.
8. **La reproducción local** (`PlayerController::playLocalFile`).

Los pasos 1-5 no dependen de la UI: se pueden probar desde el binario antes de
tocar un solo widget. **El paso 3 es el que puede cambiar el diseño**: si el
transporte propio no rendisce, hay que revisar el modelo de conexiones.

**Fase 2** (fuera de esta hoja): el adaptive (AV1 y cualquier codec del
`adaptiveFormats`), que es un archivo único de 118 MB y necesita `Range` de
verdad, y con él la possibility de bajar AV1 (D8, D14).

## 10. Riesgos

| Riesgo | Mitigación |
|---|---|
| YouTube cambia el master y rompe el parser | el parser del master ya existe en la app (`buildFilteredMaster`); se reusa, no se reescribe |
| ffmpeg no está o falla | los segmentos bajados **se conservan** y el mux se reintenta (D12); el botón avisa antes de empezar |
| El paralelismo se ve como abuso | 4-8 conexiones es lo que hacen IDM y los demás; y **una descarga a la vez** (D15) |
| Un archivo de 300 MB en una partición llena | escritura dispersa: solo ocupa lo descargado |
| La URL expira a mitad | se re-resuelve y se sigue con los segmentos ya bajados (§6.2) |
| La sesión 403 (gate) durante la descarga | es el mismo riesgo que el player; `Yt::rotateStreamSession()` existe para eso |
| El transporte propio choca con el IPv6 | `Ipv4Https` existe por eso; el transporte del downloader debe forzar IPv4 igual |
| El usuario borra ffmpeg después de descargar | el mp4 ya está hecho y se reproduce igual; solo affects a descargas nuevas |
