#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QRawFont>
#include <QHash>
#include <QStringList>

#include <algorithm>

#include <cstdio>

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
    // El QSS es un bloque de ~30 KB. MSVC NO admite un literal de cadena de
    // mas de 16380 bytes (error C2026, "string too big, trailing characters
    // truncated"), asi que el literal entero no compila en Windows aunque en
    // Linux/GCC no haya ese limite. Va partido en trozos en un array y se une
    // aqui. Cada trozo esta muy por debajo del limite y el resultado es el
    // MISMO qss de siempre, byte a byte, asi que --selftest-theme sigue viendo
    // la hoja completa. NO volver a levantar este bloque en un solo literal.
    static const char *const kTrozos[] = {
        R"QSS(QWidget {
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
/* Titulo del video en la pagina Player: un punto mas grande que los titulos de
   seccion (es el H1 de la pagina). */
QLabel#watchTitle { font-size: 17px; font-weight: 600; color: @TEXT@; }
QLabel#sectionSub { font-size: 12px; color: #8F8F8F; }
/* Estado vacio de una seccion de la Biblioteca: caja con borde punteado y el
   texto centrado, en vez de una linea suelta de texto gris. */
QLabel#emptyState {
    color: @METATEXT@; font-size: 13px;
    background: #181818; border: 1px dashed @BORDER@; border-radius: 10px;
    padding: 18px;
}
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
/* Ghost SOLO con icono (el X de limpiar del buscador). El padding global
   (7px 14px) deja 2x2 px de contenido dentro de los 28 px del boton, asi que
   sin esto el icono se sale. Es un selector de propiedad para que el boton
   siga heredando el resto de #ghost. */
QPushButton#ghost[iconOnly="true"] { padding: 0; }
QPushButton#iconBtn { background: transparent; border: none; border-radius: 6px; padding: 6px; }
QPushButton#iconBtn:hover { background: @HOVER@; }
QPushButton#iconBtn:pressed { background: @SELECTION@; }
/* iconBtn no tiene borde, así que el foco va por fondo (igual que navBtn). */
QPushButton#iconBtn:focus { background: @HOVER@; color: @ACCENT@; }
QPushButton#iconBtn:disabled { color: @DISABLED@; }

QLineEdit, QComboBox {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    border-radius: 10px;
    padding: 8px 12px;
    /* Seleccionar texto en el buscador va en el gris de la app, como el resto de
       la app, no en el acento: en rojo parecía un error. Es el mismo token que
       los menús (kSelection) y el texto seleccionado se queda en kText para que
       siga leyéndose (11:1). */
    selection-background-color: @SELECTION@;
    selection-color: @TEXT@;
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
/* El diálogo de confirmación (nt::confirm) NO es un QDialog: es un QWidget hijo
   de la ventana (una ventana propia es un toplevel y le roba el foco y el
   puntero al usuario). Por eso ya no hace falta la regla QDialog { background:
   transparent } que existed solo para que su marco se vieran con las esquinas
   redondeadas. */
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

)QSS",
        R"QSS(QScrollArea { border: none; background: transparent; }
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
/* Tarjeta de la descripcion del Player (agrupa el texto y el "…mas"/"…menos"),
   con el degradado inferior que se usa cuando la descripcion esta colapsada. */
QFrame#watchDescCard {
    background: #181818; border: 1px solid @BORDER@; border-radius: 10px;
}
QFrame#descFade {
    border: none;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 rgba(24,24,24,0), stop:1 #181818);
}
/* Titulo del bloque "A continuacion" y contador a su derecha. */
QLabel#queueTitle { font-weight: 600; font-size: 13px; color: @TEXT@; }
QLabel#queueCount {
    color: @METATEXT@; font-size: 11px;
    background: #1F1F1F; border: 1px solid @BORDER@; border-radius: 9px;
    padding: 1px 8px;
}

/* Boton "…mas"/"…menos" de la descripcion colapsada: enlace discreto, no una
   capsula (no compite con las acciones). */
QPushButton#descToggle {
    background: transparent; border: none; padding: 0;
    color: @MUTED@; font-size: 12px; font-weight: 600; text-align: left;
}
QPushButton#descToggle:hover { color: @TEXT@; }
/* Accion terciaria al final de una cabecera de seccion (Biblioteca: "Ver
   todo", "Administrar"): enlace discreto, no la capsula #ghost con marco. Eran
   cinco capsulas con borde alineadas a la derecha de cada titulo, que competian
   con el contenido de la seccion. Sin borde en la base, asi que el :focus global
   solo se ve si el boton llega a tener foco (igual que #descToggle). */
QPushButton#sectionLink {
    background: transparent; border: none; padding: 4px 8px;
    color: @MUTED@; font-size: 12px; font-weight: 600;
}
QPushButton#sectionLink:hover { color: @TEXT@; }
/* Sin borde no hay donde dibujar el anillo de foco, asi que con el teclado el
   enlace se marca con el texto mas claro. Es lo que hace el hover, para que
   "tabular" por la Biblioteca se vea igual que pasar el raton. */
QPushButton#sectionLink:focus { color: @TEXT@; }
/* "Volver" de las cuatro superficies con vuelta atrás (Canal, Historial,
   Playlist y el Player). Es un ENLACE, no una cápsula: mismo criterio que
   #sectionLink, porque "volver" es una acción de navegación, no un control de
   la página. Antes era #ghost (una cápsula con borde) en las tres páginas y un
   botón flotante sobre el vídeo en el Player, que además tapaba la imagen.
   El icono lo dibuja nt::icon (QPainter), no un glifo del texto: el "◀" del
   "Volver" era un carácter que no existe en la fuente de la app. */
QPushButton#backLink {
    background: transparent; border: none; padding: 4px 8px;
    color: @MUTED@; font-size: 13px; font-weight: 600; text-align: left;
}
QPushButton#backLink:hover { color: @TEXT@; }
/* Sin borde no hay donde dibujar el anillo de foco (como #sectionLink): el
   teclado se marca con el texto más claro, igual que el hover. */
QPushButton#backLink:focus { color: @TEXT@; }
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
   color del icono lo pinta updateActionButtons (el QSS no tinta QIcons).
   SIN MARCO (regla del usuario, igual que Me gusta / No me gusta): el borde va
   transparente y el :focus se lo queda transparente, porque el
   QPushButton:focus global lo pinta en el acento y al pulsar salia un aro rojo. */
QPushButton#actionChip {
    background: #1F1F1F; border: 1px solid transparent; border-radius: 22px;
    padding: 0;
}
QPushButton#actionChip:hover { background: @HOVER@; }
QPushButton#actionChip:focus { border-color: transparent; }
QPushButton#actionChip[active="true"] { background: @BORDER@; }
QPushButton#actionChip[active="true"]:hover { background: @SELECTION@; }

)QSS",
        R"QSS(/* Grupo segmentado Me gusta / No me gusta: las esquinas exteriores redondeadas
   y un hairline entre los dos. El fondo del grupo es el del chip. */
QWidget#actionSeg { background: #1F1F1F; border-radius: 22px; }
QPushButton#actionSegL, QPushButton#actionSegR {
    background: transparent; border: none; padding: 0;
}
QPushButton#actionSegL:hover, QPushButton#actionSegR:hover { background: @HOVER@; }
QPushButton#actionSegL {
    border-top-left-radius: 22px; border-bottom-left-radius: 22px;
    border-right: 1px solid @BORDER@;
}
QPushButton#actionSegR {
    border-top-right-radius: 22px; border-bottom-right-radius: 22px;
}
QPushButton#actionSegL[active="true"], QPushButton#actionSegR[active="true"] {
    background: @BORDER@;
}
QPushButton#actionSegL[active="true"]:hover,
QPushButton#actionSegR[active="true"]:hover { background: @SELECTION@; }

/* Chip de calidad. Antes llevaba objectName="iconBtn", que es el estilo de un
   boton cuadrado de icono (padding 6, sin borde), pero lleva texto: sin
   relleno no se leia como control. Ancho por contenido, no 98 fijos.

   REGLA (del usuario): los botones multimedia NO llevan marco. Ni en reposo ni
   al pulsarlos: por eso el borde va "transparent" y no "none" (no se ve ninguno
   pero conserva los 2 px para que el contenido no se desplace) y el :focus lo
   anula en vez de pintar el acento, que es lo que hace la regla global
   QPushButton:focus y lo que hacia que el chip encendiera un marco al
   pulsarlo. El foco se marca SOLO con el relleno, que es lo unico que se puede
   tocar sin dibujar un borde. El relleno es lo que marca el boton como control,
   no el marco. */
QPushButton#chip {
    background-color: #1F1F1F;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 5px 10px;
    color: #ECECEC;
}
QPushButton#chip:hover { background-color: @HOVER@; }
QPushButton#chip:pressed { background-color: @SELECTION@; }
QPushButton#chip:focus {
    border-color: transparent; background-color: @HANDLE_HOVER@;
}

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

/* Desplegable de sugerencias del buscador. Reaprovecha las variables de color
   del popover (#202020) y el hover de historyRow para que no introduce tonos
   nuevos. suggestRow es la fila pulsable; suggestDivider separa los dos
   grupos (lo que sugiere YouTube arriba, lo que ya buscaste abajo). */
QFrame#suggestPanel {
    background-color: @POPOVER@; border: 1px solid @BORDER@; border-radius: 10px;
}
/* Rotulo del segundo grupo, en mayusculas con la linea al lado (CSS ::after
   no existe en QSS, asi que la linea es un QFrame hermano: suggestDividerRule).
   El texto se pone en mayusculas en codigo, no aqui. */
QWidget#suggestDivider { background: transparent; }
QLabel#suggestGroupTitle {
    color: @MUTED@; font-size: 9px; font-weight: 600;
}
QFrame#suggestDividerRule { color: @BORDER@; background: @BORDER@; border: none; }
/* Fila del desplegable. Es un QWidget (no un boton) porque necesita negrita
   parcial, que Qt no dibuja en un boton. El hover es del raton y el
   resaltado [highlight] es del teclado: son estados distintos y el segundo
   tiene que poder moverse con las flechas sin que el raton lo pise.
   El borde IZQUIERDO se reserva siempre (3 px, transparente si no esta
   resaltada) para que la barra de acento no empuje el texto al aparecer. */
QWidget#suggestRow {
    background: transparent; border: none; border-left: 3px solid transparent;
    border-radius: 6px; color: @TEXTSOFT@;
}
QWidget#suggestRow:hover { background: @SELECTION@; }
/* Resaltado del teclado: barra de acento a la izquierda + fondo con un velo del
   acento. Antes era #1F1F1F, mas oscuro que el popover (#202020): no se veia
   nada. El texto se queda en kTextSoft/kMuted, que sobre el velo siguen dando
   8:1 y 4:1, asi que no hay que tocar los colores de las etiquetas hijas. */
QWidget#suggestRow[highlight="true"] {
    background: @ACCENT_A16@; border-left: 3px solid @ACCENT@;
}
/* Detalle de entidad ("Cantante y compositor peruano") a la derecha, en
   tenue, para que la consulta mande. */
QLabel#suggestDetail { color: @MUTED@; font-size: 12px; }
/* Insignia "Tab" de las filas del historial: es la tecla con la que se toman.
   Con caja, como el <kbd> del mockup. */
QLabel#suggestBadge {
    color: @MUTED@; font-size: 11px; border: 1px solid @BORDER@; border-radius: 4px;
}
/* El desplegable lleva barra cuando las filas pasan del tope de altura. La
   superficie va transparente para que se vea el fondo del panel, no el gris
   por defecto del viewport. */
QScrollArea#suggestScroll { background: transparent; border: none; }
QWidget#suggestRows { background: transparent; }

)QSS",
        R"QSS(/* --- Ajustes ------------------------------------------------------------
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
/* Tarjeta que agrupa las filas de una seccion de Ajustes: sustituye a la lista
   plana de filas sueltas separadas por hairlines. */
QFrame#settingsCard {
    background: #181818; border: 1px solid @BORDER@; border-radius: 10px;
}
QLabel#popupText { color: @MUTED@; }
/* El area de scroll de Ajustes no pinta fondo: la pagina ya tiene el suyo. */
QScrollArea#settingsScroll { background: transparent; }

/* Botones de accion de Ajustes. #actionBtn y #dataActionBtn son el mismo
   aspecto con padding algo mayor (filas de 150 px de ancho minimo frente a las
   de la seccion DATOS); se mantienen separados para no cambiar la caja. */
QPushButton#actionBtn {
    font-size: 12px; padding: 7px 14px; background-color: #1F1F1F;
    border: 1px solid @BORDER@; border-radius: 8px; color: @TEXT@;
    /* Ancho minimo comun: alinea la columna de controles dentro de la tarjeta. */
    min-width: 150px;
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
/* OJO: aqui NO va un QLabel#sectionTitle. Estuvo (22px/700, parte de la
   migracion a QSS global) y como va DESPUES de la regla de la linea 93 GANABA
   por orden, subiendo los titulos de seccion de TODAS las paginas de 16 a 22 px
   (Home, Descubre, Buscar, canal, historial, biblioteca, playlist). El diseno
   son 16 px: lo dice el comentario de settings_page.cpp ("A proposito NO es
   #sectionTitle (16 px)"). Con un selector duplicado el QSS no avisa de nada y
   el override es mudo, asi que la regla de #sectionTitle vive en un solo sitio. */

/* --- Pagina Canal ---------------------------------------------------------
   Antes era lo UNICO de la app sin estilo: las tres pestanas eran un
   QTabWidget y no hay ninguna regla QTabWidget/QTabBar en este QSS, asi que
   salian con el aspecto del tema del escritorio y el "pane" pintaba una columna
   casi negra (medido #020303) por debajo de la lista, mas oscura que el fondo de
   la pagina. Ahora las pestanas son botones dentro de una capsula con estilo
   propio, como la navegacion inferior.
   La ficha agrupa avatar, nombre, suscriptores, acciones y descripcion en una
   caja (#181818), igual que #settingsCard y #watchDescCard. */
QFrame#channelCard {
    background: #181818; border: 1px solid @BORDER@; border-radius: 12px;
}
/* Nombre del canal: es el H1 de la pagina, un punto por encima de los 18 px de
   #pageHeading porque es el dato al que se entra. */
QLabel#channelName { font-size: 19px; font-weight: 700; color: @TEXT@; background: transparent; }
QLabel#channelMeta { color: @METATEXT@; font-size: 12px; background: transparent; }
QLabel#channelDesc { color: @METATEXT@; font-size: 12px; background: transparent; }
/* Avatar de la ficha (80x80, radio 40). El circulo real lo hace
   nt::circularAvatar (recorta el pixmap): este radio solo redondea el fondo. */
QLabel#channelAvatar { border-radius: 40px; background: transparent; }
/* Capsula de las pestanas: el fondo es el de la ficha, con un pelo de borde
   igual que el resto de superficies. */
QWidget#channelTabs { background: #181818; border: 1px solid @BORDER@; border-radius: 9px; }
QPushButton#tabBtn {
    background: transparent; border: none; border-radius: 6px;
    padding: 6px 16px; color: @MUTED@; font-size: 12px; font-weight: 600;
}
/* El foco NO se marca: son pestanas, no botones de accion, y el aro del acento
   al pulsarlas se leeria como un error (mismo criterio que #iconBtn). */
QPushButton#tabBtn:focus { border-color: transparent; }
QPushButton#tabBtn:hover { background: #1F1F1F; color: @TEXT@; }
/* La seleccionada lleva el acento en el texto, como la nav inferior. El fondo no
   puede distinguirse de la capsula: los dos son #181818. */
QPushButton#tabBtn[selected="true"] { color: @ACCENT_HOVER@; }
/* Contador de videos de la pestana activa: misma pastilla que #queueCount. */
QLabel#tabCount {
    color: @METATEXT@; font-size: 11px;
    background: #1F1F1F; border: 1px solid @BORDER@; border-radius: 9px;
    padding: 1px 8px;
}

)QSS",
        R"QSS(/* Chip de acento plano: el boton "Ir a Buscar" del estado vacio del Home.
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
)QSS",
    };

    QString qss;
    qss.reserve(30627);
    for (const char *t : kTrozos)
        qss += QString::fromUtf8(t);

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

bool themeSelfTest()
{
    int fail = 0;
    const auto chk = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };

    QString qss = styleSheet();
    // Fuera comentarios: un /* ... */ puede contener llaves o nombres de
    // selector y ensuciaria el recuento.
    while (true) {
        const int a = qss.indexOf(QStringLiteral("/*"));
        if (a < 0)
            break;
        const int b = qss.indexOf(QStringLiteral("*/"), a + 2);
        if (b < 0) {
            qss.remove(a, qss.size() - a);
            break;
        }
        qss.remove(a, b + 2 - a);
    }

    QHash<QString, int> veces;
    // selector -> cuerpo, para poder comprobar el contenido de una regla concreta
    // (no solo que el selector exista).
    QVector<QPair<QString, QString>> cuerpos;
    int reglas = 0;
    int i = 0;
    while (i < qss.size()) {
        const int abre = qss.indexOf(QLatin1Char('{'), i);
        if (abre < 0)
            break;
        const int cierra = qss.indexOf(QLatin1Char('}'), abre + 1);
        if (cierra < 0)
            break;
        const QString sel = qss.mid(i, abre - i).simplified();
        if (!sel.isEmpty()) {
            ++reglas;
            veces[sel] += 1;
            cuerpos.append(qMakePair(sel, qss.mid(abre + 1, cierra - abre - 1)));
        }
        i = cierra + 1;
    }

    // Si el parser se rompe (p. ej. por un cambio de formato del QSS) el numero
    // de reglas se hunde y las dos comprobaciones de abajo pasarian por vacio.
    chk(reglas > 100 && veces.size() > 50,
        "el QSS se ha parseado (mas de 100 reglas y 50 selectores)");

    QStringList repetidos;
    for (auto it = veces.constBegin(); it != veces.constEnd(); ++it)
        if (it.value() > 1)
            repetidos << it.key();
    repetidos.sort();
    for (const QString &r : repetidos)
        std::fprintf(stderr, "    [info] selector repetido en el QSS: %s\n",
                     qPrintable(r));
    chk(repetidos.isEmpty(),
        "ningun selector se declara dos veces (el ultimo gana y no avisa)");

    // Guarda de la regresion concreta: #sectionTitle son 16 px por diseno (lo
    // dice el comentario de settings_page.cpp) y solo puede declararse una vez.
    // Estuvo duplicado a 22 px y subio los titulos de seccion de TODAS las
    // paginas (Home, Descubre, Buscar, canal, historial, biblioteca, playlist).
    chk(veces.value(QStringLiteral("QLabel#sectionTitle")) == 1,
        "QLabel#sectionTitle se declara exactamente una vez");

    // Regresion de las dos cosas que perdio el usuario de vista. La paleta no la
    // cubre el QSS (la descripcion del Player es un QLabel seleccionable y pinta
    // con QPalette::Highlight), asi que sin esto el texto seleccionado salia con
    // el color del tema del escritorio.
    QPalette pal;
    applyTextSelectionPalette(pal);
    chk(pal.color(QPalette::Highlight) == textSelectionBg(),
        "la seleccion de texto va en el gris de la app (kSelection)");
    chk(pal.color(QPalette::HighlightedText) == textSelectionFg(),
        "el texto seleccionado se queda en kText (11:1 sobre kSelection)");
    chk(qss.contains(QStringLiteral("selection-background-color: ")
                     + kSelection.name()),
        "el QSS selecciona texto en gris, no en otro color");
    chk(!qss.contains(QStringLiteral("selection-background-color: ")
                      + accentHex()),
        "ningun texto seleccionado se pinta en el acento (parecia un error)");

    // La fila de sugerencia resaltada por el teclado tiene que LLEVAR el acento:
    // se perdia porque era #1F1F1F, mas oscuro que el popover #202020.
    const int hl = qss.indexOf(QStringLiteral("QWidget#suggestRow[highlight=\"true\"]"));
    chk(hl > 0, "existe la regla de la sugerencia resaltada");
    if (hl > 0) {
        const int a = qss.indexOf(QLatin1Char('{'), hl);
        const int b = qss.indexOf(QLatin1Char('}'), a);
        const QString bloque = qss.mid(a, b - a);
        chk(bloque.contains(accentHex()),
            "la sugerencia resaltada lleva el acento (barra y velo)");
    }

    // REGLA del usuario: los botones multimedia NO llevan marco visible. El chip
    // de calidad (el que abre el menu de resoluciones) si lo llevaba y se veia
    // como una caja mas dentro de la caja de controles. Se admite "none" o
    // "transparent": este ultimo conserva los 2 px para que el foco, que en esta
    // app se senala pintando el color del borde, no desplace el contenido.
    const QStringList multimedia{
        QStringLiteral("QPushButton#chip"), QStringLiteral("QPushButton#iconBtn"),
        QStringLiteral("QPushButton#actionChip"),
        QStringLiteral("QFrame#qualityMenu QPushButton")};
    for (const QString &sel : multimedia) {
        // TODOS los estados del boton, no solo la regla base: el marco al
        // pulsarlo venia de :focus (que ademas hereda la global
        // QPushButton:focus), y un check solo sobre la base lo dejaba pasar.
        int estados = 0;
        QString culpable;
        for (const auto &par : cuerpos) {
            if (!par.first.startsWith(sel))
                continue;
            // Lo que venga detras del nombre tiene que ser un pseudo-estado o
            // una propiedad, nunca otro nombre: "#chip" no es "#chipFoo".
            const QChar sig = par.first.size() > sel.size()
                                  ? par.first.at(sel.size())
                                  : QChar();
            if (!sig.isNull() && sig != QLatin1Char(':') && sig != QLatin1Char('[')
                && !sig.isSpace())
                continue;
            ++estados;
            const QString &cuerpo = par.second;
            const bool transparente =
                cuerpo.contains(QStringLiteral("border: none"))
                || cuerpo.contains(QStringLiteral("border: 1px solid transparent"))
                || cuerpo.contains(QStringLiteral("border-color: transparent"));
            if (!cuerpo.contains(QStringLiteral("border")) || transparente)
                continue;
            culpable = par.first;
        }
        chk(estados > 0, qPrintable(QStringLiteral("existe la regla %1").arg(sel)));
        chk(culpable.isEmpty(),
            qPrintable(QStringLiteral("%1 no declara un borde visible en ningun "
                                      "estado (marco en: %2)")
                           .arg(sel,
                                culpable.isEmpty() ? QStringLiteral("-") : culpable)));
    }

    // Las pestanas del canal: no pueden quedarse sin marcar la seleccionada (el
    // QTabWidget no tenia ninguna regla y se veia como el escritorio), y el foco
    // no debe encender un aro de acento al pulsarlas.
    {
        const QString sel = QStringLiteral("QPushButton#tabBtn");
        chk(veces.value(sel) == 1, "QPushButton#tabBtn se declara exactamente una vez");
        const int act = qss.indexOf(sel + QStringLiteral("[selected=\"true\"]"));
        chk(act > 0, "existe la regla de la pestana seleccionada del canal");
        if (act > 0) {
            const int a = qss.indexOf(QLatin1Char('{'), act);
            const int b = qss.indexOf(QLatin1Char('}'), a);
            chk(qss.mid(a, b - a).contains(accentHex().toLower())
                    || qss.mid(a, b - a).contains(accentHoverHex()),
                "la pestana seleccionada del canal lleva el acento");
        }
        const int foc = qss.indexOf(sel + QStringLiteral(":focus"));
        chk(foc > 0, "las pestanas del canal anulan el :focus");
        if (foc > 0) {
            const int a = qss.indexOf(QLatin1Char('{'), foc);
            const int b = qss.indexOf(QLatin1Char('}'), a);
            chk(qss.mid(a, b - a).contains(QStringLiteral("transparent")),
                "el :focus de las pestanas del canal no pinta marco");
        }
    }

    std::fprintf(stderr, "THEME %s (%d reglas, %d selectores)\n",
                 fail == 0 ? "OK" : "FAIL", reglas, (int)veces.size());
    return fail == 0;
}

namespace {

// Quita los comentarios de una linea (respetando el estado de bloque) y
// devuelve solo el codigo. Sin esto se leen los comentarios como si fueran
// literales y cada glifo citado en un "// asi se evita el X" sale como falso
// positivo. Un "http://" dentro de un literal trunca el literal (queda sin
// comilla de cierre y se descarta): para este test da igual.
QString sinComentarios(const QString &linea, bool &enBloque)
{
    QString out;
    for (int i = 0; i < linea.size();) {
        if (enBloque) {
            const int fin = linea.indexOf(QLatin1String("*/"), i);
            if (fin < 0)
                break;
            enBloque = false;
            i = fin + 2;
            continue;
        }
        const QChar c = linea.at(i);
        if (c == QLatin1Char('/') && i + 1 < linea.size()
            && linea.at(i + 1) == QLatin1Char('/'))
            break; // resto de la linea es comentario
        if (c == QLatin1Char('/') && i + 1 < linea.size()
            && linea.at(i + 1) == QLatin1Char('*')) {
            enBloque = true;
            i += 2;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// Literales de comillas dobles de una linea, con los escapes \", \\, \n, etc.
// ya resueltos. Los que llevan \u o \x los filtra el llamante: ahi el
// codepoint es explicito a proposito.
QStringList literalesDe(const QString &linea)
{
    QStringList out;
    for (int i = 0; i < linea.size(); ++i) {
        if (linea.at(i) != QLatin1Char('"'))
            continue;
        QString lit;
        int j = i + 1;
        bool cerrado = false;
        for (; j < linea.size(); ++j) {
            const QChar c = linea.at(j);
            if (c == QLatin1Char('\\') && j + 1 < linea.size()) {
                lit += c;
                lit += linea.at(j + 1);
                ++j;
                continue;
            }
            if (c == QLatin1Char('"')) {
                cerrado = true;
                break;
            }
            lit += c;
        }
        if (cerrado) {
            out << lit;
            i = j; // seguir despues de la comilla de cierre
        }
    }
    return out;
}

} // namespace

// Selftest OFFLINE de GLIFOS. La UI pinta su texto con la fuente por defecto
// del sistema, y esa fuente no tiene todos los caracteres: "✕" (U+2715),
// "◀" (U+25C0), "▾" (U+25BE) y "✔" (U+2714) NO estan en Liberation Sans, la
// que resuelve el sistema. Qt los sustituye por el marcador de glifo ausente,
// que medido en pantalla son 2x5 px: el boton de limpiar del buscador era un
// marco vacio con un punto, y los desplegables de Ajustes likewise.
//
// Para que no dependa de una lista que se desincroniza al anadir texto, se
// recorren los literales del propio ARBOL DE FUENTES y se comprueba cada
// caracter no ASCII contra la fuente real de la app: un glifo que no exista
// sale en el CI en vez de en la pantalla del usuario. Offline y determinista.
bool glyphSelfTest()
{
    int fail = 0;
    const auto chk = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };

    const QFont appFont = QGuiApplication::font();
    const QFontMetrics fm(appFont);
    std::fprintf(stderr, "GLYFTS fuente de la app: %s (%d px)\n",
                 qPrintable(appFont.family()), appFont.pixelSize());

    // OJO: QFontMetrics::inFont() NO sirve, consulta el FALLBACK del sistema y
    // devuelve true si CUALQUIER fuente instalada tiene el caracter (aqui hay 25
    // que cubren U+2715), mientras que la app segun pintando el marcador de
    // glifo ausente. Lo que decide es si lo tiene EL FICHERO de la fuente de la
    // app, y eso lo responde QRawFont. Si la fuente no se puede abrir (una
    // fuente compuesta del sistema, p. ej.), se cae a inFont y se avisa: es
    // mas permisivo, pero mejor que no comprobar nada.
    const QRawFont raw = QRawFont::fromFont(appFont);
    const bool usarRaw = raw.isValid();
    if (!usarRaw)
        std::fprintf(stderr,
                     "GLYFTS (aviso: no se pudo abrir el fichero de la fuente; "
                     "se usa inFont, que consulta el fallback)\n");
    const auto tieneGlifo = [&fm, &raw, usarRaw](QChar c) {
        return usarRaw ? raw.supportsCharacter(c.unicode()) : fm.inFont(c);
    };

    // Raiz del arbol: este fichero es <raiz>/src/ui/theme.cpp.
    QDir root = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
    root.cdUp();
    root.cdUp();
    const QString srcDir = root.filePath(QStringLiteral("src"));
    if (!QFileInfo(srcDir).isDir()) {
        std::fprintf(stderr,
                     "GLYFTS (sin arbol de fuentes junto al binario: omitido)\n");
        return true;
    }

    // char -> primer uso ("fichero:linea"), para el diagnostico.
    QHash<uint, QString> usos;
    int ficheros = 0;
    int literales = 0;
    QStringList pila{srcDir};
    bool enBloque = false;
    while (!pila.isEmpty()) {
        const QString dir = pila.takeLast();
        QFileInfoList entradas =
            QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                    QDir::Name);
        for (const QFileInfo &fi : entradas) {
            if (fi.isDir()) {
                pila << fi.filePath();
                continue;
            }
            const QString sufijo = fi.suffix();
            if (sufijo != QLatin1String("cpp") && sufijo != QLatin1String("h"))
                continue;
            QFile f(fi.filePath());
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            ++ficheros;
            // El estado de comentario en bloque NO puede pasar de un fichero a
            // otro: un "/*" sin cerrar al final dejaria el siguiente entero
            // "en bloque" y sus literales no se comprobarian (agujero
            // silencioso, que es justo lo que este test evita).
            enBloque = false;
            const QStringList lineas =
                QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            for (int ln = 0; ln < lineas.size(); ++ln) {
                const QString codigo = sinComentarios(lineas.at(ln), enBloque);
                const QStringList lits = literalesDe(codigo);
                literales += lits.size();
                for (const QString &lit : lits) {
                    if (lit.contains(QLatin1String("\\u"))
                        || lit.contains(QLatin1String("\\x")))
                        continue; // codepoint explicito: no se puede comprobar
                    for (const QChar &c : lit) {
                        if (c.unicode() < 128)
                            continue;
                        if (!usos.contains(c.unicode())) {
                            usos.insert(c.unicode(),
                                        QStringLiteral("%1:%2")
                                            .arg(fi.filePath())
                                            .arg(ln + 1));
                        }
                    }
                }
            }
        }
    }
    chk(ficheros > 20 && literales > 200,
        "se ha recorrido el arbol de fuentes (literales incluidos)");
    chk(!usos.isEmpty(), "hay caracteres no ASCII que comprobar");

    int ausentes = 0;
    QList<uint> claves = usos.keys();
    std::sort(claves.begin(), claves.end());
    for (uint cp : claves) {
        const QChar c = QChar(ushort(cp));
        if (tieneGlifo(c))
            continue;
        ++ausentes;
        std::fprintf(stderr, "  [FAIL] U+%04X %s  NO esta en %s (primer uso: %s)\n",
                     cp, qPrintable(QString(c)), qPrintable(appFont.family()),
                     qPrintable(usos.value(cp)));
    }
    chk(ausentes == 0, "todo glifo no ASCII de la UI existe en la fuente");

    std::fprintf(stderr, "GLYFTS %s (%d ficheros, %d literales, %d glifos)\n",
                 fail == 0 ? "OK" : "FAIL", ficheros, literales, (int)usos.size());
    return fail == 0;
}

} // namespace nt
