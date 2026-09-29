#include "theme.h"

#include <QColor>

namespace nt {

// Fuente única de la paleta de acentos (labels de Ajustes, hex persistido y
// sufijo de los PNG pre-pintados de la navegación). El defecto es el rojo
// NeverTube histórico (#FF0033). Mantener en sync con tools/gen_nav_accent.cpp
// solo en el sentido de que regenerar los PNG tras cambiar AQUÍ es obligatorio.
QVector<AccentOption> accentOptions()
{
    return {
        {QStringLiteral("Rojo (por defecto)"), QStringLiteral("#FF0033"),
         QStringLiteral("rojo")},
        {QStringLiteral("Azul"), QStringLiteral("#1E88E5"),
         QStringLiteral("azul")},
        {QStringLiteral("Verde"), QStringLiteral("#22C55E"),
         QStringLiteral("verde")},
        {QStringLiteral("Naranja"), QStringLiteral("#F97316"),
         QStringLiteral("naranja")},
        {QStringLiteral("Púrpura"), QStringLiteral("#A855F7"),
         QStringLiteral("purpura")},
        {QStringLiteral("Cian"), QStringLiteral("#22D3EE"),
         QStringLiteral("cian")},
        {QStringLiteral("Rosa"), QStringLiteral("#EC4899"),
         QStringLiteral("rosa")},
    };
}

QString accentSuffix()
{
    const QString cur = kAccent.name(); // "#rrggbb" minúscula
    const auto opts = accentOptions();
    for (const AccentOption &o : opts)
        if (QColor(o.hex).name() == cur)
            return o.suffix;
    return opts.value(0).suffix;
}

QString styleSheet()
{
    QString qss = QStringLiteral(R"QSS(
QWidget {
    background-color: transparent;
    color: #ECECEC;
    font-size: 13px;
}
QMainWindow { background-color: #0F0F0F; }

/* (Sin reglas para shellPanel/contentHost: ningún setObjectName del código las
   usaba, eran restos del layout split revertido. Verificado por grep.) */

QWidget#navBottom {
    background-color: #101010;
    /* Hairline superior: separa la barra del contenido (y del mini player, que
       ya tiene la suya) sin añadir un widget. */
    border-top: 1px solid @BORDER@;
}

/* Botones de la barra de navegación inferior. Los iconos (PNG) los gestiona
   NavButton por estado; aquí solo se colorean texto y fondo. El acento
   (rojo NeverTube por defecto) aparece únicamente en estados activos o de
   interacción. */
QToolButton#navBtn {
    background: transparent;
    border: none;
    border-radius: 12px;
    /* Cada botón es una cápsula de 104x52 (mainwindow.cpp setupNav) y el
       contenido icono 26 + hueco + línea de texto 11px necesita ~46: con 3px
       por arriba y por abajo caben 52. Bajar el alto recorta el texto o el
       icono, no los encoge. */
    padding: 3px 0;
    color: #8F8F8F;
    font-size: 11px;
}
QToolButton#navBtn:hover { background: @HOVER@; color: #ECECEC; }
/* ORDEN IMPORTANTE: :checked va DESPUÉS de :focus. Tienen la misma
   especificidad y gana la última regla; con :focus al final, el foco pisaba el
   relleno de acento y la pestaña recién clicada salía gris con el texto del
   acento sobre #242424 = 3,92:1 (por debajo del mínimo AA de 4,5:1). */
QToolButton#navBtn:focus { background: @HOVER@; color: #ECECEC; }
QToolButton#navBtn:checked { background: @ACCENT_A10@; color: @ACCENT@; }
/* Activo en hover/foco: el relleno se queda al 10% y el texto sube a la
   variante hover del acento (#FF1A47) para quedar en 4,72:1; subir el relleno al
   16% bajaba el texto a 4,32:1. */
QToolButton#navBtn:checked:hover { background: @ACCENT_A10@; color: @ACCENT_HOVER@; }
QToolButton#navBtn:checked:focus { background: @ACCENT_A10@; color: @ACCENT_HOVER@; }
QToolButton#navBtn:pressed { background: @ACCENT_A10@; color: @ACCENT_HOVER@; }
QToolButton#navBtn:disabled { color: @DISABLED@; }

QLabel { background: transparent; }
QLabel#sectionTitle { font-size: 16px; font-weight: 600; color: #ECECEC; }
QLabel#sectionSub { font-size: 12px; color: #8F8F8F; }
QLabel#muted { color: #8F8F8F; }

QPushButton {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    border-radius: 8px;
    padding: 7px 14px;
    color: #ECECEC;
}
QPushButton:hover { background-color: @HOVER@; }
QPushButton:pressed { background-color: @SELECTION@; }
/* Foco por COLOR de borde, no por grosor: el botón ya reserva 1px, así que
   añadir un anillo de 2 px desplazaría el contenido en cada cambio de estado. */
QPushButton:focus { border-color: @ACCENT@; }
/* El botón deshabilitado solo perdía el color: conservaba fondo y borde, así
   que seguía leyéndose como pulsable. Aquí baja emphasis en los tres canales. */
QPushButton:disabled {
    color: @DISABLED@;
    background-color: @BORDER@;
    border-color: @BORDER@;
}
QPushButton#primary { background-color: @ACCENT@; border-color: @ACCENT@; color: #FFFFFF; font-weight: 600; }
/* Faltaba :pressed. Como :hover ya apuntaba a @ACCENT_PRESSED@, hover y
   pressed se veían idénticos y el segundo nunca se veía. Ahora hover va a la
   variante clara del acento y pressed a la oscura. */
QPushButton#primary:hover { background-color: @ACCENT_HOVER@; }
QPushButton#primary:pressed { background-color: @ACCENT_PRESSED@; }
QPushButton#primary:disabled {
    color: @DISABLED_ON_ACCENT@; background-color: @ACCENT_MUTED@;
    border-color: @ACCENT_MUTED@;
}
QPushButton#ghost { background: transparent; border: 1px solid @BORDER@; border-radius: 6px; }
QPushButton#ghost:hover { background: #1F1F1F; }
QPushButton#ghost:pressed { background: @HOVER@; }
QPushButton#ghost:focus { border-color: @ACCENT@; }
QPushButton#iconBtn { background: transparent; border: none; border-radius: 6px; padding: 6px; }
QPushButton#iconBtn:hover { background: @HOVER@; }
QPushButton#iconBtn:pressed { background: @SELECTION@; }
/* iconBtn no tiene borde, así que el foco va por fondo (igual que navBtn). */
QPushButton#iconBtn:focus { background: @HOVER@; color: @ACCENT@; }
QPushButton#iconBtn:disabled { color: @DISABLED@; }
QPushButton#danger { color: @ACCENT@; }

QLineEdit, QComboBox {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    border-radius: 10px;
    padding: 8px 12px;
    selection-background-color: @ACCENT@;
    color: #ECECEC;
}
QLineEdit:focus, QComboBox:focus { border-color: @ACCENT@; }
QLineEdit:disabled, QComboBox:disabled { color: @DISABLED@; background-color: @BORDER@; }

/* Popover no modal (p. ej. crear playlist): panel flotante que no bloquea
   el resto de la página. */
QFrame#popupDialog {
    background-color: @POPOVER@;
    border: 1px solid @BORDER@;
    border-radius: 12px;
}
QLabel#popupTitle { font-size: 15px; font-weight: 600; color: #ECECEC; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #8F8F8F;
    margin-right: 8px;
}
QComboBox QAbstractItemView {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    selection-background-color: @SELECTION@;
    color: #ECECEC;
    outline: 0;
}

QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: @HANDLE@; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: @HANDLE_HOVER@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: @HANDLE@; border-radius: 5px; min-width: 24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

QSlider::groove:horizontal { height: 4px; background: @HANDLE@; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @ACCENT@; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 12px; height: 12px; margin: -4px 0;
    background: #FFFFFF; border-radius: 6px;
}
QSlider::handle:horizontal:hover { background: @ACCENT@; }
QSlider:disabled::sub-page:horizontal { background: @SELECTION@; }
QSlider:disabled::handle:horizontal { background: @DISABLED@; }

QProgressBar { border: none; background: @BORDER@; border-radius: 3px; text-align: center; color: transparent; }
QProgressBar::chunk { background: @ACCENT@; border-radius: 3px; }

QToolButton { background: transparent; border: none; border-radius: 6px; padding: 5px; }
QToolButton:hover { background: @HOVER@; }
QToolButton:pressed { background: @SELECTION@; }
QToolButton:focus { background: @HOVER@; }

/* El objeto #cards lo pone VideoListView, que deriva de QListView (no de
   QListWidget): con el selector "QListWidget#cards" la regla NO se aplicaba
   (verificado) y la vista conservaba el marco por defecto de
   QAbstractScrollArea. Ese marco dejaba el viewport 2 px mas bajo que el
   contenido de las secciones del Home (altura fija = filas * alto de celda),
   o sea un rango de scroll INTERNO de 2 px: al pasar la rueda por encima, la
   seccion daba un "pequeno scroll vertical". Con el selector correcto el rango
   es 0 en cualquier tamano y ajuste. */
QListView#cards {
    background: transparent;
    border: none;
    outline: 0;
}
QListView#cards::item { background: transparent; border: none; }
QListView#cards::item:selected { background: transparent; }

QMenu { background-color: #1F1F1F; border: 1px solid @BORDER@; }
QMenu::item { padding: 7px 24px; }
QMenu::item:selected { background-color: @SELECTION@; }
QMenu::item:disabled { color: @DISABLED@; }

QStatusBar { background: #101010; color: #8F8F8F; border-top: 1px solid #1F1F1F; }

/* El mini player usaba kPanel (#181818), el mismo que la nav inferior, así que
   las dos barras se fundían en una sola línea. kElevated los separa. */
QWidget#miniBar { background-color: #1F1F1F; border-top: 1px solid @BORDER@; }
QWidget#watchBody { background-color: #0F0F0F; }
QWidget#cardPanel { background-color: #181818; border: 1px solid @BORDER@; border-radius: 10px; }

/* Caja de controles del reproductor. El separador de 12 px que la hace
   flotante no es de QSS sino del layout (gl1 en watch_page.cpp); aqui van solo
   los bordes. El de arriba va en blanco al 28 % y el resto en el gris de
   borde: por separado ninguno lee contra todo video (1,33:1 el gris en oscuro,
   1,04:1 el blanco en claro), juntos siempre hay un canto que la delimita. */
QWidget#playerOverlay {
    background-color: @OVERLAY_BG@;
    border: 1px solid @BORDER@;
    border-top: 1px solid @OVERLAY_EDGE@;
    border-radius: 12px;
}
QLabel#ovTime { color: #ECECEC; font-size: 12px; }

/* --- Pagina Player ------------------------------------------------------
   Los dos overlays (spinner y error) viven dentro de m_overlay, sobre el
   video, asi que los dos son transparentes: cualquier fondo taparia el frame. */
QLabel#ovSpinner {
    background: transparent; color: @MUTED@; font-size: 13px;
}
QLabel#ovError {
    background: transparent; color: @TEXTSOFT@;
    font-size: 14px; padding: 16px;
}
/* Columna de informacion: metadatos y descripcion. */
QLabel#watchMeta { color: @METATEXT@; font-size: 12px; }
QLabel#watchDesc { color: @METATEXT@; font-size: 12px; background: transparent; }
/* Titulo del bloque "A continuacion" y contador a su derecha. */
QLabel#queueTitle { font-weight: 600; font-size: 13px; color: @TEXT@; }
QLabel#queueCount { color: @METATEXT@; font-size: 12px; }

/* Boton "…mas"/"…menos" de la descripcion colapsada: enlace discreto, no una
   capsula (no compite con las acciones). */
QPushButton#descToggle {
    background: transparent; border: none; padding: 0;
    color: @MUTED@; font-size: 12px; font-weight: 600; text-align: left;
}
QPushButton#descToggle:hover { color: @TEXT@; }
/* Boton del canal bajo el titulo: icono a la izquierda y texto a la derecha.
   No es #ghost porque necesita padding de 8px en vez de 6px. */
QPushButton#channelBtn {
    background: transparent; border: none; border-radius: 8px;
    text-align: left; padding: 8px;
    color: @TEXT@; font-weight: 600; font-size: 13px;
}
QPushButton#channelBtn:hover { background: #1F1F1F; }
/* Botones de accion del player: icono solo, en pastilla circular. El estado lo
   decide la propiedad booleana `active`, igual que #subscribe con la suya; el
   color del icono lo pinta updateActionButtons (el QSS no tinta QIcons). */
QPushButton#actionChip {
    background: #1F1F1F; border-radius: 20px; padding: 0;
}
QPushButton#actionChip:hover { background: @HOVER@; }
QPushButton#actionChip[active="true"] { background: @BORDER@; }
QPushButton#actionChip[active="true"]:hover { background: @SELECTION@; }

/* Grupo segmentado Me gusta / No me gusta: las esquinas exteriores redondeadas
   y un hairline entre los dos. El fondo del grupo es el del chip. */
QWidget#actionSeg { background: #1F1F1F; border-radius: 20px; }
QPushButton#actionSegL, QPushButton#actionSegR {
    background: transparent; border: none; padding: 0;
}
QPushButton#actionSegL:hover, QPushButton#actionSegR:hover { background: @HOVER@; }
QPushButton#actionSegL {
    border-top-left-radius: 20px; border-bottom-left-radius: 20px;
    border-right: 1px solid @BORDER@;
}
QPushButton#actionSegR {
    border-top-right-radius: 20px; border-bottom-right-radius: 20px;
}
QPushButton#actionSegL[active="true"], QPushButton#actionSegR[active="true"] {
    background: @BORDER@;
}
QPushButton#actionSegL[active="true"]:hover,
QPushButton#actionSegR[active="true"]:hover { background: @SELECTION@; }

/* Chip de calidad. Antes llevaba objectName="iconBtn", que es el estilo de un
   boton cuadrado de icono (padding 6, sin borde), pero lleva texto: sin
   relleno ni borde no se leia como control. Ancho por contenido, no 98 fijos. */
QPushButton#chip {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    border-radius: 6px;
    padding: 5px 10px;
    color: #ECECEC;
}
QPushButton#chip:hover { background-color: @HOVER@; border-color: @HANDLE_HOVER@; }
QPushButton#chip:pressed { background-color: @SELECTION@; }
QPushButton#chip:focus { border-color: @ACCENT@; }

/* Pista de los dos sliders de la caja. Selector PROPIO (#ovSeek / #ovVolume) a
   proposito: la regla QSlider global es compartida con el de volumen de la mini
   bar y el de Ajustes, que no van sobre un panel translucido y no necesitan
   contorno. El global es de 4 px; aqui 6 px para que el contorno de 1 px no
   se coma el relleno. El handle baja de margin:-4px a -3px para seguir
   centrado sobre una pista 2 px mas alta. */
QSlider#ovSeek::groove:horizontal, QSlider#ovVolume::groove:horizontal {
    height: 6px; background: @HANDLE@;
    border: 1px solid @OVERLAY_EDGE@; border-radius: 3px;
}
QSlider#ovSeek::sub-page:horizontal, QSlider#ovVolume::sub-page:horizontal {
    background: @ACCENT@; border-radius: 2px;
}
QSlider#ovSeek::handle:horizontal, QSlider#ovVolume::handle:horizontal {
    width: 12px; height: 12px; margin: -3px 0;
    background: #FFFFFF; border-radius: 6px;
}
QSlider#ovSeek::handle:horizontal:hover, QSlider#ovVolume::handle:horizontal:hover {
    background: @ACCENT@;
}

/* Menu de calidad: los tres stylesheet inline de watch_page.cpp pasan aqui. */
QFrame#qualityMenu {
    background-color: #1F1F1F; border: 1px solid @SELECTION@; border-radius: 6px;
}
QFrame#qualityMenu QPushButton {
    background: transparent; color: #ECECEC; border: none;
    text-align: left; padding: 6px 8px; border-radius: 4px;
}
QFrame#qualityMenu QPushButton:hover { background: @SELECTION@; }

/* Fila de una consulta buscada antes (pestaña Historial del buscador).
   Es un QPushButton plano porque necesita text-align:left con icono a la
   izquierda, cosa que la regla base de QPushButton no da. */
QPushButton#historyRow {
    text-align: left; background: transparent; border: none;
    color: @TEXTSOFT@; padding: 7px 4px;
}
QPushButton#historyRow:hover { background: #1F1F1F; border-radius: 6px; }

/* --- Ajustes ------------------------------------------------------------
   Tres niveles de encabezado, y a proposito NO se reutilizan entre si:
     pageHeading           18 px, titulo de la pagina entera
     sectionTitle          16 px, titulo de seccion (compartido con las demas
                           paginas, que ya lo usaban)
     settingsSectionTitle  11 px en versalitas con tracking, encabezado de
                           seccion de Ajustes ("INTERFAZ", "DATOS"...)
   Y las filas de ajuste: etiqueta a la izquierda con ancho minimo para que
   todos los menus encajen, valor a la derecha. */
QLabel#pageHeading { font-size: 18px; font-weight: 700; }
QLabel#settingsSectionTitle {
    font-size: 11px; font-weight: 600; color: @MUTED@;
    letter-spacing: 0.5px;
}
QLabel#settingsRowLabel { color: @TEXT@; min-width: 200px; }
QLabel#settingsRowValue { color: @MUTED@; }
QLabel#settingsNote { font-size: 10px; color: @MUTED@; }
QLabel#settingsVersion { font-size: 12px; color: @TEXT@; }
QLabel#popupText { color: @MUTED@; }
/* El area de scroll de Ajustes no pinta fondo: la pagina ya tiene el suyo. */
QScrollArea#settingsScroll { background: transparent; }

/* Botones de accion de Ajustes. #actionBtn y #dataActionBtn son el mismo
   aspecto con padding algo mayor (filas de 150 px de ancho minimo frente a las
   de la seccion DATOS); se mantienen separados para no cambiar la caja. */
QPushButton#actionBtn {
    font-size: 12px; padding: 7px 14px; background-color: #1F1F1F;
    border: 1px solid @BORDER@; border-radius: 8px; color: @TEXT@;
}
QPushButton#dataActionBtn {
    font-size: 12px; padding: 8px 16px; background-color: #1F1F1F;
    border: 1px solid @BORDER@; border-radius: 8px; color: @TEXT@;
}
/* "Restablecer aplicacion": mismo fondo que los anteriores y solo el borde y
   el texto en acento, para que se lea como destructivo sin ser un boton
   primario. El acento sale de @ACCENT@, asi que se repinta solo. */
QPushButton#accentOutline {
    font-size: 12px; padding: 8px 16px; background-color: #1F1F1F;
    border: 1px solid @ACCENT@; border-radius: 8px;
    color: @ACCENT@; font-weight: 600;
}

/* Boton Suscribirse de la ficha de canal: los dos estados en el QSS global, con
   la propiedad booleana `subscribed` como selector. El acento sale de
   @ACCENT@, asi que un cambio de color lo repinta sin tocar el C++.
   NO se pone `border: none`: los setStyleSheet que sustituye no tocaban el
   borde, asi que el boton conservaba el 1px @BORDER@ de la regla base. En el
   estado suscrito no se nota (borde y fondo son el mismo gris), pero en el de
   acento es visible. */
QPushButton#subscribe {
    background: @ACCENT@; border-radius: 18px; padding: 0 18px;
    color: #FFFFFF; font-weight: 600; font-size: 13px;
}
QPushButton#subscribe:hover { background: @ACCENT_HOVER@; }
QPushButton#subscribe[subscribed="true"] { background: @BORDER@; color: @TEXT@; }
QPushButton#subscribe[subscribed="true"]:hover { background: @SELECTION@; }

/* Filas compactas de canales (channel_row) */
QLabel#channelRowTitle { color: @TEXT@; background: transparent; }
QLabel#channelRowMeta  { color: @MUTED@; background: transparent; }

/* Placeholder y títulos de página */
QLabel#pageTitle { font-size: 18px; font-weight: 700; }
QLabel#placeholderHint { color: @MUTED@; font-size: 14px; padding: 40px; }
QLabel#sectionTitle { font-size: 22px; font-weight: 700; color: @TEXT@; }

/* Avatar de la ficha de canal (64x64, radio 32) */
QLabel#channelAvatar { border-radius: 32px; }

/* Chip de acento plano: el boton "Ir a Buscar" del estado vacio del Home.
   Se migra tal cual estaba, SIN :hover ni :pressed a proposito: el QSS global
   tiene reglas de estado para #primary y aqui no las habia, y anadirlas seria
   cambiar comportamiento en un refactor. */
QPushButton#accentChip {
    background-color: @ACCENT@; border: none; border-radius: 8px;
    color: #FFFFFF; font-size: 12px; padding: 8px 18px;
}

/* Estado vacio del Home (va sobre un popupDialog) */
QLabel#emptyStateTitle {
    color: @TEXT@; font-size: 15px; font-weight: 700;
    border: none; background: transparent;
}
QLabel#emptyStateBody {
    color: @METATEXT@; font-size: 12px;
    border: none; background: transparent;
}

/* Filas de la Biblioteca (playlists, historial: mismo item card) */
QLabel#libraryItemTitle {
    color: @TEXT@; font-weight: 600; font-size: 14px;
    background: transparent;
}
QLabel#libraryItemMeta {
    color: @MUTED@; font-size: 12px; background: transparent;
}

/* Cartelón "Detectando hardware" (hwProbePopup). Estaba entero en un
   setStyleSheet inline con rgba(18,18,18,0.96) y #3a3a3a, que son valores que
   el QSS global no tenía: por eso era otra superficie que el cambio de acento
   no cubría. */
QWidget#hwProbePopup {
    background-color: @POPOVER@;
    border: 1px solid @HANDLE@;
    border-radius: 10px;
}
QLabel#hwPopupTitle { color: #FFFFFF; font-size: 16px; font-weight: 600; }
QLabel#hwPopupBody { color: @TEXTSOFT@; font-size: 12px; }

/* Mini player. El hueco en sí (miniVideoHost) no necesita regla: hereda el
   fondo transparente del QWidget global, que es lo correcto, porque por debajo
   va el PlayerWidget en vivo reparentado. */
QLabel#miniThumb {
    background-color: @BORDER@; border-radius: 6px;
    color: @MUTED@; font-size: 10px;
}
QLabel#miniTime { color: @MUTED@; font-size: 11px; }
QLabel#miniTitle {
    color: @TEXT@; font-size: 12px; font-weight: 600;
    background: transparent; border: none;
}

/* Área de vídeo. Negro puro, NO kBg (#0F0F0F): es el fondo de las bandas
   letterbox cuando el vídeo no llena la caja, y tiene que ser negro de verdad
   para que no se note el marco. Estaba en un setStyleSheet inline. */
QWidget#playerArea { background-color: #000000; }
)QSS");
    // Los placeholders se sustituyen en CADA llamada: un cambio de acento en
    // caliente (Ajustes → Color de acento) regenera el stylesheet al momento.
    // Los neutros también son placeholders: antes el QSS escribía #2A2A2A para
    // bordes y #232323 para hover mientras theme.h tenía kBorder #262626 y
    // kHover #242424, así que un mismo rol tenía dos valores según por dónde se
    // pintara. Ahora hay una sola fuente.
    return qss.replace(QStringLiteral("@ACCENT@"), accentHex())
              .replace(QStringLiteral("@ACCENT_PRESSED@"), accentPressedHex())
              .replace(QStringLiteral("@ACCENT_HOVER@"), accentHoverHex())
              .replace(QStringLiteral("@ACCENT_A10@"), accentAlpha(26))
              .replace(QStringLiteral("@ACCENT_A16@"), accentAlpha(41))
              .replace(QStringLiteral("@ACCENT_MUTED@"),
                       accentPressed(kAccent).darker(160).name())
              .replace(QStringLiteral("@DISABLED_ON_ACCENT@"),
                       QColor(0xFF, 0xFF, 0xFF).name())
              .replace(QStringLiteral("@BORDER@"), kBorder.name())
              .replace(QStringLiteral("@HOVER@"), kHover.name())
              .replace(QStringLiteral("@DISABLED@"), kDisabled.name())
              .replace(QStringLiteral("@SELECTION@"), kSelection.name())
              .replace(QStringLiteral("@HANDLE@"), kHandle.name())
              .replace(QStringLiteral("@HANDLE_HOVER@"), kHandleHover.name())
              .replace(QStringLiteral("@POPOVER@"), kPopover.name())
              .replace(QStringLiteral("@MUTED@"), kMuted.name())
              .replace(QStringLiteral("@TEXT@"), kText.name())
              .replace(QStringLiteral("@METATEXT@"), kMetaText.name())
              .replace(QStringLiteral("@TEXTSOFT@"), kTextSoft.name())
              .replace(QStringLiteral("@OVERLAY_BG@"), overlayBgRgba())
              .replace(QStringLiteral("@OVERLAY_EDGE@"), overlayEdgeRgba());
}

} // namespace nt