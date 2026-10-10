#include "settings_page.h"

#include <QActionGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "../../db/repositories.h"
#include "../confirm_dialog.h"
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

// Maximum volume (mpv "volume-max"): 100% by default, 150/175/200 available
// as "headroom" for those who need more volume. Each option above 100% asks
// for confirmation for hearing safety (risk of damage with headphones).
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

// "Tamaño de las miniaturas". Three levels that translate into TWO effects, because
// the Home grids and the list views are not sized the same way: the
// grids are width-sized with a FIXED number of columns (the
// factor does nothing visible: with 5 columns at 903 px the cell measures 175 px with
// density 1.0 and 175 px with 0.88), while the lists scale their row with
// the factor. That is why the Home lever is the COLUMNS.
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

// App accent color. The palette (labels + hex) lives in the SINGLE SOURCE
// nt::accentOptions() (src/ui/theme.h); here only label<->hex is translated
// against it and persisted with kAccentKey. The default is the historic NeverTube
// red (first element of the palette).
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

// Readable text of the profile detected by HwProbe (codec_profile).
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
    // The reset button is #accentOutline in the global QSS, and its color
    // comes from @ACCENT@: nt::styleSheet() is regenerated whole on every accent
    // change, so there is nothing left to re-apply here. The method is kept
    // because MainWindow calls it on the four pages alike.
}

QPushButton *SettingsPage::makeActionButton(const QString &text)
{
    auto *btn = new QPushButton(text, this);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setObjectName(QStringLiteral("actionBtn"));
    btn->setMinimumWidth(150);
    return btn;
}

// VALUE button: opens a QMenu with the alternatives. Deliberately shaped
// just like an action button: no triangle, no text glyph ("  ▼", the
// one put on it at first) and no drawn icon. The user asked to remove it and
// it is redundant decoration: the button is already a control with its own box and the
// menu marks the current option with a check. Do NOT re-add any indicator.
QPushButton *SettingsPage::makeValueButton(const QString &text)
{
    return makeActionButton(text);
}

// Builds a selection menu attached to button `btn`. The `onPick` callback
// receives the chosen option and runs SYNCHRONOUSLY: if it returns, the change is
// considered accepted and the button then shows the new option. For options
// needing ASYNCHRONOUS confirmation (non-modal dialog), use
// setupValueMenuWithConfirm() instead.
void SettingsPage::setupValueMenu(
    QPushButton *btn, const QStringList &options, const QString &current,
    const std::function<void(const QString &)> &onPick)
{
    auto *menu = new QMenu(btn);
    // Exclusive group: only one QAction can stay checked BY FRAMEWORK. Qt's
    // automatic toggle on loose checkable actions marked the new one
    // without unchecking the previous one (double check); with the group, clicking an
    // action unchecks the rest atomically.
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    // currentOpt: current option (exact text of a QAction in the menu). Lives
    // in a shared_ptr because the triggers run long after this
    // function returns (the stack no longer exists).
    auto currentOpt = std::make_shared<QString>(current);
    auto syncMenu = [menu, btn](const QString &opt) {
        for (QAction *a : menu->actions())
            a->setChecked(a->text() == opt);
        btn->setText(opt);
    };
    // On opening, re-syncs checks and button text with the current
    // option; without this, Qt leaves the last action's check marked and the
    // menu reopens with stale checks ("double check").
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

// Variant with ASYNCHRONOUS confirmation (non-modal dialog). `confirm(opt,
// applyVisual)` decides per option whether to apply directly or show the modal:
//   - If it applies directly (e.g. back to 100%): it makes the real changes
//     (DB + signal) and calls applyVisual(opt) to reflect button + checks.
//   - If it shows the modal: on OK it applies the changes and calls applyVisual(opt);
//     on Cancel it does NOT call applyVisual — the button and menu keep the
//     previous option.
// applyVisual is a wrapper that also records the accepted option as the
// current one, so the next menu opening shows it checked.
void SettingsPage::setupValueMenuWithConfirm(
    QPushButton *btn, const QStringList &options, const QString &current,
    const std::function<void(const QString &,
                              const std::function<void(const QString &)> &)>
        &confirm)
{
    auto *menu = new QMenu(btn);
    // Exclusive group: only one QAction can stay checked BY FRAMEWORK (see
    // comment in setupValueMenu). With the group, clicking an option
    // unchecks the previous check atomically.
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    // currentOpt: visually current option (exact text of a QAction).
    auto currentOpt = std::make_shared<QString>(current);
    auto syncMenu = [menu, btn](const QString &opt) {
        for (QAction *a : menu->actions())
            a->setChecked(a->text() == opt);
        btn->setText(opt);
    };
    // applyVisual handed to the caller: records the accepted option and
    // reflects it. On modal cancel the caller does NOT invoke it, so currentOpt
    // stays on the previous option.
    auto applyAndTrack = [currentOpt, syncMenu](const QString &opt) {
        *currentOpt = opt;
        syncMenu(opt);
    };
    // Before opening, syncs the check with the current option (it may have
    // changed after a previous accept/cancel).
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
    // Deliberately NOT #sectionTitle (16 px): this is the title of the whole
    // page, one notch above the section title.
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

    // Each section is a "title + card" group: rows go inside the
    // card (#settingsCard), not loose in scrollLay. cardLay points at the
    // current section's card and is consumed by the row helpers
    // (addOptionRow, addActionButton and the inline blocks).
    QVBoxLayout *cardLay = nullptr;

    // Settings section header (INTERFAZ, REPRODUCCION, DATOS...). Deliberately
    // NOT #sectionSub: that is the 12 px subtitle of the other
    // pages, and here they are 11 px small-caps with some tracking.
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
        cv->setContentsMargins(14, 6, 14, 6);
        cv->setSpacing(8);
        gv->addWidget(card);
        scrollLay->addWidget(group);
        cardLay = cv;
    };

    // Clarifying note under a setting: always grouped with its row in a
    // short-spacing block (never loose in scrollLay, which spaces with 24px).
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
        // "Row + note" block: the note stays glued to its setting (spacing 4);
        // scrollLay's 24 spacing separates whole blocks, not row-note.
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

    // --- PLAYBACK ---
    addSectionTitle(QStringLiteral("REPRODUCCIÓN"));

    // Playback quality: resolution ceiling the app asks YouTube for.
    // Auto uses the last effective value saved (default 360p) and playback
    // itself re-memorizes it live without changing anything hot.
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

    // "A continuación" mode: what gets seeded into the queue when playing a video.
    // Radio: app-style radio mix (RAMDV). Suggestions: list of
    // recommendations for the video. Disabled: the queue stays as it came.
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

    // Video in the mini player: whether the live video fills the mini bar gap
    // when leaving the Player page, or whether only the
    // static thumbnail is preferred (in both cases the video keeps rendering in the
    // background so there is no jump on return).
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

    // Video acceleration: result of the offline probe (HwProbe). Informational
    // only; "Volver a detectar" (Redetect) clears the profile and re-runs the probe on the
    // spot (MainWindow::redetectHwRequested).
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

    // Audio language: preferred track when the video offers one (includes
    // automatic/AI dubs if they come with LANGUAGE).
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

    // Maximum volume (mpv "volume-max"): allows headroom above 100%.
    // Any option above 100% asks for confirmation before applying:
    // at high levels audio can damage headphones/ears. To avoid
    // surprises, the persisted volume is CAPPED at 100% — on closing the app
    // it always starts at 100% even if the previous session went higher.
    // If the user CANCELS the modal, the button still shows the
    // previous option (setupValueMenuWithConfirm does not apply visually on cancel).
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
        // `current` is the menu TEXT ("150%"), not the raw value ("150"):
        // checks and the button text compare against the QAction
        // text (which includes the %).
        setupValueMenuWithConfirm(
            btn, volumeMaxOptions(), volumeMaxFromValue(saved),
            [this](const QString &text,
                   const std::function<void(const QString &)> &applyVisual) {
                const QString val = valueFromVolumeMax(text);
                if (val == QStringLiteral("100")) {
                    // No modal: applies directly.
                    m_settings->set(QStringLiteral("volume_max"), val);
                    emit volumeMaxChanged(val);
                    applyVisual(text);
                    return;
                }
                // With modal: we apply ONLY on OK.
                const QString confirmText = QStringLiteral(
                    "Subir el volumen máximo a %1 puede causar daño auditivo "
                    "permanente. Úsalo solo con altavoces externos y a un "
                    "nivel cómodo. Al cerrar la app el volumen volverá a 100% "
                    "aunque lo subas más alto. ¿Continuar?").arg(val);
                nt::confirm(
                    this,
                    QStringLiteral("Volumen por encima del 100%"),
                    confirmText, QStringLiteral("Aceptar"),
                    [this, val, text, applyVisual]() {
                        m_settings->set(QStringLiteral("volume_max"), val);
                        emit volumeMaxChanged(val);
                        applyVisual(text);
                    });
                // On Cancel: applyVisual is not called, no DB write, no
                // signal emitted. Button and menu keep the previous option.
            });
        // Note glued to the block (it used to hang loose with the 24 spacing).
        block->addWidget(addNote(QStringLiteral(
            "Por seguridad, el volumen solo se guarda hasta el 100%. Al "
            "reiniciar la app, aunque el ajuste siga por encima, el "
            "reproductor empezará al 100%.")));
        cardLay->addLayout(block);
    }

    // --- INTERFACE ---
    addSectionTitle(QStringLiteral("INTERFAZ"));

    // Accent color: tone of interactive elements (sliders, progress
    // bar, primary button, selection, active nav, active icons on the
    // bottom bar). Applied hot (MainWindow regenerates the
    // global stylesheet and re-tints the nav icons when it receives
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

    // Thumbnail size: "Pequeñas" (6 per row, short rows), "Normales"
    // (5 per row) or "Grandes" (4 per row, tall rows). It is the setting the
    // user really perceives, and the one that DOES move the Home grid.
    //
    // Migration: the previous setting was called "Densidad de cuadrícula"
    // ("cozy"/"compact") and only scaled a factor, which on the Home
    // grids produced no visible change (the cell is 175 px with and without the
    // factor). Its value is read the first time so the choice is not lost:
    // "compact" → "Pequeñas", "cozy" → "Normales". The old key is left in
    // the DB (harmless) and gets rewritten on its own when an option is picked.
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

    // --- DATA ---
    addSectionTitle(QStringLiteral("DATOS"));

    // The buttons of the DATOS (Data) section. There are two: the normal one (#actionBtn) and the
    // reset one (#accentOutline, with border and accent color). The accent comes
    // from the global QSS, so it no longer needs re-applying when changed.
    auto addActionButton = [&cardLay](const QString &text, bool conAcento) {
        auto *btn = new QPushButton(text);
        btn->setObjectName(conAcento ? QStringLiteral("accentOutline")
                                     : QStringLiteral("dataActionBtn"));
        btn->setCursor(Qt::PointingHandCursor);
        cardLay->addWidget(btn);
        cardLay->addSpacing(4);
        return btn;
    };

    // Clear thumbnail cache: deletes only the images saved to disk
    // (they are re-downloaded on their own when videos are shown). Touches no data nor the DB.
    QPushButton *clearBtn = addActionButton(
        QStringLiteral("Limpiar caché de miniaturas"), false);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        nt::confirm(
            this, QStringLiteral("Limpiar caché de miniaturas"),
            QStringLiteral("¿Eliminar todas las miniaturas guardadas? "
                           "Las imágenes se volverán a descargar."),
            QStringLiteral("Eliminar"), [this]() { emit clearThumbsRequested(); });
    });

    // Reset app: deletes the local DB (history, searches,
    // subscriptions and settings) and the cache, and restarts the app with defaults.
    // Never touches personal files or downloaded videos.
    QPushButton *resetBtn = addActionButton(
        QStringLiteral("Restablecer aplicación"), true);
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        nt::confirm(
            this, QStringLiteral("Restablecer aplicación"),
            QStringLiteral("Esta acción borrará la base de datos local "
                           "(historial, búsquedas, suscripciones y ajustes) "
                           "y el caché de miniaturas. No se eliminarán "
                           "archivos personales ni vídeos descargados. "
                           "La aplicación se reiniciará con los valores por "
                           "defecto. ¿Continuar?"),
            QStringLiteral("Restablecer"), [this]() { emit resetRequested(); });
    });

    // --- ABOUT ---
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

    // The vertical remainder goes at the END: sections stay glued to the top with
    // uniform spacing. Before there were two intermediate addStretch (1:3) that
    // opened big gaps between REPRODUCCION/INTERFAZ/DATOS.
    scrollLay->addStretch(1);

    // The confirmations ("Volumen por encima del 100%", "Limpiar caché de
    // miniaturas" and "Restablecer aplicación") are no longer built here: they are
    // nt::confirm, the app's modal overlay, same as in Library,
    // History, Playlist and Search. Nothing has to be built at startup.
}

// showConfirmPopup, closeConfirmPopup, centerPopup and the resizeEvent that
// re-centered the popup are gone with it: nt::confirm builds its overlay every
// time it is called and destroys it when done, so there is nothing to re-center
// nor to remember between calls.