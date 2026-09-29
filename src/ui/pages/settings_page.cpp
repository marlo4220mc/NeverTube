#include "settings_page.h"

#include <QActionGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "../../db/repositories.h"
#include "../theme.h"

static const char *kQualityKey = "quality";
static const char *kQualityAutoKey = "quality_auto";
static const char *kNextModeKey = "next_mode";
static const char *kAudioLangKey = "audio_language";
static const char *kGridDensityKey = "grid_density";
static const char *kThumbSizeKey = "thumb_size";
static const char *kMiniVideoKey = "mini_video";
static const char *kAccentKey = "ui_accent";

static QStringList qualityOptions(bool noHw = false)
{
    if (noHw)
        return {QStringLiteral("Auto"), QStringLiteral("144p"),
                QStringLiteral("240p"), QStringLiteral("360p"),
                QStringLiteral("480p"), QStringLiteral("720p"),
                QStringLiteral("1080p")};
    return {QStringLiteral("Auto"), QStringLiteral("144p"),
            QStringLiteral("240p"), QStringLiteral("360p"),
            QStringLiteral("480p"), QStringLiteral("720p"),
            QStringLiteral("1080p"), QStringLiteral("1440p"),
            QStringLiteral("2160p")};
}

static QString valueFromQuality(const QString &text)
{
    if (text == QStringLiteral("Auto"))
        return QStringLiteral("auto");
    QString s = text;
    return s.remove(QLatin1Char('p'));
}

static QString qualityFromValue(const QString &val)
{
    if (val == QStringLiteral("auto"))
        return QStringLiteral("Auto");
    return val + QStringLiteral("p");
}

static QStringList nextModeOptions()
{
    return {QStringLiteral("Radio"), QStringLiteral("Sugerencias"),
            QStringLiteral("Desactivado")};
}

static QString valueFromNextMode(const QString &text)
{
    if (text == QStringLiteral("Desactivado"))
        return QStringLiteral("none");
    if (text == QStringLiteral("Sugerencias"))
        return QStringLiteral("suggestions");
    return QStringLiteral("radio");
}

static QString nextModeFromValue(const QString &val)
{
    if (val == QStringLiteral("none"))
        return QStringLiteral("Desactivado");
    if (val == QStringLiteral("suggestions"))
        return QStringLiteral("Sugerencias");
    return QStringLiteral("Radio");
}

static QStringList miniVideoOptions()
{
    return {QStringLiteral("Ver vídeo"), QStringLiteral("Solo miniatura")};
}

static QString valueFromMiniVideo(const QString &text)
{
    return text == QStringLiteral("Solo miniatura") ? QStringLiteral("thumb")
                                                    : QStringLiteral("video");
}

static QString miniVideoFromValue(const QString &val)
{
    return val == QStringLiteral("thumb") ? QStringLiteral("Solo miniatura")
                                          : QStringLiteral("Ver vídeo");
}

static QStringList audioLangOptions()
{
    return {QStringLiteral("Original"), QStringLiteral("Español"),
            QStringLiteral("Inglés"), QStringLiteral("Portugués")};
}

static QString valueFromAudioLang(const QString &text)
{
    if (text == QStringLiteral("Original"))
        return QStringLiteral("original");
    if (text == QStringLiteral("Español"))
        return QStringLiteral("es");
    if (text == QStringLiteral("Inglés"))
        return QStringLiteral("en");
    if (text == QStringLiteral("Portugués"))
        return QStringLiteral("pt");
    return QStringLiteral("original");
}

static QString audioLangFromValue(const QString &val)
{
    if (val == QLatin1String("es"))
        return QStringLiteral("Español");
    if (val == QLatin1String("en"))
        return QStringLiteral("Inglés");
    if (val == QLatin1String("pt"))
        return QStringLiteral("Portugués");
    return QStringLiteral("Original");
}

// Volumen máximo (mpv "volume-max"): 100% por defecto, 150/175/200 disponibles
// como "headroom" para quienes necesiten más volumen. Cada opción >100% pide
// confirmación por seguridad auditiva (riesgo de daño con audífonos).
static QStringList volumeMaxOptions()
{
    return {QStringLiteral("100%"), QStringLiteral("150%"),
            QStringLiteral("175%"), QStringLiteral("200%")};
}

static QString valueFromVolumeMax(const QString &text)
{
    QString s = text;
    s.remove(QLatin1Char('%'));
    return s;
}

static QString volumeMaxFromValue(const QString &val)
{
    return val + QStringLiteral("%");
}

// "Tamaño de las miniaturas". Tres niveles que se traducen a DOS efectos, porque
// las rejillas del Home y las vistas de lista no se dimensionan igual: las
// rejillas son dimensionadas por el ancho con un número de columnas FIJO (el
// factor no hace nada visible: con 5 columnas a 903 px la celda mide 175 px con
// densidad 1.0 y 175 px con 0.88), mientras que las listas escalan su fila con
// el factor. Por eso la palanca del Home son las COLUMNAS.
static QStringList thumbSizeOptions()
{
    return {QStringLiteral("Pequeñas"), QStringLiteral("Normales"),
            QStringLiteral("Grandes")};
}

static QString valueFromThumbSize(const QString &text)
{
    if (text == QStringLiteral("Pequeñas"))
        return QStringLiteral("small");
    if (text == QStringLiteral("Grandes"))
        return QStringLiteral("large");
    return QStringLiteral("normal");
}

static QString thumbSizeFromValue(const QString &val)
{
    if (val == QStringLiteral("small"))
        return QStringLiteral("Pequeñas");
    if (val == QStringLiteral("large"))
        return QStringLiteral("Grandes");
    return QStringLiteral("Normales");
}

double SettingsPage::thumbListScale(const QString &value)
{
    if (value == QStringLiteral("small"))
        return 0.88;
    if (value == QStringLiteral("large"))
        return 1.15;
    return 1.0;
}

int SettingsPage::thumbGridColumns(const QString &value)
{
    if (value == QStringLiteral("small"))
        return 6;
    if (value == QStringLiteral("large"))
        return 4;
    return 5;
}

// Color de acento de la app. La paleta (labels + hex) vive en la FUENTE ÚNICA
// nt::accentOptions() (src/ui/theme.h); aquí solo se traduce label<->hex
// contra ella y se persiste con kAccentKey. El defecto es el rojo NeverTube
// histórico (primer elemento de la paleta).
static QStringList accentOptions()
{
    QStringList out;
    for (const nt::AccentOption &o : nt::accentOptions())
        out << o.label;
    return out;
}

static QString valueFromAccent(const QString &text)
{
    for (const nt::AccentOption &o : nt::accentOptions())
        if (o.label == text)
            return o.hex;
    return nt::accentOptions().value(0).hex;
}

static QString accentFromValue(const QString &val)
{
    for (const nt::AccentOption &o : nt::accentOptions())
        if (o.hex == val)
            return o.label;
    return nt::accentOptions().value(0).label;
}

// Texto legible del perfil detectado por HwProbe (codec_profile).
static QString codecProfileText(const QString &val)
{
    if (val == QLatin1String("av1-hw"))
        return QStringLiteral("AV1 por hardware (óptima)");
    if (val == QLatin1String("vp9-hw"))
        return QStringLiteral("VP9 por hardware");
    if (val == QLatin1String("h264-hw"))
        return QStringLiteral("H.264 por hardware");
    if (val == QLatin1String("soft-all") || val == QLatin1String("nohw"))
        return QStringLiteral("Sin aceleración (CPU)");
    return QStringLiteral("Sin detectar");
}

SettingsPage::SettingsPage(SettingsRepo *settings, QWidget *parent)
    : QWidget(parent), m_settings(settings)
{
    buildUI();
}

void SettingsPage::updateCodecProfile(const QString &profile)
{
    if (m_codecProfileLabel)
        m_codecProfileLabel->setText(codecProfileText(profile));
}

void SettingsPage::applyAccent()
{
    // El boton de restablecer es #accentOutline en el QSS global, y su color
    // sale de @ACCENT@: nt::styleSheet() se regenera entero en cada cambio de
    // acento, asi que ya no hay nada que re-aplicar aqui. El metodo se mantiene
    // porque MainWindow lo llama en las cuatro paginas por igual.
}

QPushButton *SettingsPage::makeActionButton(const QString &text)
{
    auto *btn = new QPushButton(text, this);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setObjectName(QStringLiteral("actionBtn"));
    btn->setMinimumWidth(150);
    return btn;
}

QPushButton *SettingsPage::makeValueButton(const QString &text)
{
    return makeActionButton(text + QStringLiteral("  ▾"));
}

// Construye un menú de selección asociado al botón `btn`. La callback `onPick`
// recibe la opción elegida y se ejecuta SÍNCRONAMENTE: si retorna, se considera
// el cambio aceptado y el botón pasa a mostrar la opción nueva. Para opciones
// que necesitan confirmación ASÍNCRONA (modal no modal), usar
// setupValueMenuWithConfirm() en su lugar.
void SettingsPage::setupValueMenu(
    QPushButton *btn, const QStringList &options, const QString &current,
    const std::function<void(const QString &)> &onPick)
{
    auto *menu = new QMenu(btn);
    // Grupo exclusivo: solo una QAction puede quedar marcada POR MARCO. El
    // toggle automático de Qt en acciones checkable sueltas marcaba la nueva
    // sin desmarcar la anterior (doble check); con el grupo, cliquear una
    // acción desmarca el resto de forma atómica.
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    // currentOpt: opción vigente (texto exacto de una QAction del menú). Vive
    // en un shared_ptr porque los triggers se ejecutan mucho después de que
    // esta función retorne (la pila ya no existe).
    auto currentOpt = std::make_shared<QString>(current);
    auto syncMenu = [menu, btn](const QString &opt) {
        for (QAction *a : menu->actions())
            a->setChecked(a->text() == opt);
        btn->setText(opt + QStringLiteral("  ▾"));
    };
    // Al abrir el menú re-sincroniza checks y texto del botón con la opción
    // vigente; sin esto, Qt deja el check de la última acción marcada y el
    // menú reabre con checks viejos ("doble check").
    connect(btn, &QPushButton::clicked, btn,
            [btn, menu, currentOpt, syncMenu]() {
                syncMenu(*currentOpt);
                menu->popup(btn->mapToGlobal(QPoint(0, btn->height())));
            });
    for (const QString &opt : options) {
        QAction *act = menu->addAction(opt);
        act->setCheckable(true);
        group->addAction(act);
        connect(act, &QAction::triggered, btn,
                [btn, opt, onPick, currentOpt, syncMenu]() {
                    onPick(opt);
                    *currentOpt = opt;
                    syncMenu(opt);
                });
    }
    syncMenu(*currentOpt);
}

// Variante con confirmación ASÍNCRONA (modal no modal). `confirm(opt,
// applyVisual)` decide por cada opción si aplica directo o muestra el modal:
//   - Si aplica directo (p. ej. volver a 100%): hace los cambios reales
//     (BD + signal) y llama applyVisual(opt) para reflejar botón + checks.
//   - Si muestra modal: al Aceptar aplica los cambios y llama applyVisual(opt);
//     al Cancelar NO llama applyVisual — el botón y el menú conservan la
//     opción previa.
// applyVisual es un wrapper que además registra la opción aceptada como la
// vigente, para que la próxima apertura del menú la muestre marcada.
void SettingsPage::setupValueMenuWithConfirm(
    QPushButton *btn, const QStringList &options, const QString &current,
    const std::function<void(const QString &,
                              const std::function<void(const QString &)> &)>
        &confirm)
{
    auto *menu = new QMenu(btn);
    // Grupo exclusivo: solo una QAction puede quedar marcada POR MARCO (ver
    // comentario en setupValueMenu). Con el grupo, al cliquear una opción el
    // check anterior se desmarca de forma atómica.
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    // currentOpt: opción visualmente vigente (texto exacto de una QAction).
    auto currentOpt = std::make_shared<QString>(current);
    auto syncMenu = [menu, btn](const QString &opt) {
        for (QAction *a : menu->actions())
            a->setChecked(a->text() == opt);
        btn->setText(opt + QStringLiteral("  ▾"));
    };
    // applyVisual que recibe el caller: registra la opción aceptada y la
    // refleja. Al cancelar el modal el caller NO lo invoca, así currentOpt
    // queda en la opción previa.
    auto applyAndTrack = [currentOpt, syncMenu](const QString &opt) {
        *currentOpt = opt;
        syncMenu(opt);
    };
    // Antes de abrir, sincroniza el check con la opción vigente (puede haber
    // cambiado tras un aceptar/cancelar anterior).
    connect(btn, &QPushButton::clicked, btn,
            [btn, menu, currentOpt, syncMenu]() {
                syncMenu(*currentOpt);
                menu->popup(btn->mapToGlobal(QPoint(0, btn->height())));
            });
    for (const QString &opt : options) {
        QAction *act = menu->addAction(opt);
        act->setCheckable(true);
        group->addAction(act);
        connect(act, &QAction::triggered, btn,
                [btn, opt, confirm, applyAndTrack]() {
                    confirm(opt, applyAndTrack);
                });
    }
    syncMenu(*currentOpt);
}

void SettingsPage::buildUI()
{
    auto *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(20, 20, 20, 20);
    mainLay->setSpacing(20);

    auto *title = new QLabel(QStringLiteral("Ajustes"), this);
    // A proposito NO es #sectionTitle (16 px): este es el titulo de la pagina
    // entera, un escalon por encima del titulo de seccion.
    title->setObjectName(QStringLiteral("pageHeading"));
    mainLay->addWidget(title);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("settingsScroll"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto *content = new QWidget(scrollArea);
    auto *scrollLay = new QVBoxLayout(content);
    scrollLay->setContentsMargins(0, 0, 0, 12);
    scrollLay->setSpacing(24);
    scrollArea->setWidget(content);
    mainLay->addWidget(scrollArea, 1);

    // Cada sección es un grupo "título + tarjeta": las filas van dentro de la
    // tarjeta (#settingsCard), no sueltas en scrollLay. cardLay apunta a la
    // tarjeta de la sección vigente y lo consumen los helpers de fila
    // (addOptionRow, addActionButton y los bloques en línea).
    QVBoxLayout *cardLay = nullptr;

    // Encabezado de sección de Ajustes (INTERFAZ, REPRODUCCION, DATOS...). A
    // propósito NO es #sectionSub: ese es el subtitulo de 12 px de las demas
    // paginas, y aqui son 11 px en versalitas con algo de tracking.
    auto addSectionTitle = [scrollLay, &cardLay](const QString &t) {
        auto *group = new QWidget;
        auto *gv = new QVBoxLayout(group);
        gv->setContentsMargins(0, 0, 0, 0);
        gv->setSpacing(6);
        auto *subTitle = new QLabel(t);
        subTitle->setObjectName(QStringLiteral("settingsSectionTitle"));
        gv->addWidget(subTitle);
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("settingsCard"));
        auto *cv = new QVBoxLayout(card);
        cv->setContentsMargins(14, 4, 14, 4);
        cv->setSpacing(0);
        gv->addWidget(card);
        scrollLay->addWidget(group);
        cardLay = cv;
    };

    // Nota aclaratoria bajo un ajuste: siempre se agrupa con su fila en un
    // bloque de espaciado corto (nunca suelta en scrollLay, que separa con 24px).
    auto addNote = [content](const QString &text) {
        auto *note = new QLabel(text, content);
        note->setObjectName(QStringLiteral("settingsNote"));
        note->setWordWrap(true);
        return note;
    };

    auto addOptionRow = [this, &cardLay, addNote](
                            const QString &labelText,
                            const QStringList &options,
                            const QString &saved,
                            const QString &defaultVal,
                            const std::function<QString(const QString &)> &fromVal,
                            const std::function<void(const QString &)> &onPick,
                            const QString &note = QString()) {
        // Bloque "fila + nota": la nota queda pegada a su ajuste (spacing 4);
        // el spacing de 24 de scrollLay separa bloques completos, no fila-nota.
        auto *block = new QVBoxLayout;
        block->setSpacing(4);
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *lbl = new QLabel(labelText);
        lbl->setObjectName(QStringLiteral("settingsRowLabel"));
        row->addWidget(lbl);
        QPushButton *btn = makeValueButton(fromVal(saved));
        row->addWidget(btn, 1, Qt::AlignRight);
        block->addLayout(row);
        if (!note.isEmpty())
            block->addWidget(addNote(note));
        cardLay->addLayout(block);
        setupValueMenu(btn, options, fromVal(saved), onPick);
        Q_UNUSED(defaultVal);
    };

    // --- REPRODUCCIÓN ---
    addSectionTitle(QStringLiteral("REPRODUCCIÓN"));

    // Calidad de reproducción: tope de resolución que la app pide a YouTube.
    // Auto usa el último valor efectivo guardado (por defecto 360p) y la
    // propia reproducción lo re-memoriza sin cambiar en caliente.
    addOptionRow(
        QStringLiteral("Calidad de reproducción"),
        qualityOptions(m_settings->getString(
                           QStringLiteral("codec_profile"),
                           QStringLiteral("")) == QLatin1String("nohw")
                       || m_settings->getString(
                              QStringLiteral("codec_profile"),
                              QStringLiteral("")) == QLatin1String("soft-all")),
        m_settings->getString(QStringLiteral("quality"),
                              QStringLiteral("auto")),
        QStringLiteral("auto"), qualityFromValue,
        [this](const QString &text) {
            const QString val = valueFromQuality(text);
            m_settings->set(QStringLiteral("quality"), val);
            emit qualityChanged(val);
            if (val == QStringLiteral("auto")) {
                int autoDef = m_settings->getInt(
                    QStringLiteral("quality_auto"), 360);
                m_settings->set(QStringLiteral("quality_auto"),
                                QString::number(autoDef));
            }
        });

    // Modo "A continuación": qué se siembra en la cola al reproducir un vídeo.
    // Radio: mezcla tipo radio de la app (RAMDV). Sugerencias: lista de
    // recomendaciones del vídeo. Desactivado: la cola queda como vino.
    addOptionRow(
        QStringLiteral("Modo \"A continuación\""), nextModeOptions(),
        m_settings->getString(QStringLiteral("next_mode"),
                              QStringLiteral("radio")),
        QStringLiteral("radio"), nextModeFromValue,
        [this](const QString &text) {
            const QString val = valueFromNextMode(text);
            m_settings->set(QStringLiteral("next_mode"), val);
            emit nextModeChanged(val);
        });

    // Vídeo en el mini player: si el vídeo en vivo ocupa el hueco del mini bar
    // cuando se sale de la página Player, o si se prefiere ver solo la
    // miniatura estática (en ambos casos el vídeo sigue renderizando en
    // segundo plano para que no haya salto al volver).
    addOptionRow(
        QStringLiteral("Vídeo en mini player"), miniVideoOptions(),
        m_settings->getString(QStringLiteral("mini_video"),
                              QStringLiteral("thumb")),
        QStringLiteral("thumb"), miniVideoFromValue,
        [this](const QString &text) {
            const QString val = valueFromMiniVideo(text);
            m_settings->set(QStringLiteral("mini_video"), val);
            emit miniVideoModeChanged(val);
        });

    // Aceleración de vídeo: resultado del sondeo offline (HwProbe). Solo
    // informativo; "Volver a detectar" borra el perfil y relanza el sondeo al
    // momento (MainWindow::redetectHwRequested).
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *lbl = new QLabel(QStringLiteral("Aceleración de vídeo"));
        lbl->setObjectName(QStringLiteral("settingsRowLabel"));
        row->addWidget(lbl);
        m_codecProfileLabel = new QLabel(codecProfileText(
            m_settings->getString(QStringLiteral("codec_profile"),
                                  QString())));
        m_codecProfileLabel->setObjectName(QStringLiteral("settingsRowValue"));
        row->addWidget(m_codecProfileLabel);
        row->addStretch(1);
#ifndef NEVERTUBE_NO_MPV
        auto *btn = makeActionButton(QStringLiteral("Volver a detectar"));
        connect(btn, &QPushButton::clicked, this, [this]() {
            m_settings->set(QStringLiteral("codec_profile"), QString());
            updateCodecProfile(QString());
            emit redetectHwRequested();
        });
        row->addWidget(btn);
#endif
        cardLay->addLayout(row);
    }

    // Idioma de audio: pista preferida cuando el vídeo la ofrece (incluye
    // doblajes automáticos/IA si vienen con LANGUAGE).
    addOptionRow(
        QStringLiteral("Idioma de audio"), audioLangOptions(),
        m_settings->getString(QStringLiteral("audio_language"),
                              QStringLiteral("original")),
        QStringLiteral("original"), audioLangFromValue,
        [this](const QString &text) {
            const QString val = valueFromAudioLang(text);
            m_settings->set(QStringLiteral("audio_language"), val);
            emit audioLanguageChanged(val);
        },
        QStringLiteral(
            "Nota: la preferencia se aplica solo cuando el vídeo tiene una "
            "pista en ese idioma. Si no existe, se usará el audio original."));

    // Volumen máximo (mpv "volume-max"): permite headroom sobre el 100%.
    // Cualquier opción por encima del 100% pide confirmación antes de aplicar:
    // a niveles altos el audio puede dañar audífonos/oidos. Para evitar
    // sorpresas, el volumen persistido se CAPA a 100% — al cerrar la app
    // siempre arranca en 100% aunque la sesión anterior hubiera subido más.
    // Si el usuario CANCELA el modal, el botón sigue mostrando la opción
    // previa (setupValueMenuWithConfirm no aplica visualmente al cancelar).
    {
        auto *block = new QVBoxLayout;
        block->setSpacing(4);
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *lbl = new QLabel(QStringLiteral("Volumen máximo"));
        lbl->setObjectName(QStringLiteral("settingsRowLabel"));
        row->addWidget(lbl);
        const QString saved = m_settings->getString(
            QStringLiteral("volume_max"), QStringLiteral("100"));
        QPushButton *btn = makeValueButton(volumeMaxFromValue(saved));
        row->addWidget(btn, 1, Qt::AlignRight);
        block->addLayout(row);
        // `current` es el TEXTO del menú ("150%"), no el valor crudo ("150"):
        // los checks y el texto del botón comparan contra el texto de las
        // QAction (que incluye el %).
        setupValueMenuWithConfirm(
            btn, volumeMaxOptions(), volumeMaxFromValue(saved),
            [this](const QString &text,
                   const std::function<void(const QString &)> &applyVisual) {
                const QString val = valueFromVolumeMax(text);
                if (val == QStringLiteral("100")) {
                    // Sin modal: aplica directo.
                    m_settings->set(QStringLiteral("volume_max"), val);
                    emit volumeMaxChanged(val);
                    applyVisual(text);
                    return;
                }
                // Con modal: aplicamos SOLO al Aceptar.
                const QString confirmText = QStringLiteral(
                    "Subir el volumen máximo a %1 puede causar daño auditivo "
                    "permanente. Úsalo solo con altavoces externos y a un "
                    "nivel cómodo. Al cerrar la app el volumen volverá a 100% "
                    "aunque lo subas más alto. ¿Continuar?").arg(val);
                showConfirmPopup(
                    QStringLiteral("Volumen por encima del 100%"),
                    confirmText, QStringLiteral("Aceptar"),
                    [this, val, text, applyVisual]() {
                        m_settings->set(QStringLiteral("volume_max"), val);
                        emit volumeMaxChanged(val);
                        applyVisual(text);
                    });
                // Al Cancelar: no se llama applyVisual, no se toca BD, no se
                // emite signal. El botón y el menú conservan la opción previa.
            });
        // Nota pegada al bloque (antes colgaba suelta con el spacing de 24).
        block->addWidget(addNote(QStringLiteral(
            "Por seguridad, el volumen solo se guarda hasta el 100%. Al "
            "reiniciar la app, aunque el ajuste siga por encima, el "
            "reproductor empezará al 100%.")));
        cardLay->addLayout(block);
    }

    scrollLay->addStretch(1);

    // --- INTERFAZ ---
    addSectionTitle(QStringLiteral("INTERFAZ"));

    // Color de acento: tono de los elementos interactivos (sliders, barra de
    // progreso, botón primario, selección, nav activa, iconos activos de la
    // barra inferior). Se aplica en caliente (MainWindow re-genera el
    // stylesheet global y re-tiñe los iconos de la nav al recibir
    // accentChanged).
    addOptionRow(
        QStringLiteral("Color de acento"), accentOptions(),
        m_settings->getString(QLatin1String(kAccentKey), nt::accentHex()),
        nt::accentHex(), accentFromValue,
        [this](const QString &text) {
            const QString hex = valueFromAccent(text);
            m_settings->set(QLatin1String(kAccentKey), hex);
            emit accentChanged(hex);
        },
        QStringLiteral("Se aplica al instante: sliders, progreso, botones "
                       "resaltados, selección de texto e iconos activos."));

    // Tamaño de las miniaturas: Pequeñas (6 por fila, filas cortas), Normales
    // (5 por fila) o Grandes (4 por fila, filas altas). Es el ajuste que el
    // usuario percibe de verdad, y el que SÍ mueve la rejilla del Home.
    //
    // Migración: el ajuste anterior se llamaba "Densidad de cuadrícula"
    // ("cozy"/"compact") y solo escalaba un factor, que en las rejillas del
    // Home no producía ningún cambio visible (la celda es 175 px con y sin
    // factor). Se lee su valor la primera vez para no perder la elección:
    // "compact" → "Pequeñas", "cozy" → "Normales". La clave vieja se deja en
    // la BD (inofensiva) y se reescribe sola al elegir una opción.
    m_thumbSize = m_settings->getString(
        QString::fromLatin1(kThumbSizeKey), QString());
    if (m_thumbSize.isEmpty()) {
        const QString old =
            m_settings->getString(QString::fromLatin1(kGridDensityKey),
                                  QStringLiteral("cozy"));
        m_thumbSize = (old == QStringLiteral("compact")) ? QStringLiteral("small")
                                                        : QStringLiteral("normal");
    }
    addOptionRow(
        QStringLiteral("Tamaño de las miniaturas"), thumbSizeOptions(),
        thumbSizeFromValue(m_thumbSize), QStringLiteral("Normales"),
        thumbSizeFromValue,
        [this](const QString &text) {
            const QString val = valueFromThumbSize(text);
            m_thumbSize = val;
            m_settings->set(QString::fromLatin1(kThumbSizeKey), val);
            emit thumbSizeChanged(val);
        },
        QStringLiteral("Pequeñas: 6 vídeos por fila en el Inicio y filas "
                       "cortas en búsqueda, historial y cola. Grandes: 4 por "
                       "fila y miniatura más grande."));

    scrollLay->addStretch(3);

    // --- DATOS ---
    addSectionTitle(QStringLiteral("DATOS"));

    // Los botones de la sección DATOS. Hay dos: el normal (#actionBtn) y el de
    // restablecer (#accentOutline, con borde y color de acento). El acento sale
    // del QSS global, asi que ya no hace falta re-aplicarlo al cambiarlo.
    auto addActionButton = [&cardLay](const QString &text, bool conAcento) {
        auto *btn = new QPushButton(text);
        btn->setObjectName(conAcento ? QStringLiteral("accentOutline")
                                     : QStringLiteral("dataActionBtn"));
        btn->setCursor(Qt::PointingHandCursor);
        cardLay->addWidget(btn);
        cardLay->addSpacing(4);
        return btn;
    };

    // Limpiar caché de miniaturas: borra solo las imágenes guardadas en disco
    // (se vuelven a descargar solas al mostrar vídeos). No toca datos ni la BD.
    QPushButton *clearBtn = addActionButton(
        QStringLiteral("Limpiar caché de miniaturas"), false);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        showConfirmPopup(
            QStringLiteral("Limpiar caché de miniaturas"),
            QStringLiteral("¿Eliminar todas las miniaturas guardadas? "
                           "Las imágenes se volverán a descargar."),
            QStringLiteral("Eliminar"), [this]() { emit clearThumbsRequested(); });
    });

    // Restablecer aplicación: borra la BD local (historial, búsquedas,
    // suscripciones y ajustes) y el caché, y reinicia la app con defaults.
    // Nunca toca archivos personales ni vídeos descargados.
    QPushButton *resetBtn = addActionButton(
        QStringLiteral("Restablecer aplicación"), true);
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        showConfirmPopup(
            QStringLiteral("Restablecer aplicación"),
            QStringLiteral("Esta acción borrará la base de datos local "
                           "(historial, búsquedas, suscripciones y ajustes) "
                           "y el caché de miniaturas. No se eliminarán "
                           "archivos personales ni vídeos descargados. "
                           "La aplicación se reiniciará con los valores por "
                           "defecto. ¿Continuar?"),
            QStringLiteral("Restablecer"), [this]() { emit resetRequested(); });
    });

    // --- ACERCA DE ---
    addSectionTitle(QStringLiteral("ACERCA DE"));

    auto *verLabel = new QLabel(QStringLiteral("Versión 1.0 (nevertube_qt2)"),
                                content);
    verLabel->setObjectName(QStringLiteral("settingsVersion"));

    auto *aboutBlock = new QVBoxLayout;
    aboutBlock->setSpacing(4);
    aboutBlock->addWidget(verLabel);
    aboutBlock->addWidget(addNote(
        QStringLiteral("Sin telemetría: esta aplicación no envía datos de "
                       "uso, búsquedas ni historial a ningún servidor. "
                       "Todo permanece local en tu dispositivo.")));
    cardLay->addLayout(aboutBlock);

    // --- Popup flotante de confirmación (no modal) ---
    m_confirmPopup = new QFrame(this);
    m_confirmPopup->setObjectName(QStringLiteral("popupDialog"));
    m_confirmPopup->setFixedWidth(380);
    m_confirmPopup->raise();
    m_confirmPopup->hide();

    auto *pv = new QVBoxLayout(m_confirmPopup);
    pv->setContentsMargins(18, 16, 18, 14);
    pv->setSpacing(10);

    m_confirmTitle = new QLabel(QString(), m_confirmPopup);
    m_confirmTitle->setObjectName(QStringLiteral("popupTitle"));
    pv->addWidget(m_confirmTitle);

    m_confirmText = new QLabel(QString(), m_confirmPopup);
    m_confirmText->setObjectName(QStringLiteral("popupText"));
    m_confirmText->setWordWrap(true);
    pv->addWidget(m_confirmText);

    auto *prow = new QHBoxLayout;
    prow->addStretch(1);
    m_confirmCancel = new QPushButton(QStringLiteral("Cancelar"), m_confirmPopup);
    m_confirmCancel->setObjectName(QStringLiteral("ghost"));
    m_confirmCancel->setCursor(Qt::PointingHandCursor);
    connect(m_confirmCancel, &QPushButton::clicked, this,
            &SettingsPage::closeConfirmPopup);
    prow->addWidget(m_confirmCancel);
    m_confirmOk = new QPushButton(QString(), m_confirmPopup);
    m_confirmOk->setObjectName(QStringLiteral("primary"));
    m_confirmOk->setCursor(Qt::PointingHandCursor);
    connect(m_confirmOk, &QPushButton::clicked, this, [this]() {
        std::function<void()> cb = m_confirmCb;
        closeConfirmPopup();
        if (cb)
            cb();
    });
    prow->addWidget(m_confirmOk);
    pv->addLayout(prow);
}

void SettingsPage::showConfirmPopup(const QString &title, const QString &text,
                                    const QString &acceptText,
                                    std::function<void()> onConfirm)
{
    if (!m_confirmPopup)
        return;
    m_confirmTitle->setText(title);
    m_confirmText->setText(text);
    m_confirmOk->setText(acceptText);
    m_confirmCb = std::move(onConfirm);
    centerPopup();
    m_confirmPopup->raise();
    m_confirmPopup->show();
    m_confirmOk->setFocus(Qt::OtherFocusReason);
}

void SettingsPage::closeConfirmPopup()
{
    if (m_confirmPopup)
        m_confirmPopup->hide();
}

void SettingsPage::centerPopup()
{
    if (!m_confirmPopup)
        return;
    m_confirmPopup->adjustSize();
    const int x = (width() - m_confirmPopup->width()) / 2;
    const int y = (height() - m_confirmPopup->height()) / 2;
    m_confirmPopup->move(qMax(0, x), qMax(0, y));
}

void SettingsPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_confirmPopup && m_confirmPopup->isVisible())
        centerPopup();
}