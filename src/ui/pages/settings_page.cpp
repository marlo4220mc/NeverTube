#include "settings_page.h"

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

static const char *kQualityKey = "quality";
static const char *kQualityAutoKey = "quality_auto";
static const char *kNextModeKey = "next_mode";
static const char *kAudioLangKey = "audio_language";
static const char *kGridDensityKey = "grid_density";
static const char *kMiniVideoKey = "mini_video";

static QStringList qualityOptions(bool noHw = false)
{
    if (noHw)
        return {QStringLiteral("Auto"), QStringLiteral("360p"),
                QStringLiteral("480p"), QStringLiteral("720p"),
                QStringLiteral("1080p")};
    return {QStringLiteral("Auto"), QStringLiteral("360p"),
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
    if (val == QStringLiteral("es"))
        return QStringLiteral("Español");
    if (val == QStringLiteral("en"))
        return QStringLiteral("Inglés");
    if (val == QStringLiteral("pt"))
        return QStringLiteral("Portugués");
    return QStringLiteral("Original");
}

static QStringList gridDensityOptions()
{
    return {QStringLiteral("Cómoda"), QStringLiteral("Compacta")};
}

static QString valueFromGridDensity(const QString &text)
{
    if (text == QStringLiteral("Compacta"))
        return QStringLiteral("compact");
    return QStringLiteral("cozy");
}

static QString gridDensityFromValue(const QString &val)
{
    return val == QStringLiteral("compact") ? QStringLiteral("Compacta")
                                          : QStringLiteral("Cómoda");
}

SettingsPage::SettingsPage(SettingsRepo *settings, QWidget *parent)
    : QWidget(parent), m_settings(settings)
{
    buildUI();
}

QPushButton *SettingsPage::makeValueButton(const QString &text)
{
    auto *btn = new QPushButton(text + QStringLiteral("  ▾"), this);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(
        QStringLiteral("font-size:12px; padding:7px 14px;"
                       " background-color:#1F1F1F;"
                       " border:1px solid #2A2A2A;"
                       " border-radius:8px; color:#ECECEC;"));
    btn->setMinimumWidth(150);
    return btn;
}

void SettingsPage::setupValueMenu(
    QPushButton *btn, const QStringList &options, const QString &current,
    const std::function<void(const QString &)> &onPick)
{
    auto *menu = new QMenu(btn);
    for (const QString &opt : options) {
        QAction *act = menu->addAction(opt);
        act->setCheckable(true);
        act->setChecked(opt == current);
        connect(act, &QAction::triggered, btn,
                [btn, menu, opt, onPick]() {
                    for (QAction *a : menu->actions())
                        a->setChecked(a->text() == opt);
                    btn->setText(opt + QStringLiteral("  ▾"));
                    onPick(opt);
                });
    }
    // Sin WA_DeleteOnClose: el menú queda vivo con el botón (como en los 3
    // puntos de biblioteca); borrarlo al cerrar dejaría un puntero colgante
    // en la lambda de "clicked" y la app crashearía al reabrir el cuadro.
    connect(btn, &QPushButton::clicked, btn,
            [btn, menu]() {
                menu->popup(btn->mapToGlobal(QPoint(0, btn->height())));
            });
}

void SettingsPage::buildUI()
{
    auto *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(20, 20, 20, 20);
    mainLay->setSpacing(20);

    auto *title = new QLabel(QStringLiteral("Ajustes"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    title->setStyleSheet(QStringLiteral("font-size:18px; font-weight:700;"));
    mainLay->addWidget(title);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *content = new QWidget(scrollArea);
    auto *scrollLay = new QVBoxLayout(content);
    scrollLay->setContentsMargins(0, 0, 0, 12);
    scrollLay->setSpacing(24);
    scrollArea->setWidget(content);
    mainLay->addWidget(scrollArea, 1);

    auto addSectionTitle = [scrollLay](const QString &t) {
        auto *subTitle = new QLabel(t);
        subTitle->setObjectName(QStringLiteral("sectionSub"));
        subTitle->setStyleSheet(
            QStringLiteral("font-size:11px; font-weight:600; color:#8F8F8F;"
                           " letter-spacing:0.5px;"));
        scrollLay->addWidget(subTitle);
    };

    auto addOptionRow = [this, scrollLay](
                            const QString &labelText,
                            const QStringList &options,
                            const QString &saved,
                            const QString &defaultVal,
                            const std::function<QString(const QString &)> &fromVal,
                            const std::function<void(const QString &)> &onPick) {
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *lbl = new QLabel(labelText);
        lbl->setStyleSheet(QStringLiteral("color:#ECECEC; min-width:200px;"));
        row->addWidget(lbl);
        QPushButton *btn = makeValueButton(fromVal(saved));
        row->addWidget(btn, 1, Qt::AlignRight);
        scrollLay->addLayout(row);
        // Actualizar con la opción persistida
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
                           QStringLiteral("")) == QLatin1String("nohw")),
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
        });

    auto *noteAudio = new QLabel(
        QStringLiteral(
            "Nota: la preferencia se aplica solo cuando el vídeo tiene una "
            "pista en ese idioma. Si no existe, se usará el audio original."),
        content);
    noteAudio->setStyleSheet(
        QStringLiteral("font-size:10px; color:#8F8F8F; padding-top:2px;"));
    noteAudio->setWordWrap(true);
    scrollLay->addWidget(noteAudio);

    scrollLay->addStretch(1);

    // --- DATOS ---
    addSectionTitle(QStringLiteral("DATOS"));

    auto addActionButton = [scrollLay](const QString &text,
                                       const QString &extraStyle) {
        auto *btn = new QPushButton(text);
        btn->setStyleSheet(
            QStringLiteral("font-size:12px; padding:8px 16px;"
                           " background-color:#1F1F1F;"
                           " border:1px solid #2A2A2A;"
                           " border-radius:8px; color:#ECECEC; ") +
            extraStyle);
        btn->setCursor(Qt::PointingHandCursor);
        scrollLay->addWidget(btn);
        scrollLay->addSpacing(4);
        return btn;
    };

    // Limpiar caché de miniaturas: borra solo las imágenes guardadas en disco
    // (se vuelven a descargar solas al mostrar vídeos). No toca datos ni la BD.
    QPushButton *clearBtn = addActionButton(
        QStringLiteral("Limpiar caché de miniaturas"), QString());
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
        QStringLiteral("Restablecer aplicación"),
        QStringLiteral("border:1px solid #FF0033; color:#FF0033;"
                       " font-weight:600;"));
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

    scrollLay->addStretch(1);

    // --- INTERFAZ ---
    addSectionTitle(QStringLiteral("INTERFAZ"));

    // Densidad de cuadrícula: tamaño de las celdas de vídeo. Cómoda (normal)
    // o Compacta (celdas algo más pequeñas para ver más en pantalla).
    addOptionRow(
        QStringLiteral("Densidad de cuadrícula"), gridDensityOptions(),
        m_settings->getString(QStringLiteral("grid_density"),
                              QStringLiteral("cozy")),
        QStringLiteral("cozy"), gridDensityFromValue,
        [this](const QString &text) {
            const QString val = valueFromGridDensity(text);
            m_settings->set(QStringLiteral("grid_density"), val);
            emit gridDensityChanged(val);
        });

    scrollLay->addStretch(3);

    // --- ACERCA DE ---
    addSectionTitle(QStringLiteral("ACERCA DE"));

    auto *verLabel = new QLabel(QStringLiteral("Versión 1.0 (nevertube_qt2)"),
                                content);
    verLabel->setStyleSheet(QStringLiteral("font-size:12px; color:#ECECEC;"));
    scrollLay->addWidget(verLabel);

    auto *dbLabel = new QLabel(
        QStringLiteral("Ruta de datos: ~/Documents/mini_tube/mini_tube.db"),
        content);
    dbLabel->setStyleSheet(QStringLiteral("font-size:11px; color:#8F8F8F;"));
    scrollLay->addWidget(dbLabel);

    auto *telemetryNote = new QLabel(
        QStringLiteral("Sin telemetría: esta aplicación no envía datos de "
                       "uso, búsquedas ni historial a ningún servidor. "
                       "Todo permanece local en tu dispositivo."),
        content);
    telemetryNote->setStyleSheet(
        QStringLiteral("font-size:11px; color:#8F8F8F; padding-top:4px;"));
    telemetryNote->setWordWrap(true);
    scrollLay->addWidget(telemetryNote);

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
    m_confirmText->setStyleSheet(QStringLiteral("color:#8F8F8F;"));
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