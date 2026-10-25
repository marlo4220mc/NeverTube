# nevertube_qt2

Lightweight YouTube client in Qt5 (C++17 / CMake). Playback core
based on a proven POC. Philosophy: lightweight before complete, play
before decorate, stability and low consumption.

## Build

```sh
cmake -B build && cmake --build build
```

Dependencies: Qt5 (Core Gui Widgets Network Sql), OpenGL, libmpv.

### Windows x86 build (32-bit Win7, dual engine)

The app has **two engines** (`src/playback_backend.h`): `MpvBackend` (mpv, Linux and Windows **x64**, protected piece untouched) and `FfmpegBackend` (FFmpeg/libav without libmpv, the **only** engine of the Windows **x86** build — `NEVERTUBE_X86_FFMPEG_BUILD`). There is no public FFmpeg build for Win32 (gyan/BtbN only win64, MSYS2 dropped i686): the i686 dev-package is cross-compiled once and is **vendored** in `third_party/ffmpeg-win32/` (LGPL v2.1+, schannel, DXVA2 h264, Win7-clean imports, no libwinpthread/libgcc). Reproducible recipe: `tools/build_ffmpeg_i686.sh`. CMake resolves `FFMPEG_DEV_DIR` by default to the vendored package. On x86 the app uses `Qt5 Multimedia` (`QAudioOutput`) for audio and the codec profile is pinned to `h264-hw` (DXVA2 ceiling) without `HwProbe` (guards `NEVERTUBE_NO_MPV` in hwprobe/mainwindow/settings/factory). On x64/Linux everything stays as it was (mpv). Details and verification: `third_party/ffmpeg-win32/README.md` and `BUILD_WINDOWS.md`.

The `FfmpegBackend` decides whether to attach the `hw_device_ctx` to a decoder with `codecSupportsHw` (`avcodec_get_hw_config`): ONLY if the codec has a hwaccel compiled for that device (the i686 package only ships `h264_dxva2`). It is mandatory not to attach it to AV1: with a device attached, `ff_get_format` is invoked and, without an av1 hwaccel, FFmpeg 9's native decoder aborts the whole clip with `ENOSYS` ("Your platform doesn't support hardware accelerated AV1 decoding", av1dec.c). Warning (hard rule): FFmpeg 9's NATIVE `av1` decoder **does not decode AV1 in software under any circumstance** (the code itself says "the av1 decoder doesn't support native decode"; with or without a device). FFmpeg's software AV1 is provided by `libdav1d`/`libaom`, which the i686 package does NOT ship → **on x86 AV1 is impossible** (neither HW nor SW); the x86 build pins the `h264-hw` profile and never asks for AV1, so it doesn't affect playback. That is why `--selftest-ffmpeg` skips the av1 clip when there is no sw AV1 decoder (`av1SoftwareDecodable`): on x86 it skips; on Linux/x64 with libdav1d it is still verified (there `hw=no` is libdav1d in software, not the native one).

**Open HW software fallback**: `codecSupportsHw` only reflects what is COMPILED (a hwaccel exists in the package), not what the hardware supports. `av_hwdevice_ctx_create` (DXVA2 on Windows) usually works even if the GPU doesn't decode h264 in hardware (the real check happens when opening the decoder). If `avcodec_open2` with a device attached FAILS — e.g. a platform without h264 DXVA2 — `openDecoder` returns `nullptr` and `runLoop` retries in PURE SOFTWARE before reporting "Sin decodificador de vídeo" (fatal error). Without that retry, on those machines every h264 clip would die with an error instead of falling back to SW (seen on the x86 test machine). With the retry, the first frame comes out in software → hwdec="no" → the reactive ladder lowers `h264-hw → soft-all` (ephemeral) and re-resolves.

## MSVC string literal limit (C2026): the QSS is split

**MSVC does not allow a string literal longer than 16380 bytes** and warns with `error C2026: string too big, trailing characters truncated`. GCC has no such limit, so a block that compiles cleanly on Linux may fail to compile on Windows without anything locally giving it away.

The real case: `nt::styleSheet()` (`src/ui/theme.cpp`) had the entire QSS — 30 627 bytes, 624 lines — in a single raw literal `R"QSS(…)QSS"`. The x86 build uses **toolset v142** (VS2019, mandatory for Win7) and it blew up there; x64 with the `windows-latest` toolset accepted it, so it was **only visible on x86**. C2026 is triggered by the lexer while scanning the literal, so the fix has to be in the literal, not in a conditional compilation.

The QSS now lives in `static const char *const kTrozos[]` (five chunks, the largest 8359 bytes) that `styleSheet()` joins with `QString::fromUtf8`. Rules:

- **Do NOT put the QSS back into a single literal.** If it grows, another chunk is added to the array.
- The split always falls **between rules** (on a blank line), never inside one, and the result is the same qss as before, byte for byte.
- It is joined with `fromUtf8` and not with `QStringLiteral` on purpose: that way the runtime decodes UTF-8 and it doesn't depend on the charset with which MSVC read the file (even though `/utf-8` is still set in CMakeLists).
- It has been checked that no literal longer than 16380 bytes remains in `src/`, and `--selftest-theme` still parses the whole sheet and validates it the same way.

To detect it without waiting for CI, just look at the size of the raw literals: any `R"…(` over 16 KB is a C2026 on x86.

### Preprocessor directives are NOT allowed inside a macro invocation (C2121)

Same family of trap, and it bit the same way: **MSVC rejects a `#if`/`#ifdef`
placed inside the arguments of a MACRO INVOCATION** (`error C2121: '#': invalid
character: possibly the result of a macro expansion`, plus a cascade of
`C2065 'ifdef': undeclared identifier` and `C2143`/`C2059`), while **GCC accepts
it**. Inside the parentheses of a normal function or of a type constructor it is
fine in both compilers — in `src/ffmpeg_backend.cpp` there is a
`QLatin1String(\n#if defined(Q_OS_WIN)\n"dxva2"\n#else\n"vaapi"\n#endif\n)` that
has always compiled on x86. The rule applies to MACROS only.

The real case: the guards added to compile the `FfmpegBackend` against FFmpeg 4.4
(libavutil 56 has no `AVChannelLayout`) were put inside the body of `NT_DEBUG(…)`,
which IS a macro. Every Linux build and the x64 Windows build stayed green and
**only the x86 Windows job failed**, for two compounding reasons: (1) the guard
sat inside a macro call, and (2) x86 is the ONLY build that compiles
`ffmpeg_backend.cpp` against libav at all (`NEVERTUBE_X86_FFMPEG_BUILD`; x64
links mpv, so those lines are not even preprocessed there). A change in `src/`
does not trigger the Windows workflows (their `paths:` filter is the workflow
file itself), so it was latent until a workflow edit ran them.

The fix is to hoist the value into a LOCAL before the call:

```cpp
// NOT: NT_DEBUG("...", ac->sample_rate,
//        #ifdef X
//          a,
//        #else
//          b,
//        #endif
//        tail);
#ifdef NT_FFMPEG_NEW_CHLAYOUT
    const int acChannels = ac->ch_layout.nb_channels;
#else
    const int acChannels = ac->channels;
#endif
    NT_DEBUG("E2E ffmpeg audio %dHz ch=%d src=%s\n", ac->sample_rate, acChannels,
             extAudio ? "externa" : "muxed");
```

To detect it without waiting for the x86 job, scan `src/` for a preprocessor
directive whose enclosing parenthesis belongs to an ALL-CAPS identifier (that is,
a macro). A directive inside `foo(...)` is fine; inside `FOO(...)` it is a C2121
on MSVC.

## App icon

The app has an icon on four surfaces, all from the same files:

| surface | mechanism |
|---|---|
| Windows `.exe` | `resources/nt.rc` (`IDI_ICON1 ICON DISCARDABLE "icon/nt.ico"`), added with `target_sources` inside the `if(WIN32)` of CMakeLists; rc.exe resolves the path with the `target_include_directories` to `resources/` |
| Taskbar / Explorer | multires `nt.ico` via `IconFilename` in the shortcuts of `dist.iss` and `dist64.iss` |
| The `setup.exe` itself | `SetupIconFile` in the two `.iss` |
| Window (Qt) | `app.setWindowIcon()` in `main.cpp` with the 7 PNGs from the `.qrc` |
| Linux menu / launcher | `packaging/linux/nevertube_qt2.desktop` + the 7 PNGs installed as `nevertube_qt2.png` in the `hicolor` icon theme by the `install()` rules of CMakeLists |
| Linux taskbar / Alt-Tab | `app.setWindowIcon()` (X11 `_NET_WM_ICON`) and `QGuiApplication::setDesktopFileName("nevertube_qt2.desktop")` in `main.cpp`, which is what Wayland/GNOME/KDE use to match the window with the `.desktop` |

- The CURRENT icon is the one the user chose (a bright monitor with a red play button, matching the default accent). It came from a PNG of the mockup with the **"transparency" checkerboard PAINTED** into the pixels: it was removed with a flood fill from the border (connected light grays) and the RGB of the transparent area was filled in so that rescaling doesn't leave a white halo. The 1024 master is the only thing touched by hand; the rest is regenerated by the script. There is NO need to "adapt" the icon to the app's style: the icon rules.
- The files live in `resources/icon/` and are generated by `tools/gen_app_icon.py` from the master `nt_icon.png` (1024, real alpha). The generator **fails loudly** if the master loses transparency or if the `.ico` is missing any entry, so that a future change doesn't leave a single-entry `.ico` (blurry taskbar) without anything noticing.
- **The generator NORMALIZES the framing** (`normalizar()`): it trims the transparent margin and rescales the motif until its larger dimension fills the canvas, centered. It is not cosmetic: the master had the tile at **89.7 % × 85.4 %** and shifted up (margins L53 T55 R52 B95), so next to the other system icons —Chrome, Drawio, Basecamp and company measure ~**100 % × 100 %** in `hicolor`— the app icon read as "smaller" in the taskbar, the menus and Alt-Tab. After normalizing it measures **100 % × 95.1 %** (the tile is ~5 % wider than tall in the art, and the aspect is preserved by scaling the larger side). The rule: an app icon fills its canvas; do not leave a painted margin. The master is rewritten by `python3 tools/gen_app_icon.py --cuadrar` (already normalized, so the operation is idempotent).
- The heights are 16/24/32/48/64/128/256 because **QIcon does not scale**: it picks the entry closest to what each surface asks for. The 16 one is the taskbar at 100 % and the most demanding: at that size the motif must still be readable (in the previous icon, a 1 px contour bevel disappeared: measured, 1/16 of the canvas = 0.33 px).
- `nt.ico` is in the `$need` list of the iscc manifest gate of BOTH setups (`windows.yml` x86 and `windows_x64.yml`): if an edit to `[Files]` leaves it out, CI says so instead of creating the shortcuts with Windows' default icon.
- **On Linux there is no `.rc`**: the icon reaches the desktop in two independent ways, because it depends on the session. (1) The MENU/launcher entry and the icon the launcher looks up by name come from `packaging/linux/nevertube_qt2.desktop` (`Icon=nevertube_qt2`) and the PNGs installed in `share/icons/hicolor/<N>x<N>/apps/nevertube_qt2.png`; `hicolor` is the fallback theme EVERY desktop environment reads (GNOME, KDE, XFCE, Cinnamon, MATE, ...), so there is no per-DE file. (2) The WINDOW icon (taskbar, Alt-Tab, title bar) comes from `app.setWindowIcon()`: on X11 it is published as `_NET_WM_ICON` and any WM uses it; on Wayland the compositor ignores it and uses the `app_id`, which is why `main.cpp` also calls `QGuiApplication::setDesktopFileName("nevertube_qt2.desktop")` (without it GNOME/KDE show a generic icon). Both are installed with `cmake --install` / `cmake --install build --prefix ~/.local` (override the prefix to reach `/usr`); there is no Linux installer, so the app only gets the menu entry once it is installed (running from `build/` shows the window icon but no menu entry).
- **Brand material (matching the icon)**: the accent CTAs (`#primary` —Buscar/Reproducir/OK—, `#accentChip` —"Ir a Buscar"— and `#subscribe`) use a FLAT accent fill (modern, no 3D gloss) with a 1 px white hairline at ~15 % (`rgba(255,255,255,38)`) so they read against the dark background, and crisp `@ACCENT_HOVER@`/`@ACCENT_PRESSED@` states. It works for all 7 Ajustes colors because everything is derived from the accent. The app logo appears in the empty states (`nt::appLogo` / `nt::emptyStateHtml`, above the text) and in the Home one. The FUNCTIONAL UI (player row, lists, cards, nav) stays FLAT: the material is only for branding; do not smear gradients over the whole app.

## Protected piece (do NOT modify without technical reason and proof)

The functional player is a protected piece. The following elements must NOT be touched without a technical reason backed by a concrete test:

- `QOpenGLWidget` (class `PlayerWidget`) as the video surface
- **Event-driven redraw**: the engine notifies `frameReady` when there is a new frame (mpv update callback installed in `MpvBackend::initGL` / `frameReady` per frame from `FfmpegBackend`) and `PlayerWidget::onFrameReady` forces `QWidget::update()`. There is NO poll timer: at rest (paused, buffering, covered) nothing is repainted. It replaced the old 33 ms `QTimer`, which became redundant once the event path existed (verified: real playback + overlay shown/auto-hidden with screenshots, selftest-player).
- **The stall watchdog (1 s) is only armed while the engine plays** (`PlayerWidget::updateWatchdogTimer`, called from `stateChanged` and from `setPaused`). At rest `checkStall()` returned on its first line, so the timer was a wakeup per second for the whole life of the process (the player is created at startup and then lives in the mini bar). Measured at rest: the voluntary context switches of the process went from 48 to 28 per 20 s. Do NOT start it in the constructor again: `onStreamStart` emits `stateChanged`, which is what arms it.
- `vo=libmpv` + `hwdec=auto` (works with and without hardware acceleration)
- Direct rendering to FBO (`mpv_render_context_render`) in `paintGL`
- Any change to the mpv integration (libmpv / mpv_render_context)

Context: the first attempt to separate the multimedia controls (floating overlay over the video) broke the app and was reverted. Make UI changes that touch the player in small increments, verifying each one.

## Grid layout (videos) and "Tamaño de las miniaturas"

The Home grid keeps a **FIXED number of columns** at any window width (5 by
default; 4 or 6 depending on the setting). Each cell is sized according to its
real content (`VideoListView::applyGridGeometry`): `cellH =
16:9 thumbnail + fixed text block` (8 gap + 34 title + channel line). Do NOT use
a fixed ratio (`cellW * 258/320`) for the height: when the window narrows the
text does not scale and one video covers the next one's channel name. No video
may overlap another, neither vertically nor horizontally.

The **"Tamaño de las miniaturas"** setting (Ajustes → Interfaz, key
`thumb_size` = `small|normal|large`) replaces the old "Densidad de cuadrícula"
(`grid_density`), which is MIGRATED when read (`compact`→`small`,
`cozy`→`normal`; the old key stays inert in the DB). It translates into TWO
effects because grids and lists are not sized the same way
(`MainWindow::applyThumbSize`, the only place that applies it, both at startup
and live):

| value | Home (columns) | Lists (row scale) |
|---|---|---|
| `small` "Pequeñas" | 6 | 0.88 (row 76 px) |
| `normal` "Normales" | 5 (default) | 1.0 (row 86 px) |
| `large` "Grandes" | 4 | 1.15 (row 99 px) |

- **In lists** (Búsqueda, Canal, Playlist, Historial/"Ver todo", "A continuación"
  and the Biblioteca sections) the `setDensity` factor does show: the row is
  `kListH * d` and the thumbnail `136d × 76d`.
- **In grids the factor does nothing visible** (that is why the setting used to
  go unnoticed on Home): the cell is sized by the WIDTH with the fixed columns,
  so at 903 px it measures 175 px with density 1.0 and 175 px with 0.88. The
  only real lever is the number of columns (`HomePage::setCellColumns` →
  `setGridColumns` → `applyGridSizes`, which also recomputes each section's
  height with the new cell size).
- **Width cap**: `VideoListView::clampGridColumns` trims the requested columns so
  that the cell does not drop below `kMinCellW` = 130 px (below that, the
  2-line title and the channel line get clipped). At 903 px 6 columns fit
  (cell 147 px), at 700 px only 5, and it never drops below 3. It is applied in
  `HomePage::effectiveCellColumns` from `applyGridSizes`, that is, also on every
  `resizeEvent`. Pure function tested in `--selftest-grid` (12 column cases + 10
  text cases + monotonicity from 100 to 900 px).

- **The cell text is derived from the cell** (no longer fixed):
  `VideoListView::gridTitlePx(cellW)` gives **13 px with a 140 px cell up to 18 px
  with a 340 px cell** (linear, with caps at both ends), and
  `gridTitleLineH` = 1.28 × size. Before it was 13 px and a fixed 34 px box
  while the thumbnail did grow, which on a large monitor left the text small and
  disjointed. With a 177 px cell (5 columns at a 903 px viewport) it comes out
  13 px, and with 340 px or more 16 px (cap). The title is **bold**: it was tried
  without it and the user asked to put it back. What he did not like was the
  SIZE, not the weight (with the font already 1 px smaller, bold only helps
  distinguish the title from the channel line). The **meta line** (channel ·
  views · date) is **2 px below the title size** (floor 10 px): the user asked
  for it to be more discreet than the title.
- **`kGridTitleLines` = 2 lines, with the font 1 px smaller and without bold.**
  Those are the three things the user asked for upon seeing 14 px bold at 3
  lines: the cell ends up quite a bit more compact (175 px tall instead of 195
  with a 177 px cell) in exchange for a little legibility. Measured with the
  REAL Qt metrics (`QFontMetricsF`, same font and same greedy word wrapping as
  `drawGridTitle`) over the 90 titles in the cache and the 161 px box that comes
  out at 5 columns in a 941 px window:

  | config | full title | visible text | of them, ≥5 lines |
  |---|---|---|---|
  | 14 px bold, 3 lines | 13% | 72% | 51/90 |
  | **13 px bold, 2 lines (current)** | 3% | 55% | 37/90 |
  | 13 px normal, 2 lines | 5% | 58% | 22/90 |
  | 12 px normal, 2 lines | 8% | 62% | 9/90 |

  Bold costs some legibility (3% full versus 5% without bold) and it is the
  price the user chose: the compact cell matters to him more than reading the
  full title. The cell measures `thumbH + 6 + 8 + gridTextBlockH(cellW)` and the
  block `2*lineH + 14 + 20` (64 px at 12 px, that is, a 140 px cell); it does NOT
  change the number of videos per row. If some day he complains that titles get
  cut off, the first experiment is **12 px** (measured: 8% full and 62% visible,
  BETTER than 13 px) and the second is going back to 3 lines.
  **Geometry and painting share the same pure functions**
  (`gridTitlePx`/`gridTitleLineH`/`kGridTitleLines`), so the cell reserves
  exactly what the title occupies and cannot overlap the channel line.
  The split into N lines is greedy by words and the LAST line takes in
  everything left from the start of that line, elided: you see the beginning of
  the rest instead of a hard cut. The **list** mode is not touched (it stays at
  2 lines with the font's natural leading, because its row height is fixed and
  does not derive from the cell).
- **The cell must measure the same in the THREE places that use it**: the
  `sizeHint` carried by each item of the model, `gridSize()` (from which the page
  gets the section height) and the rect that QListView ends up painting. In
  `IconMode` **the MODEL's `sizeHint` overrides `gridSize()`**, so when they
  diverge the rows occupy one thing and the section is sized with another, and
  **the last row runs off the view** (with it the channel line): it was a
  PRE-EXISTING bug with `sizeHint` pinned to the constant `kGridH` (258) while
  `applyGridGeometry` computed another `cellH` (195 at a 177 px cell), that is,
  rows of 258 inside sections of `rows*195`. The only source is
  `applyGridGeometry`, which also writes the `sizeHint` of all items
  (`m_gridCell`, and `gridItemSize()` for new items); that is why it is called
  AFTER creating them. The section height is still `rows * cellH` because in
  `IconMode` the rows are pinned one after another: `setSpacing(4)` **adds**
  nothing (measured: cell 177x195 -> rect 177x195 and step 195) and the visual
  gap between thumbnails is the 6 px internal margin the delegate paints. It is
  checked by `--selftest-grid` with a real `VideoListView` (19 videos, 5
  columns): `sizeHint` == `gridSize` == real rect, the last row fits in
  `rows*cellH` and the rows do not overlap.
- NOTE: the text already scales, but the **column** setting is still the user's
  decision (4/5/6). With very narrow cells the title will still get cut off:
  there the one that decides how many lines fit is the setting, not the font.

## Video row in lists: ONE shape on every page

The `VideoListView` row in `ListMode` (Búsqueda, Canal, Playlist,
Historial/"Ver todo", "A continuación" and the Biblioteca sections) is the
SAME shape everywhere: **thumbnail + title + "channel • views • date" +
options button ⋮**. The only thing that changes between pages is the SIZE of the
thumbnail and the row (Búsqueda/Canal/Playlist/queue at scale 1.0, Historial at
1.21, Biblioteca with its 236x133 thumbnail): the user asked for "same style,
not same font size or bold", so the typography is what each surface already had
(bold title with the app font and reduced meta in the lists; 14/600 and 12 px in
Biblioteca). Do NOT unify sizes or weights.

- **`format::metaLine(v)`** (`src/ui/format.*`) is the only function that composes
  the meta line. It is used by the delegate (grid and list) and by the
  Biblioteca rows. Before it was triplicated (two copies in the delegate and one
  in Biblioteca) and the list row painted only "channel • views": the same video
  read differently depending on the page.
- **The text block is CENTERED in the row**, and with the REAL height of the
  title (1 or 2 lines): `wrapTitle` (the same function that paints
  `drawGridTitle`) gives the lines it occupies and `paintList` centers with that
  height, so the meta stays attached to the last line of the title. Before,
  2 lines were always reserved and with a one-line title the meta fell a whole
  line lower (measured: gap ~31 px; now ~12). The block's measurements are in
  `VideosDelegate::listMetrics`, and the title box and the meta baseline come
  from there.
- **No text under the buttons**: `textWidth(r, ...)` discounts the real gap of
  the X and/or the ⋮, with the same calculation as `removeButtonRect`/
  `moreButtonRect`. Measured: without discounting the ⋮ the title reached x=839
  with the button's dots at 838-841.
- **The ⋮ sits flush against the right edge when there is no X** (like the
  Biblioteca button) and to the left of the X when there is one. Before it always
  reserved the X's slot even when it was not painted, and it stole 34 px of text
  from the row (most noticeable in the narrow "A continuación" column).
- The 2-block meta mode (`setMetaLines`/`paintRichMeta`/channel avatar) was
  deleted: nobody called it and it painted a different row, exactly the
  divergence that was being removed.

### ⋮ menu: a single one for all surfaces

`nt::addRowMenuActions` (`src/ui/row_menu.*`) adds the standard actions with
the same texts and in the same order: **Ver más tarde** (toggle; label
"Guardar" or "Quitar de Ver más tarde" depending on the state), **Añadir a playlist**
(with "+ Nueva playlist" if the surface passes the callback) and **Compartir** and
**Ver canal**. Each surface prepends its own action: Historial "Quitar del
historial", "A continuación" "Quitar de la cola". The actions that write to
the DB are executed with `QTimer::singleShot(0)`: in the `triggered()` the list
the menu was born from may be left without the row with the popup open.

- Biblioteca, Historial and "A continuación" build the menu in the page itself
  (they have the repos and, the queue, the `PlayerController`).
- Búsqueda, Canal and Playlist do **not** have the repos: they emit
  `rowMenuRequested(video, pos)` and `MainWindow::showVideoRowMenu` builds the
  menu ONCE (same helper) with its own. "Nueva playlist" comes out through the
  shared dialog `nt::inputText`, which it enters through
  `LibraryPage::createPlaylistForVideo(videoId)` (the name no longer says "popup"
  because it is the app's overlay, not a page popover).
- "Quitar de la cola" uses `PlayerController::removeFromQueue(index)`, which does
  NOT accept the index currently playing (it would leave the queue pointing at
  the next one with the state of the one that left) and reindexes the resume
  positions (`m_indexStartMs`) when shifting.

## Channel page: card in a box and its own tabs

What governs the page:

- **The card goes inside a box** (`QFrame#channelCard`, #181818 + border
  #262626 + radius 12, like `#settingsCard` and `#watchDescCard`). Before, the
  avatar, title, subscriber count and "Suscribirse" sat loose on the page
  background, and the description on its own gray line below. Now the avatar (80
  px, circular), the name (`#channelName` 19px/700), the subscriber line
  (`#channelMeta` 12px `kMetaText`) and the two actions go in one
  row, and the description (`#channelDesc`, same color/weight as `#watchMeta`)
  closes the box. The name is 19px because it is the H1 you enter to; the rest of
  the surfaces keep their typography (here NOTHING is unified with Search,
  Library or Player, only the shape).
- **Subscribers with `compactCount`** (`format.h`), not the raw `long long`:
  before it printed as-is "1234567 suscriptores".
- **The avatar is really circular**: `nt::circularAvatar` (`src/ui/icons.*`)
  scales, crops the center and masks with a circle. The QSS `border-radius` only
  rounds the background of a QLabel, and `setScaledContents` deformed
  any non-square avatar. This function replaces the two identical copies
  that `channel_row.cpp` and `watch_page.cpp` had (the Player's also stretched
  the pixmap).
- **The description is clipped by LINES, not pixels**: 3 (`kDescLines`).
  The previous cap was 165 px, which depends on the font's line spacing. The
  "…más"/"…menos" button is `#descToggle` (discreet link, not a pill) and the
  `#descFade` gradient (26 px above the bottom edge of the box,
  `positionDescFade`). NOTE: the **Player NO LONGER collapses** (it shows the full
  description); this clipping + `#descToggle`/`#descFade` belong only to the Channel page.
- **The Channel description carries links like the Player's**: the QLabel
  `#channelDesc` is `Qt::RichText` with `LinksAccessibleByMouse` +
  `setOpenExternalLinks(true)`, and the text is painted with the SAME HTML as the Player
  (`descriptionHtml()` in `src/ui/format.*`: escapes, `\n`→`<br>` and each URL in
  an `<a>` of the accent color, without underline). The PLAIN text is stored apart
  (`m_descText`) because `applyDescriptionCap`/`toggleDescription` check for
  emptiness against the real text, not against the rendered HTML (and `heightForWidth`
  measures the same rich text that is painted).
- **The tabs are no longer a `QTabWidget`**: they were the ONLY thing in the app without
  styling (there is no `QTabWidget`/`QTabBar` rule in the QSS) and the "pane" painted a
  nearly black `#020303` column under the list, darker than the page
  background. Now they are three `QPushButton#tabBtn` inside a
  `#channelTabs` capsule; the selected one carries the accent in the text (like the nav) and
  they all cancel the `:focus`. The content is three widgets that `showTab()`
  shows by hand (not a `QStackedWidget`, which would bring back the desktop theme
  background). `#tabBtn` and its `:focus` are watched by `--selftest-theme`.
- **Video counter** of the active tab as a `#tabCount` pill
  (same shape as `#queueCount`), hidden with 0 videos because then the
  empty state already shows.
- **Empty state per tab** = `#emptyState` (the same dotted box as the
  Library) with that tab's text + the app logo on top
  (`nt::emptyStateHtml`), and the list hides at the same time. With a **FIXED height
  of 116 px** (it was 96 without the logo), and NOT with stretch: measured in a 700 px
  QVBoxLayout, a label with stretch 0 whose only stretch sibling is hidden
  stretches to the full 700 px (a giant dotted box); with a fixed height it stays in a
  band centered vertically and full width.
- **"Cargar más vídeos" is a centered `#ghost`** (height 34). It was `#primary` with
  `setFixedWidth(180)`, a wide red bar under the list; loading more is not
  the main action of the page. Only visible if the response came full (25).
- **Added "Compartir" button** (`#actionChip` chip with `nt::Icon::Share`): copies
  `https://www.youtube.com/channel/<id>`. When re-tinting the accent, the icon must be
  redone by hand (`applyAccent`): `QIcon`s carry the color embedded.

## Player page: single scroll (video + description + A continuación)

The Player page scrolls like YouTube: video, description and "A continuación"
live INSIDE a single `QScrollArea` (`WatchPage::m_topScroll`) and the scroll
moves everything together — as you scroll down, the video goes off screen and the description/queue
gain space. Inside the scroll, the description goes on the left and "A
continuación" on the RIGHT (with no vertical limit: it grows with all its rows and
the general scroll traverses it). Both blocks are aligned at the TOP and keep their
natural height (`midLay->setAlignment(..., Qt::AlignTop)`): the row grows with
the queue, but the description does not stretch to that height — below each
block there is only empty scrollable space, not a stretched widget. The Player's
"Volver" button is a floating link **over the video that only appears with
the mouse** (detail in the `#backLink` point below): there is no longer a fixed
bar pushing the image down.

- **"Volver" is the SAME token on the four surfaces** with a back action
  (Channel, History, Playlist and the Player): `QPushButton#backLink`, which is a
  LINK (no frame, `padding: 4px 8px`, 13 px/600, `@MUTED@` → `@TEXT@` on
  hover) and not a pill. Before it was `#ghost` (with a border) on the three pages
  and a floating button over the video in the Player. It is the same criterion as
  `#sectionLink` in the Library: "back" is navigation, not a
  control of the page. The icon is `nt::icon(nt::Icon::Back, nt::kMuted, 13)` on all four
  (drawn with QPainter, never the `◀` glyph). What does change between
  surfaces is only WHERE the link goes: on the three pages inside their
  own root layout, and in the Player **over the video** in the top
  left corner (`m_back->move(12, 8)`), **only while the controls are
  visible**: it shares the 2.5 s cycle with `m_ctrlBar`, that is, it appears when
  moving the mouse over the image and turns off by itself with
  `hideOverlayControls`. It lives as a child of `gl` (the transparent `OverlayHost`
  of page 0 of the overlay), not of the page: that way the mini player's reparent
  needs nothing (the overlay is already a child of the player) and `gl1`, which only manages
  the stretch and `m_ctrlBar`, does not touch its geometry.
- **Beware the history of this button**, which has already gone two rounds: the user
  asked for it FIRST outside the image (it lived in a floating button `move(12,8)` +
  `raise()`), then it became a fixed 38 px bar across the full width above
  the scroll (`m_backBar`), and that bar he didn't like either (a band full of empty space
  with a 63 px link, and it pushed the video to `y=38`). The current state is again
  over the image but **revealed with the mouse**, which is the only thing that does not
  occupy space of its own. If it is touched again, look at the guard of
  `m_inFullscreen` in `showOverlayControls`: `enterFullscreen()` calls
  `showOverlayControls()`, so hiding the button ONLY from
  `enterFullscreen()` is not enough (it shows again).

- `PlayerContainer::updateMaxHeight()` sets `min = max = min(width/aspect,
  kMaxPlayerHeight=560)` (not only the maximum, like the old split): inside
  the scroll the layout must give the video EXACTLY its height, neither stretched
  by Expanding policies nor shrunk by the scrollable content. The absolute cap
  **does NOT apply in fullscreen** (`setFullscreen(true)` →
  `capped = width/aspect`): if the 560 is reapplied in fullscreen the video
  would be 560 px with the screen full of black (regression seen and
  reverted in the old split layout). The cap does apply in a normal window
  and maximized.
- The "A continuación" queue goes to the RIGHT of the description, with NO vertical
  limit: in `WatchPage::updateQueue()` its height is set to the real content
  (`sizeHintForRow(0)` × no. of rows) so that the page's general scroll
  traverses it whole, with no duplicated internal scroll.
- Fullscreen (video only): the Volver link + description + queue are hidden
  (`m_back`, `m_watchBody`, `m_bottomWidget`); with `widgetResizable` the
  content is = video only and stretches to the whole page (without reparenting the
  player). On exit they are restored and the scroll returns to 0.
- When changing video (`onCurrentChanged`) the scroll returns to the top (like
  YouTube); not in fullscreen (there it is hidden).
- Visual check: QOpenGLWidget inside a QScrollArea works in Qt
  5.15, but you must check on the real machine that there is no flicker/
  artifacts when scrolling and that the "jump on return" of the video when returning
  to the top after being outside the viewport is acceptable (mpv accumulates frames
  while it does not paint).

### Player controls row and icons (vector Material Symbols)

The control bar is **A single row** (YouTube style): the transport
starts the row with **play/pause first** (like YouTube), then
previous and next, and finally shuffle and repeat; elapsed time +
progress slider + duration in the center, and volume/actions on the right,
all inside `m_ctrlBar` (`#playerOverlay`):

```
[play][prev][next][shuffle][repeat]  0:00 ──●──  3:21 │ [mute] ──●── │ [CC][playlist][gear][fs]
```

- **"Anterior" has YouTube's behavior** (`PlayerController::previous()`):
  if the current video is advanced more than `kPrevRestartThreshold` (5 s, the same
  order as the keyboard seek step), the button **restarts** the
  current video; only if it is at the beginning does it jump to the previous video
  in the queue. The overlay's button and its enabled state use `previousAvailable()`
  (`hasPrevious() || position > threshold`), which is refreshed on every position
  tick (`onPositionChanged`) to become enabled when crossing the threshold. The
  **P** key (and **Shift+P**, which enters through the same `case`: the filter above only
  discards Ctrl/Alt/Meta) does the SAME as the button: it calls `previous()`. Before,
  P was a DIRECT jump (`previousTrack()`), and that is why on the first video of
  the queue it did nothing and seemed broken; now it restarts if the video is advanced
  and the "nothing to do" (first video not advanced) is resolved by `previous()`
  itself. `previousTrack()` still exists because `previous()` uses it.

- **The icons are vector, not PNG.** `src/ui/player_glyphs.{h,cpp}` defines
  `nt::PlayerIcon` + `nt::playerIcon(kind, color, size=20)`: the geometry is from
  **Material Symbols** (`google/material-design-icons`, Apache-2.0; files
  `symbols/web/<nombre>/materialsymbolsoutlined/`), but it is painted with
  `QPainter` (a parser for the subset `M m L l H h V v Q q T t Z` builds the
  `QPainterPath`), just like `nt::icon()`. It is sharp at any size, it is
  recolored live with the accent (cache keyed by icon/size/hex color, without the
  retint that broke the nav) and it does **not** put PNGs into the `.qrc` nor link
  `Qt5::Svg` (which would add `Qt5Svg.dll` to BOTH Windows packages). The
  transport (`Play`/`Pause`/`Prev`/`Next`) uses the **filled** variant of
  Material like YouTube; the rest, the outlined one. Sizes: play 22, rest 20
  (mini 16). The active modes (shuffle, repeat, subtitles) go in the
  accent color.
- **No frames or boxes**: all controls are `QPushButton#iconBtn`
  (transparent, fill only on hover). Play is NOT a circle.
- **Responsive** in `OverlayHost::onWidth` (`gl->onWidth`, in watch_page.cpp):
  `w < kCtrlWithVolMinW` (760) hides the volume slider (mute stays) and
  `w < kCtrlWithTimeMinW` (640) also hides the time labels. The progress
  slider is the only flexible one. The thresholds and their sum are commented in
  the constants (the `setFixedWidth` of the volume slider does not give way and the
  trimming would fall on icons and clock).
- **Shuffle and repeat are REAL**, in `PlayerController`
  (`setShuffle`/`shuffle`, `setRepeat`/`repeat`; per-session, they are not persisted).
  Shuffle randomly reorders ONLY the queue ahead of the current video (the current
  one does not move, like YouTube) with Fisher-Yates and discards the resumes
  positions of the moved indices (except the current one). Repeat goes back to the
  first video when the last one ends (`next()` and the "ended" branch of
  `onPlayerState` with wrap to `playIndex(0)`); with a queue of one it is "repeat one".
  `updateOverlayIcons()` paints its state in accent.
- **The Player description is shown COMPLETE**: the collapse to 3
  lines and the "…más"/"…menos" control were removed (the user asked for it). The Channel page DOES
  keep its own (`#descToggle`/`#descFade` remain in the QSS for it). The
  URLs are wrapped in accent links with `descriptionHtml()` (`src/ui/format.*`),
  the SAME helper the Channel card uses: a single HTML for both.
- **The quality menu** (`#qualityMenu`) uses the SAME border as `#playerOverlay`
  (overlay background, border `@BORDER@`, top edge `@OVERLAY_EDGE@`, radius 12)
  — before it was `#1F1F1F` with border `@SELECTION@` and radius 6. Its entries carry
  HD/4K via `qualityLabel()`: "720p HD", "1080p HD", "1440p HD", "2160p 4K" (the
  rest, only "NNNp").
- **The quality chip disappeared**: the old `QPushButton#chip` ("720p") is
  now the gear `PlayerIcon::Settings` (`#iconBtn`). The active resolution
  lives in the **tooltip** (`setQualityIndex`/`onVideoSizeChanged`); the menu remains
  anchored to it (`toggleQualityMenu`). That is why the `QPushButton#chip` rule
  was deleted from the QSS AND its entry in the borderless buttons list of `--selftest-theme`.
- **The tooltips are TWO lines: the action and, only if it has one, the shortcut.**
  Format `"Descripción\nAtajo"` — the second line is ONLY the shortcut, without
  parentheses or words (`"Reproducir/Pausa\nK"`, `"Anterior\nP"`,
  `"Siguiente\nN"`, `"Silencio\nM"`, `"Pantalla completa\nF"`; the mini player,
  `K`/`N`/`M`). Those without a key (shuffle, repeat, CC, add to playlist,
  quality) keep just the description. The tooltips are system windows
  and by default are painted with the desktop theme palette (the text was not
  readable depending on the distribution): that is why the QSS includes a compact
  `QToolTip` rule (background `@POPOVER@`, text `@TEXT@`, border `@BORDER@`, padding
  3×7 px, 12 px) so the box is just the size of the text. NOTE: the tooltip of
  `m_quality` is OVERWRITTEN on the fly with the active resolution
  (`setQualityIndex`/`onVideoSizeChanged`) and has no shortcut.
- **The clock is split**: `m_ovPos` (elapsed, left of the slider) and
  `m_ovDur` (duration, right), not a single "0:00 / 0:00". QSS `#ovPos`/`#ovDur`
  (`@TEXTSOFT@` 12 px) and separators `QFrame#ovSep` (1×20, `@OVERLAY_EDGE@`).
- **The two overlay sliders (seek `#ovSeek` and volume `#ovVolume`) are painted
  by hand** (`OverlaySlider` in `watch_page.cpp`), NOT with QSS. Qt 5.15's
  `QStyleSheetStyle` places the handle with its center ~half a width to
  the LEFT of the real point and anchors the sub-page (the fill) to the edge of the
  WIDGET, not to that of the track: at minute 0 the circle stuck out to the left
  of the track and a red residue peeked out beside it (the "something red" reported).
  No margin aligns it (the handle offset and the fill anchoring are
  coupled; 7 variants were tried). That is why `paintEvent` draws a 6 px track with
  border `@OVERLAY_EDGE@`, fill `@ACCENT@` that ends at the LEFT edge
  of the circle, and a white circle of 12 px whose center maps between [radius,
  width-radius]: always whole and visible, and at 0:00 the fill measures zero (no
  red). `SeekSlider` inherits from `OverlaySlider` (it only adds click/drag).
  Do NOT reintroduce `QSlider#ovSeek`/`#ovVolume` rules in the QSS: they would paint
  nothing, because a custom `paintEvent` does not go through the style. The global
  `QSlider::…` rules remain only as the default for a future slider.
- **Live streams ("EN VIVO")**: a live stream has NO final duration — mpv reports
  `duration` 0 (pure HLS live) or the "so far" duration, which GROWS with
  time. With the pos/dur bar the percentage jumps BACKWARDS when the
  duration materializes or grows (it was the bug of the SEVENRO video). The
  detection has TWO paths:
  - **AUTHORITATIVE path (immediate entry)**: `videoDetails.isLive` of the player
    response (the `kFieldMask` requests it and `handlePlayer` stores it in
    `Innertube::lastIsLive()` → `StreamInfo.isLive` →
    `PlayerController::currentIsLive()`). YouTube declares whether the media is a
    current live stream, so EN VIVO mode activates on the FIRST position
    tick after resolving, without waiting for the duration to grow (and it covers
    PURE live streams with dur=0, which by definition never produce increases).
    On both surfaces: `if (!appStream && m_player->currentIsLive())
    m_liveDet.forceLive();` — `LiveDetector::forceLive()` (`live_detect.h`)
    marks live and arms the unlock clock from NOW; the heuristic path
    still governs the EXIT (a live stream that ends and fixes its duration is
    unlocked at ~15 s just as without this function, even though the authoritative
    signal from the resolution remains active).
  - **HEURISTIC path (fallback, sustained evidence)**: **`nt::LiveDetector`**
    (`src/ui/live_detect.h`, one per surface, fed in
    `WatchPage::onPositionChanged` and in the mini of `MainWindow`), not with a loose
    tick. Rules:
  - **Only duration INCREASES count**: `duration > prevDur + 0.5`
    (`kChangeS`), twice (`kLockChanges=2`) → live. A VOD materializes its
    duration once (0 or 1 increase) and keeps it stable: it never reaches 2. A
    decrease or collapse does NOT count: a VOD in reload resets its own
    (17659.5 → 0 → 1213.6 is the SAME finite video materializing again,
    not a live stream). Nor is the "dur=0 with advanced position" branch used, which
    marked normal slow-starting videos.
  - **Automatic unlock**: in live mode, if a STABLE duration arrives
    (>0, without increases) during `kStableUnlockMs` (15 s), it goes back to the
    normal bar (a real live stream keeps growing, it does not unlock by itself; a false
    positive at ~15 s recovers the seek).
  - **EXCEPTION — the synthetic AV1 HLS is never a live stream**: the app
    builds it from a finite fMP4 (VOD+ENDLIST playlist), so by
    construction it is a VOD — but mpv reports UNSTABLE durations while
    it loads the BYTERANGE ranges (measured: 0.0, 26.4, 55.8, 1213.6, even
    2486.9 = more than the real one), indistinguishable from a live stream. That is why
    `emitAv1()` (yt.cpp) marks its `StreamInfo` with **`canBeLive=false`** and both
    surfaces do `!m_player->currentCanBeLive()` before feeding the
    detector (and release the live state if it was active). The other streams
    (YouTube master, filtered —which is also served from the local server—
    and edge) CAN be a live stream: they keep `canBeLive=true` and, if the
    player response declares it, `isLive=true`. The synthetic one comes out with both
    false (it is never a live stream by construction).
  In live mode it is painted like YouTube: playhead STUCK TO THE RIGHT EDGE (range
  (0,1) value 1, the live history fills backwards), the mark
  **"EN VIVO" in `m_ovPos` with the accent** (QSS rule
  `QLabel#ovPos[live="true"]`, dynamic property + re-polish) and the time
  since the broadcast in NEGATIVE in `m_ovDur` (`-" + fmtTime(dur)`; without a
  known duration, only the mark). `SeekSlider::setLive(true)` swallows the
  clicks: in live mode the bar is informative, it is not seeked (the seconds 0..1
  mean nothing). The mini shows "Canal · EN VIVO" and the bar filled to the
  edge. State per surface (`m_liveMedia`/`m_liveDurPeak` in the page,
  `m_miniLive` in MainWindow, each with its own `LiveDetector`) that is RESET
  when changing video (`onCurrentChanged` / connect of `currentChanged`). A
  finished live stream (the playlist becomes VOD and the duration is fixed) goes back
  by itself to the normal bar, like YouTube with the replay.
- **Mini player**: same icons (`nt::playerIcon`, 16 px) for play/pause,
  next, volume/mute and close (4 buttons); denser row (spacing
  6, `QWidget#miniBar QPushButton#iconBtn { padding: 4px; }`). The background is the
  SAME tone as the bottom bar (`#101010`, kTop), not the `kElevated` of
  before: the two bars read as a single piece. Details that cost a
  round and must not be reverted by accident:
  - **Thumbnail with REALLY rounded corners** (`RoundedThumb`, subclass
    of `QLabel` in mainwindow.cpp): the `border-radius` of the QSS only rounds the
    BACKGROUND of the QLabel, not the `QPixmap` painted on top, so square
    corners were seen. `RoundedThumb::paintEvent` clips the pixmap to a rounded
    rect (centered and without rescaling: the pixmap already comes "cover" and the clip
    trims the excess). Without a pixmap it delegates to `QLabel` ("no
    thumbnail" state).
  - **Progress bar glued to the bottom edge and full width** (like
    YouTube): `m_miniProgress` goes OUTSIDE the layout (which has side
    margins) and `MiniPlayerBar::resizeEvent` repositions it at
    `(0, height-4, width, 4)`. Also `WA_TransparentForMouseEvents` so that
    its click does not steal the bar's "expand". With `dur<=0` the range is (0,1)
    and value 0, NEVER (0,0): a 0,0 range draws the animated chunk ("busy" mode).
  - **The progress goes in the ACCENT and measures 4 px** (thicker than the 2 px
    originals): the fill is `@ACCENT@` (it follows the color chosen in
    Settings, because the QSS is regenerated when it is changed) and the track `@HANDLE@`. The
    height is fixed with `setFixedHeight(4)` in `setupMiniBar`.
  - **Hover of the whole bar**: `MiniPlayerBar::enterEvent/leaveEvent` set the
    dynamic property `hover` (with `style()->unpolish/polish`, mandatory) and
    the QSS `QWidget#miniBar[hover="true"]` lightens the background to `#141414` (the
    previous `#181818` looked too pale next to `#101010`). The
    `leaveEvent` does NOT turn off the hover if the cursor is still inside `rect()`
    (moving from the bar to a child button sends `Leave` to the parent and it would flicker).
    That guard is only evaluated when a `Leave` ARRIVES (active interaction, where
    `QCursor::pos()` is indeed fresh), so the hover is also governed
    explicitly: `hideEvent` always turns it off (when leaving the Player page,
    entering the video and coming back did not leave the tone falsely lit),
    `showEvent` re-evaluates it against the real cursor position when re-showing (Qt
    does not always re-send Enter under the cursor), and the constructor connects
    `QGuiApplication::applicationStateChanged` to turn it off when the app is deactivated
    — both alt-tab and workspace change (in both no
    Leave arrives). NOTE: on reactivation it does NOT re-evaluate with `QCursor::pos()`: with the app
    inactive that position is FROZEN at the last known point (Wayland does not
    deliver pointer events when changing workspace) and, if the pointer was
    over the bar when deactivating, it would falsely re-light on return even though the
    real pointer is elsewhere. The hover is only re-lit with a real
    `enterEvent` (or when moving the pointer over the bar).
  - **The metadata line goes on the left, next to the channel**: "Canal ·
    m:ss / m:ss" in a single `QLabel#miniTime`. Separating them was tried (channel left,
    time right) and the time ended up glued to the buttons and far from the channel (bad
    position, fixed). The mini's button order is NOT to be touched by accident:
    play · siguiente · silencio · cerrar.

## Regression tests

```sh
./build/nevertube_qt2 --selftest-db        # repos/SQLite
./build/nevertube_qt2 --selftest-player    # hwdec=auto + event-driven redraw + render ok
./build/nevertube_qt2 --selftest-innertube # sw.js_data→visitorData→player→hlsManifestUrl
./build/nevertube_qt2 --selftest-discover  # saved radios: save/retrieve/re-select/persistence/discard
./build/nevertube_qt2 --selftest-av1hls    # synthetic AV1 HLS (offline): MAP/BYTERANGE, /seg.mp4, unique &nt
./build/nevertube_qt2 --selftest-hwprobe   # offline HW probe (micro-clips): av1-hw|vp9-hw|h264-hw|soft-all
./build/nevertube_qt2 --selftest-ssl       # TLS backend (OpenSSL 1.1 next to the exe) loaded
./build/nevertube_qt2 --selftest-tls       # REAL TLS handshake against public hosts (not just loading)
./build/nevertube_qt2 --selftest-search    # REAL Innertube search: videos with a non-empty id
./build/nevertube_qt2 --selftest-browse    # Innertube endpoints: channel + tabs (videos/shorts/live) + detail + playlist
./build/nevertube_qt2 --selftest-feeds     # channel via Innertube (protocol and chain)
./build/nevertube_qt2 --selftest-grid      # grids: column cap (pure) + cell consistent across model/gridSize/section (mounts a VideoListView; no network or GPU: runs in CI)
./build/nevertube_qt2 --selftest-theme     # pure QSS: duplicate selector, selector without widget, selection palette and button borders: runs in CI
./build/nevertube_qt2 --selftest-glyphs    # every non-ASCII glyph of the UI exists in the app's real font: runs in CI
./build/nevertube_qt2 --selftest-dates     # relative-date parser ("hace 2 semanas" is NOT 2 s → not "hoy"): runs in CI
./build/nevertube_qt2 --selftest-i18n      # catalogs: same key set + every key used in the source exists in the base language: runs in CI
./build/nevertube_qt2 --selftest-variants  # the variants of a video (height, codec and real size): runs in CI
./build/nevertube_qt2 --selftest-reels     # the filter of the Shorts (the vertical videos): runs in CI
./build/nevertube_qt2 --selftest-download  # the plan of a download (the fragments, the sidx, the codec): runs in CI
./build/nevertube_qt2 --selftest-download-folder  # the folder of the downloads and the command that opens it: runs in CI
./build/nevertube_qt2 --selftest-download-settings  # the codes of the settings of the downloader: runs in CI
./build/nevertube_qt2 --selftest-settings-page  # the page of Ajustes (the new section of the downloads): runs in CI
```

The player selftest verifies invariants of the protected core without
playing any file. Run both before considering stable a change that touches
the player.

**What triggers each workflow (important for interpreting a red dashboard):**

| workflow | triggers |
|---|---|
| `linux.yml` (2x `.deb` + `.rpm` + pacman; the `online-report` job in the same file is report only) | **manual** (`gh workflow run linux.yml --ref main`), or a push that touches **the workflow file itself** (filter `paths:`). It does NOT run on every push to `main`; the online job runs only on manual dispatch with the `online_selftests` box ticked. |
| `windows.yml` (x86 / Win7) | **manual** (`gh workflow run windows.yml --ref main`) or if the push touches **the workflow file itself** (filter `paths:`). It does NOT run on every push to `main`. |
| `windows_x64.yml` (Win10+) | only if the push touches **the workflow file itself** (filter `paths:`), or manually with `gh workflow run windows_x64.yml --ref main` |

That is, a push to `main` normally starts **no** CI job; all of them are opt-in. The
x86 one is the heavy Win7 gate (Qt + FFmpeg + Inno + full smoke) and running it
per commit only burned CI time.

**Measured (2026-10-21):** the two Windows workflows DID run on their own with
the push of the ffmpeg packages, because that push edited their own `paths:`
(the gates of the setup). That is the exception, not the rule: a push that
does not touch a workflow file starts no job.

**NO workflow runs a selftest that needs the network as a GATE.** Two families, and
the distinction is a hard rule:

- **Offline (gate)**: `--selftest-db`, `--selftest-discover`, `--selftest-ssl`
  (only checks that Qt's TLS backend LOADS), `--selftest-grid`,
  `--selftest-theme`, `--selftest-glyphs`, `--selftest-i18n`,
  `--selftest-dates`, `--selftest-suggest-parse/panel/gestures`,
  `--selftest-av1hls`, `--selftest-variants`, `--selftest-reels`,
  `--selftest-download`, `--selftest-download-button`,
  `--selftest-download-folder`, `--selftest-download-settings` and
  `--selftest-settings-page`. They run in every build (the three
  `packaging/linux/*` recipes and both Windows smokes) and can fail the
  job.
- **Online (report, NEVER a gate)**: `--selftest-tls` (a REAL handshake),
  `--selftest-innertube`, `--selftest-search`, `--selftest-browse`,
  `--selftest-feeds`, `--selftest-home`, plus the GPU ones (`--selftest-hwprobe`,
  `--selftest-ffmpeg`, `--selftest-player`) that need the real machine. YouTube
  can rate-limit a runner or answer a video without a channel
  (`--selftest-search canalVacio=1` turned an x86 run red once) and no runner has
  a GPU, so they moved out: on Linux to the optional `online-report` job of
  `linux.yml` (only on manual dispatch with the box ticked, `continue-on-error` +
  `::warning::`) and on Windows behind the
  `online_selftests` input of `workflow_dispatch` (UNTICKED by default) with
  `::warning::` instead of `exit`. In the Windows smokes the offline list uses
  `--selftest-ssl` where the old one went straight to `--selftest-tls`.

**Linux packaging** (`packaging/linux/{deb,rpm,arch}`): the recipes are
self-contained and the workflow only invokes them. Each one builds natively in its
distro (no portable Ubuntu binary can go into an `.rpm` or a pacman package),
declares its own dependencies and ends with an INSTALL SMOKE: install the package,
`ldd` with no "not found" and the offline selftests run from `/usr/bin`. The `.deb`
derives its `Depends` with `dpkg-shlibdeps` (+ `libqt5sql5-sqlite`, which Qt
`dlopen()`s and which therefore does not show up as a linked library), so the two
Ubuntu bases (22.04 and 24.04, with the `t64` rename and FFmpeg 6.1) do not need a
hand-kept list; the `.rpm` relies on rpm's automatic soname requires (FFmpeg
included: dnf resolves them to `ffmpeg-free` in Fedora base, or to RPM Fusion's
`ffmpeg` if the user already has it -- no repo is forced on anyone); the
`PKGBUILD` lists them by name. The binary is STRIPPED in the deb and rpm
(`cmake --install --strip`; `makepkg` already strips in Arch): the symbol table
alone was 18 % of the binary.

Also mind the toolset: x86 uses **v142** (VS2019) because of the Win7 target and
x64 the one of `windows-latest` (newer). They do not compile the same: a literal
that x64 accepts can give C2026 on x86, and a preprocessor directive inside a
macro call gives C2121 on x86 (both are documented in the MSVC section above). A
failure that appears only on x86 should not be looked for only there.

## Recovery from a gated session (YouTube 403)

YouTube can "gate" a session of URLs (audio `>403` and HLS segments `>403`
from a specific point): mpv opens the video muted (`audio=eof`, without
`AO:`/sink) and the segments from ~1:00 fail → premature EOF at ~1:03 with
`Cache: 0.0s` and an error, with no sound during the whole video. Intermittent
(~1/10 sessions). The app now recovers on its own by re-resolving with fresh
URLs:

- `PlayerWidget::streamInterrupted()` is emitted in two cases: **premature EOF**
  (non-natural EOF with `duration > 0`, which is not end of video nor live) and
  **no active audio track** (~10 s playing with mpv's `audio-params`
  absent and `position > 2s`; `kAudioDetectMs`). The audio watchdog does NOT
  depend on the mode: with the gate also on muxed HLS mpv goes mute
  (`audio=eof`), so it fires at ~10 s (before it only watched EXTERNAL audio and
  the HLS exhausted ~60 s mute per attempt until EOF). It does NOT touch the
  render nor the redraw: it is event logic and the `checkStall` watchdog
  (audio).
- `PlayerController::onStreamInterrupted()` re-resolves (like a stall):
  `reloadCurrent()` with `m_recoverAttempts` (≤3, a setback <8s short and
  stability >5 min resets; NOT 60 s: the gate dies at ~60 s and with 60 s the
  reset erased the budget right before each death → reload → 60 s → reload…
  endless loop). With the watchdog at ~10 s each attempt costs ~10-16 s
  (resolution + wait): 3 attempts ≈ 40-50 s before the last resort. In
  `soft-all`/HW the cost is a bounded "loading" stretch; in normal
  operation no new code runs per frame.
- **Session rotation on recovery**: the gate is of the SESSION, not of the URL —
  re-resolving with `forceRefresh` asks for a new pot WITHIN the same gated
  session and fails again. That is why `onStreamInterrupted()` and the setback of
  `onStreamStalled()` call `Yt::rotateStreamSession()` (→
  `Innertube::rotateSession()`: discards the current visitorData + cached signed
  URLs) BEFORE `reloadCurrent()`: the re-resolution comes out of the gated
  session with a new visitorData (~9/10 of not being gated). It is still bounded
  by the budget: if the new session also dies, in ≤3 attempts it is
  exhausted. It only rotates on the failure path.
- **Last resort: app restart with the last video.** When the
  recovery is EXHAUSTED (`recoveryExhausted()` of `PlayerController`, both in the
  `streamInterrupted` and in the stall setback) `MainWindow::onRecoveryExhausted`
  restarts the app with the last video: restarting DOES get out of the gate (state of
  the new process — verified by the user: on restarting the audio is fixed).
  It is the ONLY auto-restart of the app and ONLY on this error: it relaunches with the URL
  of the last video only (the queue is NOT restored) and resumes the position.
  Anti-loop guards: (1) in memory `m_gateRestarting` — the same episode does not
  relaunch twice (PlayerController can re-emit in a cascade of EOF);
  (2) in settings `gate_restart_armed` — if the instance ALREADY came from a
  restart and is still gated, the error is shown and the guard is disarmed (restarts
  are never chained). The guard is disarmed only with
  `PlayerController::recoveryHealthy()` (≥5 min of stable playback, emitted
  from the `m_flushTimer`): after a restart that worked, the next gated
  session will be able to restart again. Keys: `gate_restart_vid`,
  `gate_restart_pos` (ms), `gate_restart_armed` (1/0). The new instance
  starts with `http://...` → `MainWindow::playVideo` (read in the constructor).
- `hasActiveAudio()` queries mpv's `audio-params` (`MPV_FORMAT_NODE`). With
  healthy audio the track is active and the detection does not fire (verified:
  normal run with `AO: [pipewire]` without false `streamInterrupted`).
- No sound + a video that "goes away" at ~1:00 = symptom of a gated session, not of the
  audio sink nor of the protected piece. Verify with the `E2E` log
  (`NEVERTUBE_DEBUG=1`): `audio=eof`, `HTTP error 403` on segments, and
  `E2E streamInterrupted -> reloadCurrent`.

## Debug logging

The `E2E ...` logging is enabled by an environment variable (it does not print by
default, avoiding noise in the event loop of the player):

```sh
NEVERTUBE_DEBUG=1 ./build/nevertube_qt2
```

On Windows, `NEVERTUBE_DEBUG=1` also makes a double-click launch
open its own console for that log; without the variable, the app (GUI
subsystem) does not open any terminal window. Launched from a shell, it always
inherits the console of the parent process.

Use `NT_DEBUG(...)` (src/debug.h) for debug traces.

## Multilanguage (i18n)

The UI is translated with a small engine of its own (`src/i18n.h` /
`src/i18n.cpp`), NOT with Qt's `.ts`/`.qm`. The reason: adding a language must be
dropping ONE file, with no translation toolchain and no build step, and the app ships
the catalogs inside the binary.

- **One JSON per language** in `localization/` (`es.json`, `en.json`, ...), embedded
  through `localization/localization.qrc` (prefix `/localization`). The file name is
  the language code. Adding a language = dropping `<code>.json` next to the others and
  listing it in the `.qrc`; the Settings selector picks it up by itself
  (`nt::availableLanguages()`).
- **Spanish is the base (source) language** and the fallback: a key missing from the
  active language is resolved in `es`, and a key missing everywhere is returned as-is,
  so it shows up in the UI instead of blank. Each file carries its own native name
  (`"language": { "code": "en", "name": "English" }`).
- **The default UI language is English, not the base.** On a fresh install (no
  `ui_lang` saved) `MainWindow` calls `nt::systemPreferredLanguage()`: a system in
  Spanish picks `es`, English or any other language keeps English; the user's explicit
  choice in Settings always wins (it is saved as the code in `ui_lang`). The base stays
  Spanish because that is the wording of the source and the fallback, not because it is
  the default UI.
- **Keys are semantic** (`nav.home`, `settings.quality`), never the Spanish text, so
  the wording can change without touching the code. Every language file MUST have the
  SAME key set as `es.json` (the selftest fails otherwise).
- **API**: `nt::tr("key")` returns the text in the active language; it is the one used
  when a text is composed at set time (a counter, a video title, a menu built on the
  fly). For the static chrome there are live bindings — `nt::bindText`,
  `nt::bindTooltip`, `nt::bindPlaceholder`, `nt::bindAction` — that apply the text now
  and re-apply it when the language changes (`nt::retranslate()` runs them all). Only
  static labels may be bound.
- **Live switching, no restart**: the Settings option calls `nt::setLanguage()` and
  `MainWindow::applyLanguage` re-applies the bindings, the Settings page and the
  Library section titles (which compose "Title · N" and cannot be bound, hence their
  own `retranslate()`). The on-screen text changes in place.
- **What `applyLanguage` must call, in three tiers** (getting this wrong is the
  "it does not change language in place" bug). (1) **Static chrome → live binding**:
  `bindText`/`bindTooltip`/`bindPlaceholder`/`bindAction`/`bindAccessibleName`. The
  last one exists because an icon-only button (`#iconBtn`, `#actionChip`) has no text:
  its tooltip and its accessible name ARE its label, and both must be bound (they are
  separate properties, hence two calls). (2) **Composed text → the page's
  `retranslate()`**, called from `MainWindow::applyLanguage`: any text built with
  data at set time cannot be bound. That covers `LibraryPage` (section counters),
  `SettingsPage` (full rebuild), and —added with the hot-switch audit— `WatchPage`
  (title from the playback state, channel/subscribe buttons, meta line, queue counter,
  quality tooltip), `ChannelPage` (name, "N suscriptores" —`m_subscriberCount` is kept
  so it does not ask the network again—, tab empty states, tab counter, "…más/…menos"),
  `SearchPage` (the channel cards' "@handle · N suscriptores" meta, from the stored
  `ChannelInfo`), `PlaylistPage` (title fallback, counter, empty state) and
  `HistoryPage` (counter, empty state). The empty states go through
  `nt::emptyStateHtml` (it embeds the logo), so they are NOT bindable and must be
  re-set in the page's `retranslate()`. (3) **Text painted with `nt::tr` at paint
  time** —the list/grid meta line (`format::metaLine`), the "En directo" badge and the
  Library's hard-painted progress/badge— is NOT in any widget: after the retranslates,
  `applyLanguage` walks `findChildren<QWidget*>()` and calls `update()` on each, which
  repaints the delegates (`VideosDelegate`, `AsyncThumb`). Without that repaint the
  rows keep the old language until they are rebuilt.
- **Never decide logic by comparing TRANSLATED text.** The real bug: Search's
  busy/empty labels were shown/hidden with
  `child->text().startsWith("Buscando")` / `startsWith("Sin...")`. That only matched
  in Spanish, so in English the spinner was never hidden and the empty state never
  appeared —and it broke on a live switch—. The labels are now members
  (`m_busyLabel`/`m_emptyLabel`, object name `#sectionSub` kept for the QSS) and are
  toggled directly. Grep for `text()` next to `nt::tr`/`startsWith` before adding UI:
  a translated string is not an identifier.
- **A dead `PlaceholderPage` is not an excuse**: it is not mounted anywhere, but its
  hint was still a one-shot `nt::tr` and got moved to `bindText` so the audit is
  uniform.
- **The identity of an option is a stable code, not its text** — that is what makes
  rebuilding the Settings page (or switching language) lossless. Quality
  (`auto`/`720`...), `next_mode`, `mini_video`, `audio_language`, `volume_max`,
  `thumb_size`, the accent (hex) and the language itself (`ui_lang`, the code) are all
  stored as codes; the visible text is `nt::tr(...)` of the code.
- **Error messages are translated too**: the player/network layer
  (`player_controller`, `playerwidget`, `ffmpeg_backend`, `yt`, `yt_innertube`) emits
  `error.*` keys instead of literal Spanish, because that text is what the Player
  overlay shows.
- `--selftest-i18n` (in the smoke of both CIs): parses every catalog, requires the
  same key set, and scans the source tree for the keys that appear INSIDE
  `nt::tr(...)` / `nt::bind*` calls, HARD-FAILING when a key used in the code is
  missing from the base language (a typo that would show the raw key on screen stops
  CI). `i18n.cpp` is excluded from the scan (its own prefix table would produce
  garbage keys). The key must be a string LITERAL in the call line; `nt::tr(variable)`
  is not scanned.

### OPEN BUG: the hot language switch can interrupt playback (x64, real Win10)

**Symptom**: with a video playing, switching the language to English in Ajustes
made the video pause as if it were buffering. Switching was done in place
(`SettingsPage` emits `languageChanged` → `MainWindow::applyLanguage`). The same
build at the same commit runs clean on the dev VM.

**What was measured, and what it RULES OUT.** Reproduced on the dev VM with the
same build, with `NEVERTUBE_DEBUG=1`, and analysed from `E2E tick` (the
per-position lines mpv emits through the Qt event queue, which is the direct
probe of a busy main thread):

| probe | result |
|---|---|
| position ticks in the session | 1594 |
| **gaps >= 5000 ms** (the watchdog threshold) | **0** |
| largest gap | 3303 ms, and it was at `pos=0.1` (start-up), not at the switch |
| `E2E streamStalled` / `streamInterrupted` in the whole session | **none** |
| `ui_lang` after the run | `en` — the switch DID happen |

So the switch does **not** block the main thread past the watchdog threshold and
does **not** trip `PlayerWidget::checkStall` on this machine. The only
`reloadCurrent` in the log was at `+72918 ms` and its cause is printed on the
line above it: `E2E qualityChanged index=5 -> 1080p` — a **quality** change
(360p → 1080p, the software-cap ladder), not a stall. Do not chase that one.

**What remains**: the machine where it reproduces is NOT the one measured. The
dev VM has no GPU (`codec_profile=soft-all`, ANGLE over WARP, 360p, 940x800);
the report is a real Win10 with hardware decoding at a larger size. The
difference that plausibly matters is **hwdec and the render size**, not the
text: the player sits in the mini bar (112x63) on both.

**Suspect, unproven**: `MainWindow::applyLanguage` calls `update()` on every
widget in the tree (`findChildren<QWidget*>()`), which includes the
`QOpenGLWidget` — a forced repaint outside mpv's render-update callback. The
alternative suspect is the **full `SettingsPage` rebuild** that
`applyLanguage` defers with `singleShot(0)` (it deletes every direct child and
re-runs `buildUI()`), which could resize or re-show things. Note that the pages
live in a `QStackedWidget`, so only one is visible at a time and the repaint is
bounded to it — which is why the "the event loop is flooded" theory is weak.

**Evidence that would settle it** (not available yet): `NEVERTUBE_DEBUG=1` on the
machine where it happens, plus the same tick-gap analysis. `applyLanguage` logs
**nothing** on success today, so the switch is invisible in the log — an `NT_DEBUG`
line with its duration (and the `PlayerWidget` geometry before/after) is the
cheapest fix to the diagnostics and touches no protected code. Until then this is
a report, not a fix: **do not "fix" it by guessing**, because the two suspects
lead to different changes and only the measurement distinguishes them.

## The Shorts (the reels) are left out of the lists

A Short is a **vertical** video (9:16). The app does not play them well (the
player is 16:9) and they are not what the person wants in a list, so they are
**discarded** from "A continuación", "Continuar viendo" and "Descubre".

**How they are recognized — and the trap of the thumbnails.** The first idea
was the proportion of the thumbnail, and it works for the VISIONOS layout
(`compactVideoRenderer` brings 320x180, 1280x720 and, for a Short, 1080x1920).
But in the **WEB layout the thumbnail is ALWAYS horizontal** (336x188) even
for a Short, so the proportion says nothing there. The reliable signal in
the WEB layout is the **shelf**: the Shorts come in their own
`reelShelfRenderer`, whose items are `shortsLockupViewModel`. So
`collectWebLockups` does NOT walk that branch: the shelf is skipped whole and
its items are never collected.

**The list has to be cut AFTER the filter, not before.** The WEB layout
returned 20 lockups of which 6 were reels, so the list came out with 14
videos. Now **40 are collected** and the cut to 20 happens after the filter.
If it still comes short of 20, the **next batch is asked** with the
continuation token (`requestRadioWebMore`, up to 4 batches): the person asked
for 20 videos, not for 20 rows of which some are reels.

**Where the filter is applied** (all in the source, so no surface forgets it):
the radio (the panel, the VISIONOS fallback and the WEB list), the
suggestions (`parseRelated`) and the Home shelves. The radio
that was **saved** in the database is filtered again when it is reused
(`MainWindow::openWatchAt`), because it may have been saved before the rule
existed.

**The "Continuar viendo" comes from the database**, so the flag is persisted:
`videos.is_short` (an `ALTER` for the old caches) and `VideoInfo.isShort`.

The pure part is in `src/reels_filter.*` and its selftest
`--selftest-reels` (runs in CI). `reels::thumbnailIsVertical` needs the
**measures**: without them nothing is discarded (a video is never dropped by
ignorance).

## "A continuación" mode (queue seeding)

The seeding mode is chosen with `NEVERTUBE_NEXT_MODE`:

- `radio` (default) → mix `RDAMVM<videoId>` of the clicked video (`/next` with
  `playlistId`, YouTube Music style); if the video does not bring a
  playback panel, the recommendations mix of the same `/next` is used.
- `suggestions` → static `/next`: ~20 recommendations of the current video.
- `none` → no seeding (the queue stays as it came from the entry).

`radio` does NOT auto-seed: the mix queue is generated only with an explicit click
from outside "A continuación" (`MainWindow::openWatch` → `WatchPage::triggerRadio`),
that is, when opening a video from search/home/channel/URL. Inside the list
"A continuación", clicking its items only plays (radio is not regenerated).
`suggestions` does auto-fill in `WatchPage::updateQueue` when ≤5 items remain and
each new video is regenerated. Innertube: `fetchRadio` (panel + mix),
`fetchRadioMore` (continuation, if the client emits it). The innertube
selftest covers the RDAMVM panel and the fallback.

**Already-saved radio (reuse without network)**: radios are saved COMPLETE in
`discover_radios`/`discover_radio_videos` when generated. If when opening a video
(`openWatchAt`, Radio mode) its radio is already saved, it is reused from the DB
for "A continuación" (`MainWindow` with `DiscoverRepo::radioVideoIdsForSeed` +
`VideoCacheRepo::getMany`) and YouTube is NOT asked again. Only
`WatchPage::triggerRadio` is called when there is no saved radio (or its metadata is not
in the cache). For Descubre, `hasRadio` already avoided re-asking.

**Radio single-flight is a QUEUE, not a drop**: `Innertube::fetchRadio` admits
ONE request in flight; a request for a DIFFERENT video is queued
(`m_radioQueuedId`, promoted by `promoteQueuedRadio()` at the terminal point
of the current radio, BEFORE the finished radio is emitted so the player's
request takes the slot ahead of the Descubre generator's next seed) instead of
being dropped. Without it, the first click of a fresh session collided with the
generator's in-flight seed — the previous grid video — and "A continuación"
stayed EMPTY (verified live: `radio single-flight id=zZb26...` swallowed by the
`radioWeb ok id=1YJJ...` that was already in flight). The generator is not
starved: each promoted fetch frees the slot at its own terminal point, so its
next seed simply becomes the new queue. Only a request for the SAME id already
in flight (or already queued) is dropped.

**Views and date of the queue (metadata)**: the `compactVideoRenderer` of the
VISIONOS client (the recommendations mix of `/next`) brings the views
as localized text (`"85.805 visualizaciones"`, `"1,2 M de vistas"`) and does NOT
bring a date. That is why:

- The views are read with `countFromText` (the own parser that was in
  `parseRelated` did `toLongLong` of the whole text and gave 0: the queue came out without
  views). The same in `parseWatchQueue` (panel), which did `toInt()` directly.
- The date of the modern lockup comes ABBREVIATED (`"hace 8 h"`, `"hace 3 d"`,
  `"hace 2 m"`): `relativeToIso` accepts those units (and the English
  `"2 months ago"`). Careful, `m` = months (measured: videos with millions of views cannot
  be from 2 minutes ago).
- The lockup gives the views WITHOUT the word "vistas" (`"21 K"`): `looksLikeCountText`
  recognizes them only if the text is number + multiplier (so a channel that
  starts with a digit, "3 Doors Down", is not confused with a count). The lockup
  also gives the CHANNEL (the first data that is not views nor date) and its id (in
  `metadata.image.decoratedAvatarViewModel...browseEndpoint`).
- So that the queue has a date, `handleRadio` does not emit the VISIONOS mix
  directly: it asks ONCE for the same `/next` from the WEB client
  (`requestRadioWeb`), whose modern layout (`lockupViewModel`) does bring views and
  date. If that request fails, the VISIONOS one is emitted (without date). It is an extra
  request, not the complete cascade (~12-15 s).
- **Refresh of old radios**: a radio saved by a parser that brought
  neither views nor date is detected when opening it (`openWatchAt`: channel + some
  metadata) and is refreshed ONCE; `onRadioReadyDiscover` then rewrites its
  list of ids (`clearForSeed` + `saveRadio`, because `saveRadio` does not overwrite).
  Thus it converges in a single opening and then reuses without network. The videos that
  YouTube returns without a date (live, premieres) do not force refreshes: the
  threshold is "some metadata", not "all".

- **Relative dates: declarative "metalanguage", not a cascade of `if`s**. The
  real bug (2026-10) was that videos from 2/4 weeks ago were painted as
  **"hoy"** on Home, Canal, Búsqueda, queue, playlists and Player. Cause: in
  `relativeToIso` the seconds branch used `unit.startsWith('s')` and
  **"semanas" also starts with 's'**, so `"hace 2 semanas"` was parsed
  as *2 seconds* → the date ended up 2 s from "now" → `format::publishedLabel`
  (`days < 1`) returned "hoy". The English ones ("2 weeks ago") did not fail because
  "weeks" does not start with 's'. The fix **is not** to shield that branch (that would leave
  the next collision for tomorrow); the parser was rewritten as a *metalanguage*:
  - **Language-agnostic extraction**: the first number (or the article
    "un/una/um/uma/a/an") whose next token is a recognizable unit is located. It does NOT
    look for `"hace"`/`"ago"` (the connector depends on the language). Thus "hace 2 semanas",
    "Emitido hace 6 horas", "2 weeks ago" and the Portuguese
    "há 2 semanas" work the same **without new code**.
  - **Declarative table `kRelativeUnits`** (hint → factor, in `src/yt_innertube.cpp`):
    the unit is resolved by **LONGEST match**, not by the order of `if`s.
    "sem" beats "s" because the `contains` of a 1-letter hint requires an exact
    token; adding a unit (e.g. "quincena") or a language is **one more row**,
    not touching the logic. The hints are compared **without accents** (`sinAcentos`, NFD +
    removing marks) to cover "día"/"mês"/"año".
  - **What is not recognized returns EMPTY** (the UI does not invent a date) instead of
    falling back to a default unit — which was what produced the false "hoy".
  - The classifier of `metadataParts` of the WEB lockup (`videoFromWebLockup`) no
    longer **filters by `t.contains("hace") || t.contains("ago")`** (it was a second
    copy of the parser and in pt/another connector the date did not get classified and the
    text fell into the channel name): now `relativeToIso(t)` is the **only
    authority** of "is this a date?".
  - It is fixed by the offline selftest `--selftest-dates` (`Yt::relativeDateSelfTest`,
    `src/yt_innertube.cpp`): **31 pure cases** with a 5 s tolerance (weeks/
    days/hours/minutes/seconds/months/years, abbreviations `h/d/m/s`, prefixes
    "Emitido…", Portuguese with "há" and accents, English with `a/an`, texts that are not a
    date and **unknown units → empty**). The test catches the regression: without the
    "sem" row the 4 weeks cases fail. It goes in the smoke of BOTH CIs.
  - It is the function used by the 6 network `publishedAt` assignments.

## The three playback paths (and the one that is only a backup)

There are three ways a video reaches the screen, and they are NOT equivalent.
Measured on a real day with a fresh quota (2026-10-19):

| path | when | container | who fragments it | seek |
|---|---|---|---|---|
| **synthetic HLS** (AV1) | profile `av1-hw` | fMP4 + `sidx` | **we** do, with the real index | **32 ms, measured** |
| **YouTube's master** | profile `vp9-hw` / `h264-hw` | YouTube's own | **YouTube** does | works (it is a normal HLS) |
| **edge** (direct adaptive) | ONLY when the master fails | adaptive | nobody | works |

Verified live, one profile at a time, with the master reachable:

| profile | route used | decoded |
|---|---|---|
| `av1-hw` | `av1-hls emit 1080p` | av1 |
| `vp9-hw` | `hls filtrado a 1080p` | **vp9** |
| `h264-hw` | `hls filtrado a 1080p` | **h264** (QSV) |

The master filter honours the profile: `buildFilteredMaster` scores each
`#EXT-X-STREAM-INF` line with `codecRankedScore` (the line carries the `CODECS=`
attribute, so the codec is really read). YouTube's master offers **9 VP9
variants** (`vp09.00.10.08` … `vp09.00.51.08`) and 7 h264 ones, and **no AV1**,
which is exactly why AV1 needs the synthetic HLS at all.

**The master is the primary path and it is already fragmented by YouTube.**
That is why the master measures −0 % of quota, and why the "VP9 cannot be
fragmented" question only ever concerned the adaptive/DASH form, which reaches
the edge and nothing else.

### The throttle is a PER-REQUEST budget of ~8 MB, not a quota on the IP

This is the mechanism that explains every path above. Measured with `curl` over
the same keep-alive connection, same video, fresh URLs:

| requested range | speed |
|---|---|
| 2 MB | 5,18 MB/s |
| 4 MB | 5,62 MB/s |
| **8 MB** | **14,30 MB/s** |
| **16 MB** | **0,36 MB/s** |
| 32 MB | 0,37 MB/s |
| **no `Range` (sustained)** | **0,36 MB/s from the first byte** |

So a request transfers ~8 MB at full speed and **then** drops to 0,36 MB/s, and
a sustained read never gets a fast stretch at all. The synthetic HLS works
because its fragments are ~2 MB and never reach the ceiling; that is also why
it needs a different `&nt=<n>` per segment (without it ffmpeg merges the
contiguous ranges into ONE request and the throttle returns).

**None of the three paths burns the IP.** Each arm ran 60 s and the health was
measured before and after with the same probe (8 MB ranges, median of 4):

| arm | moved in 60 s | health before | health after |
|---|---|---|---|
| control (idle) | — | 11,45 MB/s | 16,43 MB/s |
| master (YouTube's segments) | 200 MB | 16,43 | 11,58 |
| synthetic HLS (2 MB ranges) | 206 MB | 11,58 | 16,36 |
| **edge direct (sustained)** | 21 MB | 16,36 | 9,62 |
| h264 sustained | 45 MB | 6,03 | 6,69 |
| h264 2 MB ranges | 114 MB | 6,69 | 6,05 |

(MB/s. The probe's own noise floor is 8,4 – 17,2 MB/s with occasional failed
probes, so only a drop **to 0,36** would be meaningful. None of the arms
reached it.)

And the app itself, on the edge, with **its own** URLs (master unreachable,
`vp9-hw`): 2048 ticks, largest gap between ticks **786 ms** (the watchdog cuts
at 5000), zero gaps ≥3 s, **0** `streamStalled`, **0** `streamInterrupted`,
`paused-for-cache=0`, audio `48000Hz stereo`, vp9 1920x1080, 132 s of content
played. mpv asks by range, so the sustained-read throttle of a raw `curl` does
not touch it.

**NOT attributed:** YouTube's serving rate fell from ~16 MB/s (08:49) to a
**stable 6,3 MB/s** (09:22) and did NOT come back after 60 s idle, against
21 – 67 MB/s from a neutral host (Cloudflare). 6,3 MB/s is 17× above the 0,36
floor and does not affect playback, but **whether the tests caused it or the
server allocation did could not be decided with that evidence**. Do not claim
it either way.

### The edge is a backup and it never fires on its own

The edge (direct adaptive URL) is entered ONLY when the master is not
filterable or the master fetch fails. **In every real session it has never
triggered by itself** — the only times it was seen were when the master was
broken on purpose to force it. It is unconditional (no env switch): when the
chosen format cannot be fragmented at all it is the only thing that can play
it.

That is the whole trade: the edge reads the adaptive URL as **one sustained
download**, which is what the throttle punishes (0,36 MB/s), but it is a path
that almost never runs, and **the seek works** (which is why the fragmented
edge was dropped: it traded the seek for bounded requests).

**Rule for a future change:** do not "improve" the edge by fragmenting it.
A fallback that plays and cannot be seeked is worse than one that plays in a
different codec, and the whole point of the edge is that it is the last thing
that still works. If a fragmented edge is ever revisited, it must select a
format that carries a real index (mp4 with `sidx`), and the seek must be
measured on the app — not inferred.

### The rule that was missing: look at the CONTROL when it fails

Four measurements in a row were wrong, and the last three failed for the same
reason: **the control was not looked at when it failed too.**

1. "37 % of the requests are throttled" — the IP was already degraded; "slow"
   was the dead IP, not the requests.
2. "VP9 has no box in its indexRange" — there IS a `Cues` box, of 13 bytes:
   it exists and it is empty.
3. "WebM cannot be fragmented" — the test playlist was served over `file://`,
   so the demuxer refused the https; **the h264 control failed identically**
   and that was read as a format limit.
4. "the seek measurably fails on fixed chunks" — those were HTTP 403 from
   expired URLs, detected by grepping for the word "Failed".

The corrected reading: WebM **can** be served as a playlist (mpv decodes a WebM
init section as `#EXT-X-MAP` plus fixed `BYTERANGE` chunks without complaint),
but it carries no usable index, so a playlist built over it plays and refuses
to seek. **Serviceable ≠ seekable** are two different questions and need two
different checks. Before believing a "the format cannot" or "it measurably
fails" conclusion, run the control **and read what the control did**.

## The downloader (button on the Player, tab in the bottom bar)

The app can now **fetch** a video, not only watch it: the Player has a
download button and the bottom bar a "Descargas" tab with the list, the
progress and the state. The design is in
`docs/superpowers/specs/2026-10-19-downloader-design.md`.

### Where the bytes come from

The **adaptive formats** of the player response, fetched **by byte range**
and in parallel.

The reason is the throttle: YouTube throttles a request **by its size**.
Measured on the same audio resource, asked for whole (3,87 MB) it runs at
**0,011 MB/s**, and a 2 MB range of the very same resource runs at
**14 MB/s**. A thousand times. So nothing is ever asked for whole: the
sidx of each resource (the same index the synthetic AV1 HLS of the player
already reads) gives the exact boundary of every fragment, and the
fragments are fetched in parallel. A track with **no** index (a WebM/Opus
one: its clusters are not indexed) is cut in pieces of 2 MB instead.

The unit is the **FRAGMENT**, and the progress is by fragments: the sizes
of the fragments of a resource are not uniform, so a fraction of the bytes
would make the bar jump backwards.

The size of the download is known **before** it starts and it does not
move: it is the `contentLength` of each resource (the sum of the two
tracks). The estimate of the `BANDWIDTH` (which is the PEAK of the
variant) was about five times the real size, and it made the bar jump
backwards as it was corrected.

The **HLS master is not used**: its audio arrives as AAC in LATM (its
segments carry a PRIV box of Apple that says so) and the FFmpeg of this
system refuses to mux it ("Error applying bitstream filters to an output
packet for stream #1: Not yet implemented in FFmpeg"). The adaptive gives
both tracks as clean mp4.

The consequence is that the file is an **MKV** and not an mp4: Matroska
takes the track as it is, and an mp4 would need the conversion that does
not work here. It plays everywhere (measured with mpv and with ffmpeg).

Measured, against YouTube, on a fresh quota:

| | | |
|---|---|---|
| 1080p | av1 1920x1080 + aac | 122,2 MB in 10 s |
| 720p | av1 1280x720 + aac | 18,0 MB in 6 s |
| solo audio | mp3 173 kbps | 13,1 MB in 10 s |
| 240p | av1 320x240 + aac | 0,3 MB in 2 s |

The fragments of the master arrived at 9,44 MB/s with one connection and
43,12 MB/s with four, against 0,37 MB/s for a sustained read.

The **height wins over the codec** for a download, the OPPOSITE of the
player: the player ranks the codec first because it wants the one the GPU
decodes, but a download wants the best picture.

### The four things the real master taught (each was a bug first)

- **The response is CHUNKED.** YouTube answers the segments with
  `Transfer-Encoding: chunked` and NO `Content-Length`, so the chunks have
  to be decoded, and the decoding has to be **INCREMENTAL**: the response
  arrives in many pieces and decoding it again from the start duplicates
  the bytes.
- **The server closing the connection is not an error.** With
  `Connection: close` Qt reports the close as an error, and failing there
  threw away the bytes that had already arrived.
- **An fMP4 variant has an `#EXT-X-MAP`**: the init segment ("moov") is a
  separate URL and it goes ONCE at the front of the file. Without it
  nothing can read the file (`ffmpeg`: *"trun track id unknown, no tfhd
  was found"*).
- **The master does not always mark the original audio.** When the video
  has a single language there is no `acont=original`, and the two
  renditions (itag 233 = 48 kbps, itag 234 = 128 kbps) are only told
  apart by the bitrate. Picking the wrong one downloads 48 kbps audio for
  a 1080p video.

And two that are not about the master:

- **The `BANDWIDTH` of the master is the PEAK** of the variant, not its
  average: the estimate was 380 MB against 179 MB real. The real total
  comes from the segments that are on disk.
- **A null `QString` is bound as a NULL in SQLite**, and the NOT NULL
  columns of `downloads` rejected the row: the download finished and the
  list stayed empty. The text is bound as an empty string.

### The muxing is the ffmpeg of the SYSTEM (and on Windows, of OUR build)

The vendored FFmpeg had **no muxer**: it was built with `--disable-everything`
and only demuxers, and `avformat_write_frame` was not even exported, so it
could read an mp4 but not write one. The mux is done with the `ffmpeg` found
at runtime (`QStandardPaths::findExecutable`, plus `NEVERTUBE_FFMPEG`).
**Without ffmpeg the download still works**: the segments are downloaded
and kept, and the mux is a separate step that can be retried. The mux
itself is a stream copy (`-c copy`, plus `-movflags +faststart` so the
moov is at the front): 0,05 s for 30 s of video, no re-encoding.

**The CLI, not the libraries.** The app links libav for the DECODER (the
x86 build); the mux is a separate step that launches the CLI.

- **Linux**: the packages declare the CLI as a dependency OF ITS OWN,
  because `dpkg-shlibdeps` and rpm's automatic generator only see the
  SONAMES the binary links (`libavformat.so`), never the `/usr/bin/ffmpeg`
  that the downloader runs. The .deb declares `ffmpeg`, the .rpm declares
  `/usr/bin/ffmpeg` (BY PATH: Fedora's `ffmpeg-free` and RPM Fusion's
  `ffmpeg` both provide it and they CONFLICT with each other, so requiring
  one by name would break the install of the people who have the other) and
  the PKGBUILD already had `ffmpeg`.
- **Windows**: there is no public ffmpeg for 32-bit Windows (gyan and
  BtbN are x64 only and MSYS2 retired i686), so the vendored package
  (`third_party/ffmpeg-win32/`, cross-compiled by
  `tools/build_ffmpeg_i686.sh`) is compiled **with the muxers and the CLI
  enabled**: `bin/ffmpeg.exe` (405 KB) next to the DLLs that were already
  there (no new binary to download and no new licence: it is the same
  LGPL build). The workflow copies it into `dist` and the setup gate
  requires it. The gate of imports Win7 covers the EXE too (it also runs
  on that machine).
  The **x64** build gets the same thing from the same recipe
  (`tools/build_ffmpeg_i686.sh <dest> x86_64`, the arch is the second
  argument): `third_party/ffmpeg-win64/`, whose CLI is 431 KB. The x64
  build plays with mpv and does NOT link libav: the DLLs of that package
  are the ones the CLI needs, not the ones the app uses. The names do not
  collide with `libmpv-2.dll` (it has libav inside, statically) because
  the CLI is a separate process.

**The mp3 encoder IS there**: LAME (`libmp3lame`) is **LGPL v2**, NOT
GPL, and FFmpeg's own list of the GPL libraries does not carry it, so the
LGPL build can use it. The recipe compiles it as a DLL
(`libmp3lame-0.dll`) and FFmpeg links it: the "solo el audio" download
is an mp3, as it was. (I had it wrong at first and believed LAME was GPL:
it is not.)

**The fall-back to an M4A** is still there, for the case of an ffmpeg of
the SYSTEM that has no mp3 encoder (a very trimmed one): the audio is
then COPIED into an M4A (the same AAC, no re-encoding). It is decided by
`hasMp3Encoder()` (`ffmpeg -encoders`), so it is one call and not two
runs, and the failed attempt does not leave a corrupt `.mp3` behind.

**The search of the ffmpeg**: the env var (`NEVERTUBE_FFMPEG`), the PATH,
**the directory of the app** (a build that SHIPS its own ffmpeg has it
right next to the exe and the PATH of a desktop session does not include
the install directory) and the usual places.

### The resume, and the cleanup

Each segment goes into its own file inside a temporary folder **as it
arrives**, so an interrupted download keeps the bytes and the index of the
segments that are done is saved in the repository. Starting again skips
them (measured: with 77 of the 246 segments already on disk, the resume
fetched **1** and finished the file).

The encoding of that index had a bug: the video segments are their own
index (0, 1, 2...) and the audio ones `-(index + 2)`, so the 0 of the
video and the 0 of the audio do not collide and **neither is lost**.

The partials and the temporary folder are deleted when the mux works (they
are twice the size of the file: 171 MB of partials for a 171 MB mp4).
When the mux FAILS they are NOT deleted: the bytes that were downloaded
are still there.

### Where the files are

`QStandardPaths::DownloadLocation` (the folder of downloads of the
SYSTEM, which comes already translated: "Descargas", "Downloads") plus a
`NeverTube` subfolder, so the files are not mixed with the ones of the
user. It is the default; the path can be changed in Ajustes.

### The local playback does NOT go to the history

`PlayerController::playLocalFile()` plays a file that is already on the
disk, without the resolver (asking YouTube for a video that is already
downloaded is wrong, and it fails when the video is not available any
more). The position lives in the `downloads` table, not in the history:
that is about YouTube videos, and `HistoryRepo`/`onDurationKnown` assume a
YouTube id.

### The transport is its own

`Ipv4Https` (the transport of the app) is a single socket with a FIFO
queue: it cannot download in parallel, and its callback is (status, body),
so it exposes no response header and cannot send a Range. The downloader
has its own (`src/downloader/download_transport.*`), with N simultaneous
connections, the headers exposed and the IPv4 forced (the same reason
`Ipv4Https` does it: YouTube blocks some videos over IPv6 from this
network).

### The codec of the download (the submenu)

A video has **several variants per height** (AV1, VP9 and H264) and they are
not the same picture: the AV1 is smaller for the same quality, but the H264
is the one that opens anywhere. So the **resolution opens a SUBMENU** with
the codecs of that resolution, each with its **real size**:

```
1080p HD  ▸
720p HD   ▸
──────────────
Solo el audio (mp3)
```

and inside "1080p HD ▸": `VP9 · 761,1 MB / AV1 · 874,4 MB / H264 · 1001,9 MB`.

**Clicking the resolution ITSELF still downloads** the default codec of it
(the smallest one), so nobody is forced to open the submenu.

**The sizes are not a guess**: they are the `contentLength` of the player
response, which the app ALREADY has (the `kFieldMask` asks for
`streamingData.adaptiveFormats`), so the menu does not ask YouTube for
anything. The signal is `Yt::streamVariantsReady` (the variants: height,
codec and real size) → `WatchPage::m_variants` → the menu.

**The AV1 is not always the smallest**: in `7wVjjtpWWZs` the 1080p is 761 MB
in VP9 and 874 MB in AV1 (YouTube encoded the VP9 with a more aggressive
CRF). That is exactly why the submenu shows the size: the choice is
informed, not assumed.

**The types** live outside the two subsystems that use them
(`src/video_variant.*`), so the player and the downloader do not depend on
each other: `VideoVariant` (height, codec, real size) and `videoVariantsOf()`.
The pure part has its own selftest, `--selftest-variants` (runs in CI).

**The plan** (`src/downloader/segment_plan.*`) has one option per
**(height, codec)** and `planWithVariant(plan, height, codec)` picks the
exact track. The default (no codec chosen) is the **smallest** variant of
the highest resolution: for a download the size is what matters.

**The audio is chosen with ONE rule** for the player and for the downloader
(`preferredAudioFormat` in `video_variant.cpp`): the original track (or the
preferred language), the mp4 (with a sidx, so it can be fetched in pieces)
over the WebM (without one), and the smallest among those. Before there
were two copies of that rule (one in `segment_plan.cpp` and one in
`yt_innertube.cpp`): the size the menu showed was not always the one that
got downloaded.

### The i18n of the downloader

Every text that the person reads goes through `nt::tr` with a KEY (never the
Spanish text). That includes the **error messages of the downloader**: they
are shown as they are in the row of the page of Descargas, so they are
`error.dl*` keys (19 of them: "ffmpeg no encontrado", "no se pudo escribir
el vídeo", "ya hay una descarga en curso"...). Before they were Spanish
literals inside the downloader: in English the page said "ffmpeg fallo" in
the middle of an English row.

**The sizes and the resolutions use the SAME functions as the rest of the
app**: `format::humanBytes` ("916,8 MB") and `format::resolutionLabel`
("720p HD"). Before the downloader had its own copy of the size formatter
and the player had its own copy of the resolution label: three copies of the
same two functions. Now they are in `src/ui/format.*` and the selftest
checks the exact text (so the size of the menu and the size of the page
cannot diverge).

**The numbers come out with a POINT** ("874.4 MB"), not a comma: the app does
not set a `QLocale` and never did. It is the same in every language (and the
same as the "1.2 M" of the views), so it is a convention of the app, not a
miss.

### The settings of the downloader (Ajustes → DESCARGAS)

There is a **DESCARGAS** section in Ajustes with the two settings of the
downloader:

- **La carpeta de las descargas**: the folder where the files are written.
  It is a PATH and not a code (it is a place of the file system). Empty =
  the default one (the folder of downloads of the SYSTEM plus the subfolder
  "NeverTube"). The row opens a menu with the current folder, "Cambiar"
  (a `QFileDialog::getExistingDirectory`: the ONE place where a system
  dialog is right, because choosing a place of the file system cannot be
  invented) and "Restablecer" (back to the default).
- **Conexiones simultáneas**: how many parts the app downloads at the same
  time (1 to 16, **8 by default**). It is the only setting of the downloader
  that the person NOTICES (the speed) and that had no menu. 8 is the
  measured optimum (4 -> 15,23 MB/s, 8 -> 17-24, 12 -> 20,3 but not
  reliable, 16 -> **3,69 MB/s**: the connections start to fight each
  other). It is saved in `download_connections` and it is applied to the
  pool of transports of the job (`DownloadJob::setConcurrency`).

**The submenu of the download marks the RECOMMENDED codec** — the one that
**this PC can reproduce**, from the hardware profile (`av1-hw` -> av1,
`vp9-hw` -> vp9, `h264-hw` and `soft-all` -> h264) — and it says why:

```
☑ VP9 · 725,8 MB · Recomendado para tu PC
  AV1 · 833,9 MB
  H264 · 955,5 MB
```

The click on the resolution downloads the recommended one. When the
resolution does not have it, the smallest one of that resolution is the
marked: the recommendation never breaks the download.

**There is NO setting of the codec** (there was one and it was removed):
the submenu already lets the person choose the codec, so a setting that
only decides WHICH one is marked was redundant.

**The download is NOT capped by the PLAYER.** The cap of the resolution of
the download used to be `m_player.maxHeight()`: with the player at 360p the
menu offered "1080p HD" and the download brought the **360p** (the plan
only had the options up to the cap, and the one that was picked was not
there). The download is a FILE: the menu shows the resolutions the video
really has and the plan is built **without a cap** (`maxHeight = 0`).

**The folder is ONE function** (`downloads::folderOf`): the downloader writes
there, the page of Descargas opens it and the setting of Ajustes shows it.

### The button of "Reintentar" (the resume was lost, twice)

**The retry DELETED the row of the repository** (`remove()`), and with it the
index of the fragments that were already on the disk (`downloads.parts`).
The download then started from zero: the bytes that had been downloaded were
thrown away, and the fragments of the `.parts` folder were left behind
(hundreds of MB of orphans).

**And the row was erased a SECOND time**, by a line that looked harmless:
`DownloadManager::start()` builds a FRESH `DownloadInfo` and upserts it, and
the upsert is an `INSERT OR REPLACE`: the new row had no `parts`, so the
index was gone even when the retry did not delete the row. Now `start()`
**reads the row first and keeps it** (only the fields that this call owns
are written).

**And a third time**, by the first `saveProgress()` of the resumed job: the
flags of the fragments are marked by `startNext()`, and the progress is
saved before that happens, so the index was written EMPTY. Now
`DownloadJob::doneSegments()` also returns the fragments that were already
on the disk (`m_alreadyDone`): they count as done from the first moment.

**The retry is now `DownloadManager::retry()`** (it does NOT delete the row)
and the button calls it. The fragments that were half written are discarded
by the job itself (it checks the size of each one).

**And `remove()` now deletes the FOLDER of the fragments** (the `.parts`
one): the bytes of a deleted download were staying on the disk.

### The resume asked for NOTHING (the download stayed still)

The loop that gives a fragment to each free transport gave **ONE** fragment
per transport and went to the next transport when the fragment was already
on the disk (`continue`). With the **first eight fragments already
downloaded** (the resume), the eight free transports found their fragment
already done, the loop asked for **NOTHING**, and the download stayed still
**FOREVER**: nothing was in flight, so nothing called `startNext()` again.

Now the transport takes the **next fragment that is missing** (the loop
inside the transport goes on with the following ones), so a resumed download
asks for the fragments that are not on the disk and keeps going.

Measured after the fix: a download that was cut at 236 MB continued at
**30-33 MB/s** (about 264 Mbps) from the byte it was at.

### A fragment that fails is asked AGAIN (and the resolution is kept)

A fragment that fails (a timeout, a connection that dies) **failed the whole
download**. With a saturated IP some requests do not answer at all, and one
of them was enough to abort hundreds of megabytes. Now the fragment is asked
again (up to `DownloadJob::kFragmentRetries` = 3) and only after that the
download really fails.

**The resume keeps the RESOLUTION of the row** ("720p" -> 720): the person
paused a 720p and it is not a 4K what they get when they continue (it was
the highest one of the video, because the resume did not pass any height).

### The signed URL runs out (HTTP 401) and the download died

The URLs of the player response carry a **per-request budget**: after a
while the CDN answers **401/403** and every fragment fails. The download
used to die there (and it was the most common failure: it is what the
"fallo al bajar un fragmento" of the log was).

Now a 401/403 is **not an error of the video**: the job emits `urlExpired()`,
the manager drops the cached formats (`requestAdaptiveFormats(id, true)`)
and asks the player response again, which brings **new URLs**, and the
download continues from the fragments that are already on the disk. It is
the same thing the app does with a gated session of the player.

**The fragment that fails is asked again** too (up to
`DownloadJob::kFragmentRetries` = 3): with a saturated IP some requests do
not answer at all, and one of them used to abort hundreds of megabytes.

### What actually limits the speed of the download (measured, 2026-10-21)

The speed of the download is **not** the codec and **not** the number of
connections. It is the route that the URL comes from, and there are two:

| route | what it is | speed | scales with the connections? |
|---|---|---|---|
| **adaptive** (`gir=yes`) | the one the app uses: the `adaptiveFormats` of the /player, by byte range | **43 MB/s** (x8) | **NO**: x32 gives 27 MB/s |
| **master HLS** (`ratebypass=yes`) | the playlist of the master: its segments are small (0,18 MB) | **72 MB/s** (x48), **91 MB/s** (x96) | **YES**: it reaches the ceiling of the line (88 MB/s to cachefly) |

**The master is the faster one** and it is the one that carries
`ratebypass=yes` **signed** in its `sparams` (the adaptive has the pacing
`initcwndbps` signed instead, in its `lsparams`). The master is NOT usable
for the download as it is: **its audio does not mux** (it is AAC in ADTS
without timestamps, and the ffmpeg 9 refuses it: "Can't write packet with
unknown timestamp"). The video of the master would be fine (its segments are
fMP4).

**BUT the master is the one that BURNS the IP, and that is why the hybrid
is NOT worth it.** Measured with the same video, the same host and the same
connections (48-96):

| what was downloaded just before | the direct route (master) right after |
|---|---|
| **615 MB with the ADAPTIVE** (the app) | **9,0 - 9,6 MB/s** (untouched) |
| **612 MB with the DIRECT route** (the master) | **0,3 MB/s** (strangled) |

The direct route asks for a lot of volume at once (96 connections of
0,18 MB) and the CDN punishes it; the adaptive asks for 1 MB pieces with a
signed pacing and it is left alone. **The adaptive is the POLITE route, and
the polite one is the one that keeps the IP healthy** (it is also the
reason the AV1 "looks" faster: it is smaller, so it spends less time in
the part where the CDN has already squeezed the IP).

So the 91 MB/s of the master is a loan: it is paid back with an IP that
does not work for the player nor for the next download. **The adaptive
(the one the app uses) is the right choice**, and its ceiling (~50 MB/s
with 16 connections) is the real one.

**The codec does NOT change the speed.** Measured with the app, the same
video at 1080p:

| | 1 s | 10 s | 22 s |
|---|---|---|---|
| VP9 (836 MB) | 48,2 MB/s | 37,9 | 19,7 |
| AV1 (961 MB) | 48,4 MB/s | 37,9 | 19,0 |

The curves are **identical**: the AV1 only *looks* faster because it is
smaller (it spends less time in the part where the CDN has already squeezed
the IP). The H264 (the biggest) is the one that ends the worst (7,3 MB/s).

**The degradation (48 -> 20 MB/s in 30 s) is the CDN**, not the app: the
same host that gives 45-90 MB/s at the beginning gives 20 after a few
hundred MB, and it does not recover with pauses (measured: 30 s, 60 s and
120 s of pause, the same 0,1-4 MB/s).

**The pacing cannot be touched**: `initcwndbps` is signed (`lsparams`),
and changing it answers **403** (measured). The same with `ratebypass=yes`
added by hand.

**The other clients do NOT give a URL** (and there is nothing to decipher):
the ANDROID and the IOS return the `adaptiveFormats` with **no** `url`, no
`signatureCipher` and no `n`: they force the SABR path. Only the
"desktop" ones (the VISIONOS that the app uses, and the ANDROID_VR) return
the direct URL. The ANDROID_VR returns one that the CDN does not serve
(0 MB/s).

### A fragment that does not advance is discarded and asked again

The CDN serves **some** of the requests at a crawl (a few KB/s). Those
hold a connection of the pool for the whole request (the timeout total is
60 s) and the speed of the download falls: it was measured starting at
**45 MB/s and settling at 20**.

The transport now **discards** a request that does not advance and the job
asks that fragment again (it is the same rule that ABDM uses). The rule is
**not** "not one byte in N seconds" (that also killed the fragments that
were arriving fine but with a gap in the middle): it is "**not even the
10% of the expected bytes in 12 s**" (`DownloadTransport::stallTimeoutMs()`
and `stallMinRatio()`). A fragment that advances, even slowly, is left
alone.

The size that is expected is the one of the range that is asked
(`range.size()`), so the threshold is proportional to each fragment.

### The delete of a download (a waste basket and a check box)

The X of a row of the page of Descargas is a **waste basket** (`nt::Icon::Trash`,
drawn with the painter like the other icons) and it **asks before deleting**:

- **In course or paused**: it warns that it cancels the download and deletes
  what was already downloaded. There is no check box: the partials always go
  with it.
- **Finished**: it asks with a **check box** ("also delete the file from the
  disk"), UNCHECKED by default, so the action of a single click is the safe
  one (the file stays). It is the same shape that ABDM uses.

The check box is a new capacity of the confirmation overlay
(`nt::confirm` takes a `checkText` and a `bool *checkState`): the state is
only written when the dialog is ACCEPTED.

**The app only deletes its own files**: `DownloadManager::remove` checks that
the path is INSIDE the folder of the downloads (`isInsideDownloadFolder`). The
path comes from the repository and a row could name a file anywhere (a stale
row, a hand-edited database), so a file outside that folder is never touched
(there is a selftest for it).

### The button of "Abrir carpeta" (two bugs, one button)

The button of the "Descargas" page did **nothing at all**, for TWO reasons
that had to be fixed together:

1. **The path came from the wrong place.** It took the `filePath` of the
   FIRST ROW of the list, and that is the **partial** file of a download in
   progress (a `.ts` per track) — which the new downloader does not use
   anymore, so the column is **empty**. The fix: the button opens the
   folder of the **downloader** (`downloads::folderOf`), the one where the
   files are written.
2. **`QDesktopServices::openUrl` returns FALSE** in the desktops that have
   no handler registered for `file://` (measured: it returned false for a
   folder AND for a file, in the same desktop where `xdg-open` opened
   nautilus without trouble). The fix: when it fails, the file manager of
   the session is called directly (`downloads::openCommandFor`): `gio open`
   first (it is the freedesktop mechanism and it respects the preferences of
   the session) and then the known file managers, one by one.

The folder of the downloads is now ONE function
(`downloads::folderOf`, in `src/downloader/download_folder.*`), used by the
downloader AND by the button: the size that the menu shows and the folder
that it opens cannot diverge. (There is **no setting** for the folder yet:
the one of Ajustes does not exist, and the default one is used.)

The pure part has its own selftest, `--selftest-download-folder` (runs in
CI).

### The selftests

- `--selftest-download`: the plan is a pure function of the master
  (27 cases, offline).
- `--selftest-download-button`: the button of the REAL window (11 cases):
  it exists, it has a tooltip AND an accessible name (an icon-only button
  has no text: those two ARE its label), and the row it queues lands in
  the repository with the state "pending".
- `--selftest-variants`: the variants of a video (the family of the codec
  and one variant per (height, codec) with its real size), offline.
- `--selftest-download-settings`: the codes of the settings of the
  downloader, offline.
- `--selftest-settings-page`: the REAL page of Ajustes (the new section of
  the downloads is there).

The two CLI entries `--download <id>` and `--play-local <file>` are how
the downloader is driven without touching the UI.

### CPU: the download yields to playback, and it is recoverable

The report (Lenovo, W11, x64, `h264-hw`): with a download running and a video
playing, **the FPS dropped from the FIRST second of the download**, before any
fragment had been muxed, so the mux was not to blame. Chrome does not lose those
frames doing the same thing: it hands the real-time threads a better position in
the scheduler than the ones of the download.

The downloader never lowered anything: the transport thread and the finalize
thread ran in `NORMAL`, exactly like the thread of the interface (the one that
runs `paintGL` of the `QOpenGLWidget`). With the same priority Windows divides
the CPU by time-slicing, so the thread that has to present a frame every ~16 ms
waits behind eight sockets decrypting TLS. That is the mechanism, and it is
independent of the total CPU: it costs frames even with free cores.

**What is done (nothing else):** while video is playing, the download's threads
run **below normal**. The concurrency of the download is **NOT touched** (it stays
at the value of Ajustes): lowering it would punish the machines that can do both,
and with the throttle measured (~8 MB per request) 8 -> 4 barely changes the
final speed but for a strong PC it is a loss for no reason. Priority is the
lever, because it is the one that only costs something when there is actually
contention.

- **Which state counts as "playback"**: `playing` and `loading` of the
  `PlayerController` (`MainWindow::updateDownloadPlaybackState`, called from
  `phaseChanged`, from `currentChanged`, from the page change of the
  `QStackedWidget`, from the exit of full screen and at the end of the
  constructor). It covers the Player page, the full screen and the **mini
  player**: the three paint frames. A `paused`, `stopped` or `error` video does
  **not** count.
- **It is fully recoverable and nothing is persisted**: the state is derived
  from the player on every change. Closing the mini player (or pausing, or the
  video failing) restores `NORMAL` **immediately**, with no timer, no delay and
  no saved flag: the next call to `setPlaybackActive(false)` either posts or
  applies the restore.
- **It changes WHILE the download runs**, which is the case that matters:
  `DownloadJob::setPlaybackActive` posts the change to the thread of the
  transports (`QMetaObject::invokeMethod` on `m_home`, queued), so a download
  started before pressing play also cedes CPU. Applying it only at
  thread-start would have missed exactly the reported case.
- **Per thread, not per process**: `applyThreadPriority` (`download_job.cpp`,
  the anonymous namespace) does `SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL)`
  on Windows and `nice 10` on Unix. The process is not touched: the interface,
  the player and the audio keep `NORMAL`. The finalize thread (the assembly of
  ~1 GB plus the `ffmpeg` of the mux) reads the same state when it starts.
- **`m_playbackActive` is `std::atomic<bool>`**: the pool thread and the finalize
  thread read it while the thread of the interface writes it.
- The `ffmpeg` of the mux runs **at normal priority** on purpose: it is a
  `-c copy` (0.05 s for 30 s of video) and the process priority of a child is
  inherited from the process, not from the thread that launches it. Lowering it
  would be speculating without a measurement; if the Lenovo shows frames lost
  during the mux, that is the next lever (a `BELOW_NORMAL_PRIORITY_CLASS` in the
  `QProcess` of `runFfmpeg`).
- **The next lever, if priority alone is not enough**, is the concurrency, and
  only under measured pressure (CPU high with playback + download), never by
  default: the aim is not to punish a PC that can do both.
- **Kill switch for the measurement**: `NEVERTUBE_NO_DL_LOWPRIO=1` makes the
  downloader ignore the playback state entirely (it behaves exactly as before
  this feature). It exists so a report of "player + download is slow" can be
  told apart from "the priority change is to blame" on the machine that
  reports it, instead of guessing.
- **A gotcha this already produced**: ANGLE was made the default on x64 in the
  same batch, and it looked harmless for a while because the D3D shader
  compiler was reaching the app with the WRONG NAME: the startup died before
  ANGLE could run, so the "it works" of the first test was the default OpenGL.
  With the DLL fixed, ANGLE really activated and **the player + download went
  unresponsive** on the Lenovo. It is back to opt-in (`NEVERTUBE_ANGLE=1`),
  which is what this document always said. Lesson: a feature that cannot run
  because of a missing file is NOT a feature that works.

## AV1 over HLS (synthetic)

YouTube **does not publish AV1 in its HLS master** (`hlsManifestUrl`): only VP9/h264
(verified: every `av01` of YouTube is `https`/DASH, it does not appear in the HLS master). In
addition, the direct adaptive AV1 URL ("edge" mode) is **throttled** (~0,36 MB/s
measured) because the throttle is **per request** and a continuous download
exhausts its budget on the first byte. On the other hand, each short `Range`
request (~2 MB) is not throttled, even over the same keep-alive connection
(≈14 MB/s). The full mechanism, with the numbers, is in "The three playback
paths" below.

`Yt::buildAv1Hls` (profile `Modern`) synthesizes a local HLS over the
`adaptiveFormat` AV1: fMP4 media playlist with `#EXT-X-MAP` (from `initRange`) and
one `#EXT-X-BYTERANGE` segment **per real fragment of the fMP4**. Keys for
it to work:

- **the boundary of each segment comes from the `sidx`** (box of the `indexRange`, ~4
  KB): `buildAv1Hls` downloads those bytes with `fetchRangeBlocking` and
  `av1ParseSidx` gives exact size+duration per subsegment. It is
  essential: with fixed chunks (the old ~2 MB fallback) sequential
  playback works, but **the seek falls in the middle of a fragment and
  mpv does not load video** (`Cache: 0.0s` + `Errors when loading file`, without
  `VO:`). Verify with `mpv --no-config --vo=null --ao=null --frames=2
  --start=<t> <m3u8>` for several `t`;
- path suffix `/seg.mp4` after `/videoplayback` (YouTube ignores it) so that
  the HLS demuxer accepts the extension without `extension_picky`/whitelist;
- a different `&nt=<n>` per segment: without it ffmpeg **merges** the contiguous
  ranges of the same resource into a single request and the throttle returns;
- the writing of the `.m3u8` is atomic (`.tmp` + `rename`): the recovery
  rewrites the same path while mpv may be reading it;
- separate audio (`PlayerWidget::setAudioFile`, like edge mode).

It is served from `HlsServer` (`http://127.0.0.1:<port>/<videoId>.m3u8`) and wins in
`hlsReady` before `fetchMaster`; if there is no AV1 (or another profile), YouTube's
HLS is kept (VP9/h264). Verified in mpv logs: `● Video (av1 …)`,
`Using hardware decoding (vulkan-copy)`, `paused-for-cache=0` and `cache-time`
= complete duration in a few seconds. The offline selftest `--selftest-av1hls`
validates the generated playlist (`EXT-X-MAP`/`BYTERANGE`, suffix `/seg.mp4`, `&nt`
different per segment, height selection and the parsing of the `sidx`) without network nor
player.

## Codec profile by hardware (HwProbe)

The profile that decides which codec is requested from YouTube (`settings.codec_profile`)
is **not** guessed during playback: it is probed **at startup** with
`HwProbe` (`src/hwprobe.*`), which plays in headless mode (`vo=null`,
`hwdec=auto-copy`, without audio or config) three micro-clips embedded in
`resources/clips/` (AV1, VP9, H.264) in that order. Profiles: `av1-hw` >
`vp9-hw` > `h264-hw` > `soft-all`. `MainWindow::applyCodecProfile` translates
the profile to `Yt::CodecProfile` (Modern/Vp9/Legacy), an initial buffer wait and, in
`soft-all`, `m_noHw` + 1080p cap. While probing, nothing is played: the
request is queued (`m_hasPendingWatch`) and launched when it finishes. The
"Detecting hardware" message only appears if the probe takes long (>300 ms, basic PC) or
if the user tries to play while it is probing.

**Verdict per clip (the mpv log rules; `hwdec-current` only corroborates):**
with `msg-level=all=no,vd=v,ffmpeg=v` the decoder line is captured
(`Using hardware/software decoding`), the errors of the HW attempt (`av1_qsv:
unsupported`, `Error initializing…` → `sawHwFailed`) and the software
decoders. **400 ms grace after the first frame** (`video-params/h > 0`):
the decoder verdict is queued 1–10 ms AFTER the first frame and
deciding earlier was the origin of the false positive `av1-hw` on i5/HD4600 (QSV
reports `qsv-copy` but does not decode AV1; the errors arrived 5 ms after
the verdict). `hwprobe::clipVerdict` (a pure function, tested with 9 offline
cases in `--selftest-hwprobe`): `HW` only with a clean HW log and no signals
against; `SW` with an SW log, a failed HW attempt or `hwdec-current == "no"`;
`""` (indeterminate) if there is no signal → it **advances to the next codec**
instead of aborting (thus the i5 reaches H.264, which it does decode in HW). An
`END_FILE` due to error or a timeout without frames also advances as indeterminate.

- Persistence: first codec with a `HW` verdict → that profile (it is persisted);
  if in the end there is no HW but there was ≥1 decisive `SW` → `soft-all` (real
  verdict, it is persisted); if everything was indeterminate → empty profile (transient
  soft-all WITHOUT persisting, retried at startup). Only
  `decoded && verdict` concludes.
- Single re-probe on update: the probe logic is versioned in
  `settings.codec_probe_v` (`kCodecProbeVersion`). If the persisted value is from
  a previous version (e.g. a false positive `av1-hw` recorded by old code)
  or does not exist, it is re-probed ONCE at startup **using the stored value as
  a fallback** and the fresh verdict replaces it; upon completion the
  version is marked and the following startups are fast (it is not re-probed on every boot,
  critical on the test machine). Ajustes → "Volver a detectar hardware" deletes
  `codec_profile` and relaunches `HwProbe` live.
- Old names migrated at startup: `modern→av1-hw`, `vp9→vp9-hw`,
  `legacy→h264-hw`, `nohw→soft-all`.
- Descent ladder (`PlayerWidget::streamDecodeInfoReady` → `MainWindow`,
  `hwprobe::lowerProfile`): the profile is decided at startup (HwProbe) and, if
  playback shows that the profile promises HW for the codec that sounds but
  mpv decodes in **software**, it drops one step
  (`av1-hw → vp9-hw → h264-hw → soft-all`) and is **re-resolved** with the new
  profile (`reloadCurrent`, WITHOUT rotating session: a codec problem is not a network
  one). Hard rules: **only codec/HW evidence, NEVER by network** (a 403, an
  expired visitorData or a cache stall do not change `hwdec-current`; the signal
  only exists with frames decoded in software); `soft-all` is the floor;
  its own budget separate from the session one (`kDescendMaxPerSession=3`) with
  anti-bounce (`kDescendCooldownMs=15 s`) and budget reset with a
  streak decoded in HW. The descent is **EPHEMERAL** (not persisted): an occasional
  failure does not "condemn" the machine; "Volver a detectar hardware" restores it. The
  comparison uses the profile **active in memory** (`m_activeProfile`), not
  `settings`, or after an ephemeral descent it would drop again falsely.
  `--selftest-hwprobe` covers the offline table of `hwprobe::lowerProfile`.
- `Yt::codecRankedScore(altura, mime) = rank*100000 + altura`: the codec
  preference weighs more than the height, so the codec the GPU decodes wins even if its
  variant is lower (e.g. `h264-hw` prefers h264 1080 over VP9 2160,
  which would go to software). It is used in `edgeHeight`, `buildEdgeMaster` and
  `buildFilteredMaster`. The synthetic HLS AV1 (`buildAv1Hls`) is only generated with
  profile `Modern` (`av1-hw`).
- The self-test `--selftest-hwprobe` first verifies the offline decision
  table (9 cases, deterministic regression) and then prints the detected
  profile (GPU-dependent; it is not a fixed assert). It is NOT run in
  CI: the GitHub runner has no GPU, so the HW probe is validated on the
  test machines (x86/x64). For the same reason `--selftest-ffmpeg` (gate of the
  FfmpegBackend) is not in the CI smoke either: the VM cannot exercise AV1/
  DXVA2 (av1 is impossible on x86, see above); it is run on the test machine, where
  it skips the av1 clip if there is no sw decoder.

## ANGLE experiment (zero-copy x64, opt-in)

The 100% CPU of the x64 test machine is the INHERENT cost of
`d3d11va-copy`: with desktop GL as the widget surface, mpv cannot
import the D3D11 textures of the decoder and copies VRAM→RAM→GL texture per
frame. The real zero-copy path on Windows with libmpv+render is **ANGLE +
`d3d11-egl`**: the widget's GL context is ANGLE (GLES over D3D11), the
decoded frame travels through ANGLE's **EGLStream** to the texture without going through
CPU. EXPERIMENTAL and MEASURABLE with `NEVERTUBE_ANGLE=1` (Windows only; it does not exist
in the x86 build, which goes out via `NEVERTUBE_NO_MPV`):

- `main.cpp` forces `QT_OPENGL=angle` before `QApplication` (only if the
  user has not already set `QT_OPENGL`). With that Qt creates the context of the
  `QOpenGLWidget` with ANGLE/D3D11 (everything else in the UI still goes via QPainter,
  which does not use GL).
- There is NO need to pass EGL handles to libmpv: mpv loads `LIBEGL.DLL` with
  the LoadLibrary search (same directory → the SAME ANGLE that Qt already loaded)
  and takes the display in `eglGetCurrentDisplay()` of the render thread
  (`paintGL`). `hwdec=auto` stays (protected invariant): in
  `video/out/gpu/hwdec.c` the driver `d3d11-egl` comes BEFORE `d3d11va`, so
  with an ANGLE context mpv chooses only the zero-copy; with desktop GL
  `eglGetCurrentDisplay()` is NULL and it falls back to `d3d11va-copy` (current
  behavior, no harm).
- `MpvBackend::initGL` logs `E2E GL ctx es=… vendor=… renderer=…` to
  empirically confirm which context there is (ANGLE → vendor "Google … (ANGLE)";
  desktop → driver vendor).
- **How to measure on the x64 test machine** (before/after with the SAME video and quality):
  1. `NEVERTUBE_ANGLE=1 ./nevertube_qt2` (needs `libEGL.dll`/`libGLESv2.dll`
     next to the exe; `windeployqt` deploys them and the x64 CI verifies them in the
     `$need` list of the setup gate).
  2. With `NEVERTUBE_DEBUG=1`, the verdict is the mpv log + `hwdec-current`
     in the E2E: `Using hardware decoding (d3d11-egl)` = zero-copy active;
     `(d3d11va-copy)` = nothing changed. Compare CPU in the Administrador de
     tareas.
- **Expected (harmless) failures**: if the ANGLE of Qt 5.15.2 does not expose the
  extensions that `hwdec_d3d11egl.c` requires (`EGL_ANGLE_stream_producer_d3d_texture`,
  `EGL_ANGLE_d3d_share_handle_client_buffer`, `EGL_EXT_device_query`, GLES
  `GL_OES_EGL_image_external_essl3`/`GL_NV_EGL_stream_consumer_external`), the
  `init` of the driver fails and mpv continues with `d3d11va-copy`: neither improves nor breaks.
  ANGLE uses WARP only if the GPU does not reach D3D11 FL9_3 (the Apollo Lake of the
  x64 test machine is FL11_1: hardware). If the ANGLE mode gave flicker/black in the
  widget, the event-driven redraw (update callback) forces the repaint when
  each frame arrives; even so, the mode
  is reversible by removing the variable.
- On Linux the `Q_OS_WIN` guard leaves everything as it was (desktop GL;
  there zero-copy is already possible via VAAPI/EGL without ANGLE, see `hwdec-current`).

## Linux: the render node + EGL (the 35 % that the render API was hiding)

With `vo=libmpv` (render API) mpv has **no native display of its own**: it gets the GL
context of the `QOpenGLWidget`. Without a display it cannot create its `VADisplay`, so
`hwdec=auto` fell back to a **copy** path (`vulkan-copy` here): every frame went
VRAM → RAM → GL texture. It was invisible in the logs unless `hwdec-current` was read,
and it cost about **1 pp of the UI thread and 0.85 pp of the mpv core**.

Two independent pieces, both guarded (`render_fd = -1` / no `Display*` = exactly the
previous behaviour):

1. **`MPV_RENDER_PARAM_DRM_DISPLAY_V2`** with a render node the app opens itself
   (`/dev/dri/renderD128`, 129, 130: `MpvBackend::openDrmRenderNode`). This is what
   makes `hwdec=auto` reach `vaapi` (dmabuf imported into the GL texture) on Wayland.
2. **EGL on X11**, because the DRM node alone is NOT enough there:
   `hwdec_vaapi.c` `init()` first looks for a **dmabuf→texture interop** and only
   warns `"VAAPI hwdec only works with OpenGL or Vulkan backends."` when there is
   none — the message is about the interop mapper, not about the backend. With GLX
   there is none (EGL only), so the fix is to make Qt use EGL on X11 too:
   - `src/main.cpp` sets `QT_XCB_GL_INTEGRATION=xcb_egl` **before** `QApplication`
     when Qt really ends up on xcb (explicit `QT_QPA_PLATFORM` wins; a Wayland
     session is left alone) and only if the user has not set it. `xcb_glx` restores
     the old path.
   - `MpvBackend::openX11Display` passes `MPV_RENDER_PARAM_X11_DISPLAY` with a
     `Display*` opened with `dlopen("libX11.so.6")`: no X11 headers, no new link
     dependency, and only when `QGuiApplication::platformName()` starts with `xcb`.

Measured with the same build, the same local clip (1080p30), the player page and no
debug logging, three 30 s samples per configuration (Ryzen 7 7840U / Radeon 760M):

| configuration | total | UI thread | mpv core |
|---|---|---|---|
| base, Wayland, `vulkan-copy` | 6.32 % | 2.88 % | 1.56 % |
| now, Wayland, `vaapi` | **4.08 %** | 1.84 % | 0.72 % |
| base, X11/GLX, `vulkan-copy` | 6.08 % | 2.96 % | 1.68 % |
| now, X11/EGL, `vaapi` | **4.08 %** | 1.84 % | 0.76 % |

(`%` of one core.) The same result on both sessions is the proof that it is the
interop and not the compositor. Verification without trusting the number:
`NEVERTUBE_DEBUG=1` must show `E2E render node … fd=`, `E2E render ctx x11 display`
(only on X11) and mpv's `Using hardware decoding (vaapi)` **plus**
`Using EGL dmabuf interop via GL_EXT_EGL_image_storage`. If the interop is missing mpv
says `VAAPI hwdec only works with OpenGL or Vulkan backends` and it is the copy path.

- **Do not** "solve" this by switching the widget to a Vulkan surface: `vo=libmpv` +
  the GL context is the protected piece, and both fixes are inside it.
- **Windows does not need any of this**: there `hwdec=auto` reaches `d3d11-egl`
  with the ANGLE experiment above, or `d3d11va-copy` with desktop GL.
- The x86/FfmpegBackend build is untouched: it has no libmpv (`NEVERTUBE_NO_MPV`),
  so neither the node nor the EGL integration is compiled or applied.
- `MpvBackend` also pins `clipboard-backends=vo`: the app never pastes anything into
  mpv and its wayland backend reads the data offer on every clipboard change
  (measured: 66 reads/s for a whole minute while the owner kept re-offering).

**If the video goes black on an X11 machine** the suspect is the EGL integration:
launch with `QT_XCB_GL_INTEGRATION=xcb_glx` and the app is back to the previous path
(with the copy hwdec). It has only been measured on Mesa/AMD; NVIDIA's EGL is the
other one QA has to confirm.

## CLI entry points

- `./build/nevertube_qt2` → normal app
- `./build/nevertube_qt2 --search <consulta>` → search at startup
- `./build/nevertube_qt2 --channel <channelId>` → open channel at startup
- `./build/nevertube_qt2 http://...` → play a URL at startup
- `./build/nevertube_qt2 --selftest-db` / `--selftest-player` /
  `--selftest-innertube` / `--selftest-discover` / `--selftest-home` /
  `--selftest-feeds` / `--selftest-av1hls` / `--selftest-hwprobe` / `--selftest-search` /
  `--selftest-grid` / `--selftest-theme` / `--selftest-glyphs` /
  `--selftest-dates` / `--selftest-i18n` → tests

## Configurable accent color

The accent (`settings.ui_accent`, by default the icon red `#FA011B` — the
play of the app icon, aligned with the brand) is changed
live from Ajustes (Interfaz → Color de acento) and is persisted.

- Single source of the palette: `nt::accentOptions()` in `src/ui/theme.*`
  (visible label + canonical hex + file suffix). `accentHex()`,
  `accentHoverHex()` (mixes ~10% white; from the icon red comes `#FF1B35`) and
  `accentPressedHex()` (`darker(115)`; from the icon red comes `#D90117`) are
  read on EVERY call: nothing `static`
  that freezes the accent on first use.
- The global QSS is generated with placeholders `@ACCENT@`/`@ACCENT_PRESSED@`
  substituted per call (`nt::styleSheet()`), so the live re-polish
  repaints slider/progress/primary/selection/scrollbar with the new color.
- The bottom navigation uses **PNGs pre-painted per accent**
  (`nav/<key>_<state>_<suffix>.png`), NOT retint at runtime: the retint was the
  origin of two bugs (black square due to `QColor(QRgb)` that forced alpha=255,
  and accent frozen by caching the color in a `static`). NEW ACCENT COLOR:
  add it to `nt::accentOptions()` and regenerate the PNGs with
  `tools/gen_nav_accent.cpp` (all suffixes are re-tinted with an EXACT RECOLOR
  to the value of the variant — active=accent, hover=accentHover,
  pressed=accentPressed —, pixel by pixel and keeping only the alpha; it is NOT
  passed through HSL because Qt's RGB→HSL→RGB roundtrip is lossy and the nav must
  show the SAME value as the accent buttons; the NATIVE color of the
  sprites is the red `#FF0033` — `nativeRed` in the generator, which never
  changes — and since the `rojo` suffix of the palette points to the icon red
  `#FA011B`, that one is also re-tinted). Before, the tinting kept
  the luminosity of the sprite and the nav came out in tones that were NOT the new red
  (active `#FE021F` instead of `#FA011B`, salmon hover `#FF697B` instead of the
  `#FF1B35` of the CTAs, pressed `#CB0118` instead of `#D90117`); the exact
  recolor is part of the migration of the bottom bar to the icon red.
- `MainWindow::accentChanged` applies live: global stylesheet, re-selection
  of the nav icons (only re-evaluates the suffix), repaint of `VideoListView`
  and `applyAccent()` in watch/channel/home/settings (subscribe, active
  actions, description links, empty state, reset button).

## SELECTION color: text and suggestion (from the user)

Two details that went unnoticed and that the user pointed out:

- **Selecting text goes in the app gray** (`kSelection` #2D2D2D, the same
  token as the menus), not in the accent. Before, the `QLineEdit` painted
  `selection-background-color: @ACCENT@` (red) and it looked like an error. The
  Player description (a `QLabel` with `Qt::TextSelectableByMouse`) is not
  covered by the QSS: it paints it with `QPalette::Highlight`, which without more
  came out with the desktop theme blue. That is why **the palette is applied in `main.cpp`**
  (`nt::applyTextSelectionPalette`, right after creating the `QApplication`, on
  `app.palette()`): it is the only thing that reaches the selectable widgets that the
  QSS does not reach. Only `Highlight` and `HighlightedText` are touched (the latter to `kText`,
  11:1 on `kSelection`) so as not to change anything else of the desktop theme.
  Measured on a real render: selection band `#2D2D2D` on field `#1F1F1F`,
  text inside in `#ECECEC`.
- **The highlighted suggestion row carries the accent.** It was lost because
  `QWidget#suggestRow[highlight="true"]` was `#1F1F1F`, DARKER than the popover
  `#202020`: it was a darkening, not a highlight. Now it is a **3 px accent bar
  on the left** + an `@ACCENT_A16@` veil, on hover in
  `kSelection`. The text stays in `kTextSoft`/`kMuted` (they still give 8:1 and 4:1
  on the veil), so the child labels do not need to be touched. The left border
  is always reserved (3 px transparent in the normal row) so that the
  bar does not push the text: measured, the text starts at `x=13` with the row
  highlighted and without it. The dynamic property is set by `SuggestRow::setHighlighted`
  (with re-polish, which is mandatory: changing the property does not repaint by itself).

`--selftest-theme` covers both: palette in `kSelection`/`kText`, that the QSS does not
select in the accent, and that the `[highlight="true"]` block carries the accent.

## QSS selectors without a widget behind them (`--selftest-theme`)

A rule whose `#nombre` is set by **nobody** on any widget paints nothing:
it applied to nothing and went unnoticed. That is why `QPushButton#danger` can be
in the sheet without it being noticed (it was deleted; also it was bad for using the accent
to say "peligroso", which only works if the accent is red).
`--selftest-theme` catches it: it extracts the ids from the QSS and requires each one to appear
as a **literal** somewhere in `src/`.

Two decisions that make the gate useful and not a hindrance:

- **The literals of ALL of `src/` are looked at, not just the arguments of
  `setObjectName`.** There are helpers that receive the name as a parameter and
  apply it inside (`makeSeg` in `watch_page.cpp` sets `#actionSegL` and
  `#actionSegR`): with the strict criterion they came out as dead without being so, and a
  false positive teaches you to ignore the test. The trade-off is the opposite — that the
  same name is genuinely cited somewhere else and covers a dead selector —, but
  that is a **false negative**: the CI warns and it is looked at, whereas a false
  positive kills the gate.
- **Comments are discarded before looking at the literals**, with the same
  `sinComentarios`/`literalesDe` that `glyphSelfTest` uses. Otherwise it would be
  enough to cite the name in a `// este botón se llama X` for a dead selector to
  pass as live, and the gate's own comment would self-block.

The block state **does not cross from one file to another**: an unclosed `/*` would leave
the following file entirely marked as a comment and its literals would not be seen
(a silent hole). If the source tree is not next to the binary it is skipped
with an `[info]` and does not fail, just like `glyphSelfTest` does.

It has really found it: `QLabel#muted` was in the sheet and the literal
`"muted"` did not appear anywhere (the color is still available to whoever
needs it as `@MUTED@`/`kMuted`).

## Glyphs: the default font does NOT have all the characters (measured)

The UI paints its text with the system default font, and that font
(Liberation Sans on Linux, the one fontconfig resolves) **does not cover all
non-ASCII characters**. When the character is missing, Qt paints the *missing
glyph marker*: measured on screen, it is **2x5 px**, i.e. an almost
invisible button. Four glyphs of the UI were in that situation:

| glyph | U+ | place | looks like |
|---|---|---|---|
| `✕` | 2715 | search clear X button | empty box with a dot |
| `◀` | 25C0 | "◀ Volver" button of the Player (it also duplicated the `Back` icon it already had) | a dot before "Volver" |
| `▾` | 25BE | suffix of the 3 value buttons of Ajustes (`makeValueButton`) | a dot |
| `✔` | 2714 | "ya está" label of the playlists menu | a dot |

How it is fixed, in order of preference:

1. **Icon from `nt::icon()`** (`src/ui/icons.*`): draws with `QPainter`, it
   does not depend on any font. It is what is used for the clear X and for
   "Volver" (which already had the `Back` icon: the `◀` in the text was redundant).
2. **A glyph the font does have** (check it first, see the self-test
   below).
3. **Remove it**: if the text already says the same thing ("✔ ... (ya está)"), the glyph is
   decorative and superfluous. That is what ended up happening with the `▾` of the Ajustes
   value buttons: the `▼` (U+25BC) put in its place IS in the font,
   but the user asked to remove it entirely — the button is already a control with its
   box and the menu marks the option with a check, so the triangle was
   redundant decoration. `makeValueButton` is today a wrapper of
   `makeActionButton` with a comment that forbids reintroducing it.
4. What **not** to do is trust that the glyph is visible: `✕`, `▾`, `◀` and
   `✔` are not in Liberation Sans and on screen they came out as a dot.

`--selftest-glyphs` (`nt::glyphSelfTest` in `src/ui/theme.cpp`, in the smoke of
both CIs) walks the **literals of the source tree itself** —comments are
discarded first— and checks each non-ASCII character against the app's
real font: a new glyph that does not exist comes out in the CI instead of on
the user's screen. 66 files, ~3360 literals, 17 glyphs.

- **WATCH OUT: `QFontMetrics::inFont()` does NOT work for this.** It queries the system
  *fallback* and returns true if **any** installed font has the
  character (here there are 25 that cover U+2715), while the app kept painting the
  missing glyph marker. What decides is whether the **file** of the
  app's font has it, and that is answered by `QRawFont::fromFont(appFont)` +
  `supportsCharacter()`. If the font cannot be opened (a system composite
  font), it falls back to `inFont` and warns on screen.
- The `✕` that remains in the code comments is on purpose: the test
  discards them, and citing them explains why they are forbidden.

## Thumbnail and subscriptions cache

- `ThumbnailCache::pixmap()` **falls back to disk** if the LRU evicted the image from
  memory. Do NOT remove that fallback: the `VideoListView` delegate paints reading
  from `pixmap()` on every paint and nobody requests an item already shown again, so
  without it the thumbnails "disappear" (search/channel/home) when the budget is
  shared between pages.
- On subscribing, the avatar URL is persisted (`SubscriptionsRepo::add(..., avatar)`),
  and `SubscriptionsRepo::setAvatar` updates it when `ChannelInfo` arrives.
  `LibraryPage` also requests `channelInfo` when `s.avatar` is empty (even if it
  already has the count), to fill in avatars of old subscriptions.
- The "Nuevos de tus suscripciones" feed is invalidated when entering Home by comparing
  the signature of subscription ids stored in `settings.feed_subs_sig`. Without that
  check the cached feed (TTL 30 min) would keep showing only the previous
  subscriptions after subscribing.

## Home sections: "Tendencias" does NOT exist (removed 2026-10)

Home is "Continuar viendo" + "Nuevos de tus suscripciones" + "Recomendado" +
"Descubre". The "Tendencias" section (`Yt::trending` →
`Innertube::fetchTrending`, browseId `FEtrending`, section `trending` of
`home_cache`, i18n key `home.trending`) was **removed**, and it was not
cosmetic: YouTube answers **`400 "Request contains an invalid argument"`** for
`FEtrending` in both VISIONOS and WEB (with and without `params`), while the
control `FEwhat_to_watch` answers 200. It was dead, and it was costing time:
`Ipv4Https` is ONE connection with a FIFO queue, so the ~12 s of that request
blocked everything behind it and Home's dynamic block was ready at **16,3–17,6 s**
instead of **0,15 s** (measured). Do not bring it back without a working
trending source.

## "Continuar viendo" (duration + position in the history)

**The "playback state" rule lives in ONE place**:
`HistoryRepo::progressFraction(v, positionMs)` (src/db/repositories.cpp) returns
the fraction 0..1 of the bar, or 0 if it cannot be shown (no position, no known
duration, already completed, or a fraction outside (0,1)). It is called by the THREE
places where a video with progress is shown, and there is no fourth copy:

| place | who paints the bar |
|---|---|
| Home, "Continuar viendo" | `VideoListView` delegate (`drawProgressBar`) |
| Historial page ("Ver todo") | the same delegate, in `ListMode` |
| Historial section of **Biblioteca** | `AsyncThumb::paintEvent` |

The Biblioteca section painted it by hand because its rows are `RowItem`
(`QLabel` thumbnail + text), not the delegate; that is why it uses the SAME measurements
as `drawProgressBar` (2 px margin, 3 px high, 1 px above the border,
radius 1.5) clipped to the rounded rectangle of the thumbnail. Since it is painted by
hand with `nt::kAccent` (the QSS does not reach a `paintEvent`), there is
`LibraryPage::applyAccent()` to repaint the thumbnails when the accent changes hot,
and `MainWindow` calls it along with the other pages. `buildRows` receives a
`QHash<videoId, fraction>`; empty = no bar, and only the Historial section passes
it.

`HomePage::loadContinueWatching` requires, for each recent id: `pos > 0`, a row
in `videos` (the cache), **`durationSeconds > 0`**, not completed and 0 < frac < 1
(the last three things are what `progressFraction` returns).
The last two data items are written by the player, not the network:

- **Duration**: `PlayerWidget::durationUpdated` (emitted by BOTH engines:
  `mpv_backend.cpp` observing the `duration` property and `ffmpeg_backend.cpp`
  with the audio clock) → `PlayerController::onPlayerDuration` →
  `durationChanged(double)` → `WatchPage::onDurationKnown`, which stores it in the
  cache. It is the AUTHORITATIVE datum: `parseVideoDetail` (`/next`) does not fill in
  duration and the origin renderers (search/home/channel) sometimes do not either, so
  without this a video watched several minutes stayed with `duration=0` and
  NEVER appeared in "Continuar viendo" (the filter discarded it silently).
  `PlayerController` compares against `m_reportedDuration` (reset on every
  `playIndex`), not against `m_duration`: position ticks also update
  `m_duration` and could "consume" the notice before the signal arrives.
  `onDurationKnown` fills in ONLY the duration and respects what is already cached — the
  `upsert` is `INSERT OR REPLACE`, so putting the queue item in as-is
  would erase title/channel/thumbnail. For the same reason `onDetailReady` keeps the good
  duration when the `/next` brings it as 0.
- **Position**: `kPositionFlushMs = 20000` (20 s, previously 60 s: it was the
  visible delay of the progress bar; YouTube saves every ~15-20 s). `savePosition()`
  discards the write if it is the same video and the position has not advanced 2 s,
  so with the video paused nothing is written. The cost per write is an
  `INSERT OR REPLACE` of one row (local SQLite) plus the `changed()`.
  Places where it is saved, all already in the code:
  `playPause` (pause), `playIndex` (video change: explicit click on
  any page, "A continuación" or auto-advance on finishing), `stop()`
  (the "Cerrar y detener" button of the mini), `aboutToQuit`, and
  `MainWindow::savePositionNow()` when **entering Home** with something playing
  in the mini player (`onNavButton` case 0 and `showPreviousTab` with destination 0).
  That last one is what makes the row paint with the REAL progress, and it goes
  **before** `refreshDynamic()` on purpose: in `onNavButton` the
  `setCurrentIndex` goes afterwards, so the page is still hidden and the `changed()`
  of the save is skipped by the visibility guard of `HomePage`.
  `HomePage::refreshContinueWatching` (connected to `HistoryRepo::changed`) does an
  early-return if the page is hidden: every flush with the app in the mini player
  must not rebuild a grid nobody sees.
  `MainWindow::onRadioReadyDiscover` (a player radio that was just saved for
  Descubre) calls `m_home->refreshDynamic()` ONLY with Home VISIBLE, with the
  same guard: with the page hidden (playback on the Player), that refresh
  re-read the history while the just-opened video still had `pos=0` (`start=0`
  until the first 20 s flush) and CLEARED "Continuar viendo" for nothing — the
  counter then stayed frozen at the empty result because the next `changed()`
  skips the recalculation while hidden. Entering Home always re-runs
  `refreshDynamic()` (`onNavButton`), so nothing is lost.
- **When verifying in the DB, also copy `mini_tube.db-wal`**: the app uses WAL and
  copying only the `.db` gives an outdated snapshot (it looks like nothing is saved).

## "En directo" badge on thumbnails (badge + refresh on re-entry)

A video that is a **live stream** no longer shows its duration on the thumbnail: it shows
the **"En directo"** badge (accent pill, like the "EN VIVO" mark of the
player and YouTube's red pill). The datum travels per row in
`VideoInfo.isLive` and is persisted in `videos.is_live` (`ALTER` migration for old
DBs, `app_database.cpp`).

- **Who paints it**: the `VideoListView` delegate (grid and list:
  `drawLiveBadge` replaces `drawBadge`, and `drawProgressBar` returns early if
  `v.isLive` because a live stream has no fraction to show) and `AsyncThumb`
  (`setBadgeInfo(durationSeconds, isLive)`) for the **Biblioteca** rows, which
  do not use the delegate. All video thumbnails of Biblioteca carry a
  badge (duration or "En directo"), uniform like YouTube.
- **Where `isLive` comes from in the network** (anonymous helpers of `yt_innertube.cpp`,
  each client declares it its own way):
  - VISIONOS `compactVideoRenderer` (search/home/feed): `thumbnailOverlays[].
    thumbnailOverlayTimeStatusRenderer.style` contains `LIVE` (`overlayIsLive`).
  - classic WEB `videoRenderer`: `badges[].metadataBadgeRenderer.style` ==
    `BADGE_STYLE_TYPE_LIVE_NOW` (`badgesIsLive`). `rendererIsLive` combines the two
    and is applied in `videoFromHomeRenderer`, `parseRelated`, `parseWatchQueue`
    (`playlistPanelVideoRenderer`) and `parsePlaylistBrowse`
    (`playlistVideoRenderer`).
  - modern WEB `lockupViewModel` (channel tabs): `contentImage.
    thumbnailViewModel.overlays[].thumbnailBottomOverlayViewModel.badges[].
    thumbnailBadgeViewModel.badgeStyle` == `THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE`
    (`webLockupMeta`).
  - `/next` (`parseVideoDetail`): the `videoMetadataRenderer` does NOT bring duration or
    live flag, but the `viewCountText` DOES distinguish "sigue en emisión"
    (`"10.554 usuarios viéndolo ahora"`) from "terminó"
    (`"40.071 visualizaciones"`) → `viewCountShowsLive`.
- **AUTHORITATIVE source at playback time**: the player response
  (`videoDetails.isLive` → `StreamInfo.isLive` → `PlayerController::
  currentIsLive()`) is written by `WatchPage::onDurationKnown` into the cache alongside
  the duration. A live stream in progress reports a duration that **grows** with
  time (the "hasta ahora"), so that guard does not cut it off.
- **Refresh on re-entry**: a cached live stream may have ended between
  visits. When rebuilding Home ("Continuar viendo") or the Historial section of
  Biblioteca, each `isLive` row triggers `maybeRefreshLiveStatus` →
  `Yt::getDetail` (the `/next`): if the viewCountText goes back to "visualizaciones",
  `onLiveDetailReady` calls `VideoCacheRepo::updateLiveStatus(id, false)` and the
  row is re-painted as a normal video (duration + bar). **The player
  response / `fetchHls` is NOT used from the pages**: `m_videoId`/`m_flightVideoId` are unique
  state of the player phase and a concurrent resolution would corrupt a
  playback in progress. The `/next` is serialized and safe.
  Dedup: in-memory TTL `kLiveRecheckMs` (10 min) per id + in-flight guard
  (`m_liveRefreshPending`) with cleanup in `detailFailed`.
- **Known limit**: the `/next` does not give the total duration, so a live stream
  that ended keeps the last cached duration (the "hasta ahora" that
  `onDurationKnown` wrote during the broadcast) until it is played, at which point
  the player response sets the exact one.
- **"Continuar viendo"**: `loadContinueWatching` no longer discards a live stream without a
  fraction (`if (frac <= 0.0 && !v.isLive) continue;`): a live stream with duration
  0 or watched to the edge (`progressFraction` returns 0) still enters and only
  paints its badge; the bar is added only if `frac > 0`.

## Conventions

- UI, logic and playback separated. `PlayerController` (state/queue) vs
  `PlayerWidget` (render) vs pages (views).
- SQLite repos in `src/db/`; reusable infrastructure in `src/ui/`.
- **The bottom bar: SETTINGS is ALWAYS the last one.** The order is Inicio,
  Buscar, Biblioteca, Descargas, Ajustes. The button index and the page
  index are NOT the same: the nav index of the pages is `m_currentTab == 4
  ? 2 : m_currentTab == 8 ? 3 : m_currentTab == 5 ? 4 : m_currentTab`
  (library 2, downloads 3, settings 4). When a button is added or moved,
  those THREE places move with it: the `addBtn` calls, the `switch` of
  `MainWindow::onNavButton` (which assigns `m_currentTab`) and that mapping.
  `openDownloads()`/`goLibrary()` are shortcuts to that same switch; there is
  no `goSettings()` (the settings button is the last one and goes through
  `onNavButton`).
- **ONE single dialog to confirm and ONE only to ask for a text**, and they are
  `nt::confirm` / `nt::inputText` (`src/ui/confirm_dialog.*`). Both mount
  an overlay **child of the WINDOW** (`page->window()`), with a veil, modal, without
  `QDialog` and without a floating window: a window of its own grabs the pointer.
  Hard rule: if a destructive action or a text field appears, one of the two is called;
  a `QFrame#popupDialog` is not mounted per page. There are seven
  confirmation places (the three in Ajustes, deleting the search history,
  deleting the video history, and deleting a playlist in Biblioteca and on its
  page) and five text ones (creating a playlist in Biblioteca, Historial and Player,
  and renaming in Biblioteca and on its page). The popovers that remain are
  deliberate and are **not** dialogs: they are scrollable lists (managing
  subscriptions, adding to playlist, quality). The box measures 400 px in
  `nt::confirm` (the body is a wrapping paragraph) and 340 px in
  `nt::inputText` (a single field).
- Stream resolution: Innertube (VISIONOS client, `src/yt_innertube.*`) as the only path; if it fails, `Yt::resolveError`.
- Channel loading: Innertube `fetchChannelVideos` (browse "Vídeos" tab), with no race or external fallback. A result with 0 videos of non-empty id counts as empty.
- Search, channel info, tabs and detail: EVERYTHING via Innertube (`src/yt_innertube.*`):
  - `Yt::search` → POST `/search` (pagination by `continuation`; parser `compactVideoRenderer`).
  - `Yt::channelInfo` → browse "Información" tab: `c4TabbedHeaderRenderer` (name, avatar, subscribers) + `channelAboutMetadataRenderer` (description).
  - `Yt::channelTabVideos` → channel browse with the **WEB** client for the three tabs (videos/shorts/live): VISIONOS only exposes "Vídeos" and its continuation returns empty pages, while WEB serves the three with the modern layout (`lockupViewModel`/`shortsLockupViewModel`) and a working continuation. The WEB client does not need an API key. `fetchChannelVideos` (subscriptions feed, without pagination) continues via VISIONOS with `compactVideoRenderer`.
  - `Yt::getDetail` → `/next`: `videoMetadataRenderer` (title/description translated, views, date, channel) + `likeButtonRenderer` (likes). The **channel id** comes from `owner.videoOwnerRenderer.navigationEndpoint.browseEndpoint.browseId` and, if missing (COLLABORATION videos: the owner only brings `avatarStack` + `subscribeButton`), from `owner.subscribeButton.subscribeButtonRenderer.channelId`: without that path the `channelId` stayed empty and the "Canal" and "Suscribirse" buttons did nothing (both return early if it is empty). The queue renderers (`compactVideoRenderer`/`playlistPanelVideoRenderer`) also take the id from `longBylineText`/`shortBylineText` → `runs[0].navigationEndpoint.browseEndpoint.browseId` (`channelIdFromByline`), where before only the name was read.
  - `Innertube::fetchPlaylist` → browse `VL<id>`: `playlistHeaderRenderer` +
    `playlistVideoRenderer`. There is NO `Yt::playlistDetail`: browsing a
    YouTube playlist is not wired to any page (the user playlists are local,
    in the DB), so the forwarding through `Yt` was dead code and was removed.
    The parser stays because `--selftest-browse` covers it.
- **Searching with Enter really closes the suggestions dropdown**: `doSearch()`
  stops the 300 ms debounce and marks the query as already searched
  (`SearchPage::m_suggestClosedFor`), and `onSuggestReady` ignores it. Without that,
  when typing fast and pressing Enter the suggestions request came out later and the
  panel reappeared over the results (also with a request in flight).
  Typing again clears the mark.
- Windows packaging (Win7 x86): the UCRT is NOT copied app-local from the Windows SDK — that version imports `GetSystemTimePreciseAsFileTime` (W8+) and ucrtbase does not load on Win7. Instead the setup bundles `VC_redist.x86.exe` (downloaded in CI) and installs it with `Exec` (`/install /passive /norestart`, codes 0/1638/3010 accepted) with `PrivilegesRequired=admin`. The CRT (msvcp140/vcruntime140) does go next to the exe (`windeployqt --compiler-runtime`). ORDER of the setup: 1) full app → 2) icons (menu + desktop shortcut, `[Tasks] desktopicon` checkbox checked by default) → 3) `InstallVcRedist` from `CurStepChanged` → `ssPostInstall` (not in `AfterInstall` of a `[Files]`, which would run in the middle of the copy). The `[InstallDelete]` keeps purging the app-local UCRT of old installations. The x86 setup **cancels on 64-bit Windows 10/11** (`InitializeSetup` + `IsWin64` + `GetWindowsVersion >= $0A000000`) and directs to the x64 build (the `MsgBox` names `nevertube-setup-x64.exe`: cancelling without saying where the installer that does work is leaves the user with no way out): the x86 there would be capped to h264 HW + VP9 software and without AV1. It is allowed on 32-bit and on 64-bit < 10 (where the x64/modern mpv does not run). **TOOLSET and REDIST (Win7)**: the x86 compiles with `-T v142` (VS2019/14.29) on `runs-on: windows-2022` and the setup installs `vc_redist.x86` **2015-2019** (`aka.ms/vs/16`): the target (Win7) needs that CRT. `windows-latest` is already VS2026 (MSVC 14.5x) and its runtime is NOT Win7-compatible (imports W8+); a 14.5x binary also does not link against vs/16. Do not remove the toolset/image pin nor change the redist to `vs/17`+ without testing on the target. Also, the CI purges `qtaudio_wasapi.dll` from the bundle before the gate: Qt5 Multimedia deploys it but it imports `RoGetActivationFactory` (W8+) and the imports gate aborts it; on Win7 audio goes through `qtaudio_windows.dll` (WaveOut), not WASAPI.
- Windows x64 packaging (Win10+): the deliverable is an **installable setup.exe**
  (`dist64.iss`, compiled with `iscc` in `windows_x64.yml`), NOT a portable zip.
  It is the twin of `dist.iss` (x86/Win7) with 5 differences, all deliberate:
  (1) `ArchitecturesInstallIn64BitMode=x64compatible` so that `{autopf}` is
  64-bit `C:\Program Files` and not `Program Files (x86)`, and
  `ArchitecturesAllowed=x64compatible` (real 64-bit and emulated Win11 ARM);
  (2) `MinVersion=10.0` (modern mpv + Vulkan do not exist before Win10). CAREFUL:
  Win10 reports as 6.3 unless the compatibility manifest that the Inno 6 setup does
  carry; with Inno 5.5.5 that `MinVersion` would reject Win10 itself;
  (3) there is NO UCRT problem (Win10+ brings it in the system) → without
  `[InstallDelete]` of the app-local UCRT, and the redist is `VC_redist.x64.exe` from
  `aka.ms/vs/17` (the 2022+, without the W8+ imports restriction that forces the x86
  to use vs/16); (4) the files come from `dist\` with `VC_redist.x64.exe`
  excluded and copied to `{tmp}` so that `InstallVcRedist` launches it from
  `CurStepChanged`; (5) there is **NO `InitializeSetup`**: the x86 architecture
  guard cancelled on 64-bit 10/11 and here 64-bit IS the target (a 32-bit
  one is rejected by `ArchitecturesAllowed` before writing to disk, with its own
  message). What it DOES share with the x86: `[Tasks] desktopicon` checked
  by default, `[UninstallDelete]` of the DB (`{userdocs}\mini_tube`) and of the
  caches (`{localappdata}\nevertube\nevertube`), `PrivilegesRequired=admin` and the
  same order app → icons → redist. In `[Code]` comments go with `//`:
  the `;` is only valid in the rest of the sections (inside `[Code]` it is a
  statement separator and `iscc` rejects it). The workflow gate requires that
  the setup weigh >5 MB and checks the 9 key files against the manifest that
  `iscc` prints (`Compressing: <ruta>` per file, saved in `iscc.log`), not
  against `dist\`: a badly written `[Files]` pattern compiles the same. `innoextract`
  is NOT used for that: its last version is 1.9 (2020) and it cannot read the Inno
  6.7 that choco installs (loader revision 2), so it would only give a warning; the
  compiler log is the authoritative source (and seeing `VC_redist.x64.exe` in the list
  is the proof that `dist64.iss` was compiled and not the x86 one).
- The controls overlay is NOT a separate widget: it lives inside
  `WatchPage::m_overlay`/`PlayerContainer` (src/ui/pages/watch_page.cpp).
  Any change there that affects the video area requires the selftest.
- **The media buttons have NO frame** (user rule). Neither at rest nor
  **when pressed**. All are `#iconBtn` (transparent, no border in any
  state). The quality button USED to be the text chip `QPushButton#chip`
  ("720p"), which carried `1px solid transparent` (it keeps the 2 px so the
  focus does not shift the content, but the frame is not visible); today it is the
  `PlayerIcon::Settings` gear, one more `#iconBtn` (no text and no border of its own),
  and the `#chip` rule was deleted from the QSS. **The `:focus` is nulled by hand**
  (`border-color: transparent`): the global rule `QPushButton:focus { border-color:
  @ACCENT@; }` applied anyway and lit a frame in the accent as soon as it was
  pressed. Focus is marked ONLY with the fill (`@HANDLE_HOVER@`).
- **The action buttons of the Player page** (`#actionChip`: Compartir, Me
  gusta (favourites/star), Ver más tarde and Añadir a playlist) follow the same
  rule as **Me gusta / No me gusta** (`#actionSegL`/`#actionSegR`, which already
  carried `border: none`): **no frame in any state**. `#actionChip` did not
  declare a border, so it inherited the global `1px solid @BORDER@` and the
  `QPushButton:focus` lit it in the accent when pressed (a red ring). Now it
  declares `1px solid transparent` and its `:focus` stays transparent; the state
  is still marked by the fill (`active` and `:hover`) and by the icon colour.
  `--selftest-theme` watches it: `#iconBtn`, `#actionChip` and the items of
  `#qualityMenu` cannot declare a visible border **in any state**
  (it recurses through `:hover`/`:pressed`/`:focus`; verified that it FAILS both if the frame
  is brought back in the base and if it is brought back only in `:focus`).

## Window coverage and scaling on x86 (FfmpegBackend)

Possible symptom (Win7 x86): **FPS drops when opening a floating window
over the video, with the app's CPU/RAM low**. Two mechanisms:

1. With DWM (Aero *or* Basic theme, which also composites) Windows **does not warn
   the app that it is covered**: the `QOpenGLWidget` keeps receiving paint events and
   the FfmpegBackend kept decoding+converting+painting frames nobody
   sees. The bottleneck is the compositor's **presentation** on the GMA 950
   (blend of the overlapping region every frame), not the app's computation — that is
   why the CPU/RAM stay low. The player's redraw is not to
   blame.
2. The fix is surgical and **only applies to FfmpegBackend**:
   - `PlayerWidget` starts a **250 ms** timer (only if
     `occlusionSkipSupported()`) that walks the **Windows z-order**
     (`GetWindow` from the top down to our top-level; visible windows, not
     `WS_EX_LAYERED`, that fully cover the widget's rect). Covered →
     `setVideoOccluded(true)` and: the decode thread stops doing copy-back and
     `swscale` (the frame is discarded without touching CPU/RAM, keeping the decode
     hot and the **position up to date with the audio clock** — critical: without that
     the stall watchdog would see the position frozen > 5 s with the video
     covered and would fire a false `streamStalled`) and `paintGL` returns early
     (it does not upload textures nor paint). It is frame-driven: on uncovering the most
     recent frame is shown, without a burst. On **mpv/x64 it does NOT apply**: stopping the render
     there accumulates frames and produces the "jump on return" (see mini player);
     `MpvBackend` does not override the virtuals.
   - `frameToImage` scales **only downwards** to the size of the render
     surface (physical px, given by `renderGL`), respecting the aspect of the
     stream: fewer pixels in `swscale` and in the texture upload (win also
     outside the overlap: the mini bar 112×63 converts almost for free). If the
     surface is larger than the stream, it stays at full resolution (the
     upscale is done by GL, as before). The guard of the walk is `Q_OS_WIN`
     (the child widgets do not appear in the `GetWindow` chain).

_Future (pending, decided 2026-09-24):_ port of the coverage detection to
**X11 for Puppy Linux** (x86 machine as a test bench for the FfmpegBackend
without Windows): same 250 ms timer + `XQueryTree` from the root down to our
top-level (`map_state == IsViewable`, geometry + `XTranslateCoordinates` to
screen coordinates), same rule → `setVideoOccluded`. The only platform-specific
thing is the DETECTION (`occludedByTopWindow`, today `#ifdef Q_OS_WIN`): the
plumbing (timer, `setVideoOccluded`, frame discard of the thread) is already neutral.
With a "smart" CMake (Linux build without mpv → `NEVERTUBE_NO_MPV` on Linux, or
force `NEVERTUBE_BACKEND=ffmpeg`; the FfmpegBackend already compiles on Linux via
pkg-config). On **Wayland it is NOT** possible (there is no global z-order). The scaling to the
surface already works on any Linux without touching anything.

## Mini player with live video (PlayerWidget reparent)

When leaving the Player page with playback active, the mini player shows
**the static thumbnail** and, below it, the `PlayerWidget` keeps
rendering for real. Reason: if the widget is hidden, mpv stops drawing
and accumulates frames that, on returning, it plays in a burst to match the audio
(the "jump on return", worse the longer it spends in mini). `vid=no`/`framedrop`
force reconfig or discards that show up as flicker. In contrast, reparenting
the `PlayerWidget` to the mini slot and leaving it visible (even if covered by the
thumbnail) keeps the pipeline hot WITHOUT decoder reconfig and WITHOUT
accumulated frames.

- `WatchPage::detachPlayerTo(host)` reparents the `PlayerWidget` to the host of the
  mini bar and hides the controls overlay; `attachPlayer()` returns it to the
  `PlayerContainer` (which reintegrates it into its layout and recomputes the overlay).
  The overlay is a child of the player and moves with it (it is not re-parented).
- Reparenting **within the same top-level window preserves the GL context and
  the `mpv_render_context`** (verified: `initializeGL` only once in the whole
  mini→return cycle). Do NOT reparent to another window: it would destroy the context.
- `MainWindow::updateMiniPlayer` manages the transitions on state
  changes (guard `m_miniDetached`); `applyMiniVideoMode()` applies the
  `mini_video` setting (`video` → the live video fills the slot and the thumbnail is
  hidden; `thumb` → the thumbnail goes ON TOP with `raise()` and the video
  renders invisible underneath at 112x63). In both modes the `PlayerWidget` stays
  visible and rendering: only what covers what changes.
- Important order: `MainWindow::openWatchAt` switches to the Player page BEFORE
  activating the queue; if it is activated first, `updateMiniPlayer` sees "playing
  outside Player" and hooks/unhooks the mini for one frame (flicker when
  opening a video).
- `PlayerContainer` (previously in an anonymous namespace) lives in `watch_page.cpp` at global scope
  only so it can be referenced from `WatchPage` (member `m_playerContainer`).
