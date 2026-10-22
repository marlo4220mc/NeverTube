#include "settings_page.h"

#include "../../downloader/download_folder.h"
#include "../../downloader/download_settings.h"

#include <QActionGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileDialog>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "../../db/repositories.h"
#include "../../i18n.h"
#include "../confirm_dialog.h"
#include "../theme.h"

static const char *kGridDensityKey = "grid_density";
static const char *kThumbSizeKey = "thumb_size";
static const char *kAccentKey = "ui_accent";

static QStringList qualityOptions(bool noHw = false)
{
    if (noHw)
        return {QStringLiteral("auto"), QStringLiteral("144"),
                QStringLiteral("240"), QStringLiteral("360"),
                QStringLiteral("480"), QStringLiteral("720"),
                QStringLiteral("1080")};
    return {QStringLiteral("auto"), QStringLiteral("144"),
            QStringLiteral("240"), QStringLiteral("360"),
            QStringLiteral("480"), QStringLiteral("720"),
            QStringLiteral("1080"), QStringLiteral("1440"),
            QStringLiteral("2160")};
}

static QString qualityDisplay(const QString &code)
{
    if (code == QStringLiteral("auto"))
        return nt::tr("settings.quality.auto");
    return code + QLatin1Char('p');
}

static QStringList nextModeOptions()
{
    return {QStringLiteral("radio"), QStringLiteral("suggestions"),
            QStringLiteral("none")};
}

static QString nextModeDisplay(const QString &code)
{
    if (code == QStringLiteral("none"))
        return nt::tr("settings.nextMode.none");
    if (code == QStringLiteral("suggestions"))
        return nt::tr("settings.nextMode.suggestions");
    return nt::tr("settings.nextMode.radio");
}

static QStringList miniVideoOptions()
{
    return {QStringLiteral("video"), QStringLiteral("thumb")};
}

static QString miniVideoDisplay(const QString &code)
{
    if (code == QStringLiteral("thumb"))
        return nt::tr("settings.miniVideo.thumb");
    return nt::tr("settings.miniVideo.video");
}

static QStringList audioLangOptions()
{
    return {QStringLiteral("original"), QStringLiteral("es"),
            QStringLiteral("en"), QStringLiteral("pt")};
}

static QString audioLangDisplay(const QString &code)
{
    if (code == QLatin1String("es"))
        return nt::tr("settings.audioLang.es");
    if (code == QLatin1String("en"))
        return nt::tr("settings.audioLang.en");
    if (code == QLatin1String("pt"))
        return nt::tr("settings.audioLang.pt");
    return nt::tr("settings.audioLang.original");
}

// Maximum volume (mpv "volume-max"): 100% by default, 150/175/200 available
// as "headroom" for those who need more volume. Each option above 100% asks
// for confirmation for hearing safety (risk of damage with headphones).
static QStringList volumeMaxOptions()
{
    return {QStringLiteral("100"), QStringLiteral("150"),
            QStringLiteral("175"), QStringLiteral("200")};
}

static QString volumeMaxDisplay(const QString &code)
{
    return code + QLatin1Char('%');
}

// "Tamaño de las miniaturas". Three levels that translate into TWO effects, because
// the Home grids and the list views are not sized the same way: the
// grids are width-sized with a FIXED number of columns (the
// factor does nothing visible: with 5 columns at 903 px the cell measures 175 px with
// density 1.0 and 175 px with 0.88), while the lists scale their row with
// the factor. That is why the Home lever is the COLUMNS.
static QStringList thumbSizeOptions()
{
    return {QStringLiteral("small"), QStringLiteral("normal"),
            QStringLiteral("large")};
}

static QString thumbSizeDisplay(const QString &code)
{
    if (code == QStringLiteral("small"))
        return nt::tr("settings.thumbSize.small");
    if (code == QStringLiteral("large"))
        return nt::tr("settings.thumbSize.large");
    return nt::tr("settings.thumbSize.normal");
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
// nt::accentOptions() (src/ui/theme.h); here only hex<->label is bridged and
// persisted with kAccentKey. The IDENTITY of an option is its HEX (stable
// across languages); the shown text is the already-translated label. The
// default is the first palette entry (the icon red).
static QStringList accentOptions()
{
    QStringList out;
    for (const nt::AccentOption &o : nt::accentOptions())
        out << o.hex;
    return out;
}

static QString accentDisplay(const QString &hex)
{
    for (const nt::AccentOption &o : nt::accentOptions())
        if (o.hex.compare(hex, Qt::CaseInsensitive) == 0)
            return o.label;
    return nt::accentOptions().value(0).label;
}

// Readable text of the profile detected by HwProbe (codec_profile).
static QString codecProfileText(const QString &val)
{
    if (val == QLatin1String("av1-hw"))
        return nt::tr("codec.av1");
    if (val == QLatin1String("vp9-hw"))
        return nt::tr("codec.vp9");
    if (val == QLatin1String("h264-hw"))
        return nt::tr("codec.h264");
    if (val == QLatin1String("soft-all") || val == QLatin1String("nohw"))
        return nt::tr("codec.soft");
    return nt::tr("codec.unknown");
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

void SettingsPage::retranslate()
{
    // Recreate the whole page: every row comes out of the settings again and
    // the menus are rebuilt with the new language. The identity of an option
    // is a stable code, so rebuilding loses no choice. The signal that led
    // here comes from a menu born in this page, so MainWindow calls this
    // deferred (QTimer::singleShot(0)) and the old widgets are deleted
    // outside of their own event delivery.
    const auto children =
        findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *w : children)
        delete w;
    delete layout();
    buildUI();
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
    const std::function<QString(const QString &)> &display,
    const std::function<void(const QString &)> &onPick)
{
    auto *menu = new QMenu(btn);
    // Exclusive group: only one QAction can stay checked BY FRAMEWORK. Qt's
    // automatic toggle on loose checkable actions marked the new one
    // without unchecking the previous one (double check); with the group, clicking an
    // action unchecks the rest atomically.
    auto *group = new QActionGroup(menu);
    group->setExclusive(true);
    // currentOpt: the OPTION CODE currently in effect (never the visible
    // text: that one changes with the language). Lives in a shared_ptr
    // because the triggers run long after this function returns (the stack
    // no longer exists).
    auto currentOpt = std::make_shared<QString>(current);
    auto syncMenu = [menu, btn, display](const QString &opt) {
        for (QAction *a : menu->actions())
            a->setChecked(a->text() == display(opt));
        btn->setText(display(opt));
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
        QAction *act = menu->addAction(display(opt));
        act->setCheckable(true);
        group->addAction(act);
        connect(act, &QAction::triggered, btn,
                [opt, onPick, currentOpt, syncMenu]() {
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
    const std::function<QString(const QString &)> &display,
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
    // currentOpt: option CODE in effect (the visible text changes with the
    // language, so it must not be the identity).
    auto currentOpt = std::make_shared<QString>(current);
    auto syncMenu = [menu, btn, display](const QString &opt) {
        for (QAction *a : menu->actions())
            a->setChecked(a->text() == display(opt));
        btn->setText(display(opt));
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
        QAction *act = menu->addAction(display(opt));
        act->setCheckable(true);
        group->addAction(act);
        connect(act, &QAction::triggered, btn,
                [opt, confirm, applyAndTrack]() {
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

    auto *title = new QLabel(nt::tr("settings.title"), this);
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
                            const std::function<QString(const QString &)> &display,
                            const std::function<void(const QString &)> &onPick,
                            const QString &note = QString(),
                            const QString &mark = QString()) {
        // "Row + note" block: the note stays glued to its setting (spacing 4);
        // scrollLay's 24 spacing separates whole blocks, not row-note.
        auto *block = new QVBoxLayout;
        block->setSpacing(4);
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *lbl = new QLabel(labelText);
        lbl->setObjectName(QStringLiteral("settingsRowLabel"));
        row->addWidget(lbl);
        QPushButton *btn = makeValueButton(display(saved));
        // The mark is what the selftest of the page looks for (it says what
        // the setting is, and it is not a text that can change).
        if (!mark.isEmpty())
            btn->setProperty("downloadSetting", mark);
        row->addWidget(btn, 1, Qt::AlignRight);
        block->addLayout(row);
        if (!note.isEmpty())
            block->addWidget(addNote(note));
        cardLay->addLayout(block);
        setupValueMenu(btn, options, saved, display, onPick);
    };

    // --- PLAYBACK ---
    addSectionTitle(nt::tr("settings.section.playback"));

    // Playback quality: resolution ceiling the app asks YouTube for.
    // Auto uses the last effective value saved (default 360p) and playback
    // itself re-memorizes it live without changing anything hot.
    addOptionRow(
        nt::tr("settings.quality"),
        qualityOptions(m_settings->getString(
                           QStringLiteral("codec_profile"),
                           QStringLiteral("")) == QLatin1String("nohw")
                       || m_settings->getString(
                              QStringLiteral("codec_profile"),
                              QStringLiteral("")) == QLatin1String("soft-all")),
        m_settings->getString(QStringLiteral("quality"),
                              QStringLiteral("auto")),
        qualityDisplay,
        [this](const QString &code) {
            m_settings->set(QStringLiteral("quality"), code);
            emit qualityChanged(code);
            if (code == QStringLiteral("auto")) {
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
        nt::tr("settings.nextMode"), nextModeOptions(),
        m_settings->getString(QStringLiteral("next_mode"),
                              QStringLiteral("radio")),
        nextModeDisplay,
        [this](const QString &code) {
            m_settings->set(QStringLiteral("next_mode"), code);
            emit nextModeChanged(code);
        });

    // Video in the mini player: whether the live video fills the mini bar gap
    // when leaving the Player page, or whether only the
    // static thumbnail is preferred (in both cases the video keeps rendering in the
    // background so there is no jump on return).
    addOptionRow(
        nt::tr("settings.miniVideo"), miniVideoOptions(),
        m_settings->getString(QStringLiteral("mini_video"),
                              QStringLiteral("thumb")),
        miniVideoDisplay,
        [this](const QString &code) {
            m_settings->set(QStringLiteral("mini_video"), code);
            emit miniVideoModeChanged(code);
        });

    // Video acceleration: result of the offline probe (HwProbe). Informational
    // only; "Volver a detectar" (Redetect) clears the profile and re-runs the probe on the
    // spot (MainWindow::redetectHwRequested).
    {
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *lbl = new QLabel(nt::tr("settings.hwAccel"));
        lbl->setObjectName(QStringLiteral("settingsRowLabel"));
        row->addWidget(lbl);
        m_codecProfileLabel = new QLabel(codecProfileText(
            m_settings->getString(QStringLiteral("codec_profile"),
                                  QString())));
        m_codecProfileLabel->setObjectName(QStringLiteral("settingsRowValue"));
        row->addWidget(m_codecProfileLabel);
        row->addStretch(1);
#ifndef NEVERTUBE_NO_MPV
        auto *btn = makeActionButton(nt::tr("codec.redetect"));
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
        nt::tr("settings.audioLang"), audioLangOptions(),
        m_settings->getString(QStringLiteral("audio_language"),
                              QStringLiteral("original")),
        audioLangDisplay,
        [this](const QString &code) {
            m_settings->set(QStringLiteral("audio_language"), code);
            emit audioLanguageChanged(code);
        },
        nt::tr("settings.audioLangNote"));

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
        auto *lbl = new QLabel(nt::tr("settings.volumeMax"));
        lbl->setObjectName(QStringLiteral("settingsRowLabel"));
        row->addWidget(lbl);
        const QString saved = m_settings->getString(
            QStringLiteral("volume_max"), QStringLiteral("100"));
        QPushButton *btn = makeValueButton(volumeMaxDisplay(saved));
        row->addWidget(btn, 1, Qt::AlignRight);
        block->addLayout(row);
        // `saved` is the raw value ("150"); the menu shows "150%".
        setupValueMenuWithConfirm(
            btn, volumeMaxOptions(), saved, volumeMaxDisplay,
            [this](const QString &code,
                   const std::function<void(const QString &)> &applyVisual) {
                if (code == QStringLiteral("100")) {
                    // No modal: applies directly.
                    m_settings->set(QStringLiteral("volume_max"), code);
                    emit volumeMaxChanged(code);
                    applyVisual(code);
                    return;
                }
                // With modal: we apply ONLY on OK.
                const QString confirmText =
                    nt::tr("settings.volumeMaxBody").arg(code);
                nt::confirm(
                    this, nt::tr("settings.volumeMaxTitle"), confirmText,
                    nt::tr("common.accept"),
                    [this, code, applyVisual]() {
                        m_settings->set(QStringLiteral("volume_max"), code);
                        emit volumeMaxChanged(code);
                        applyVisual(code);
                    });
                // On Cancel: applyVisual is not called, no DB write, no
                // signal emitted. Button and menu keep the previous option.
            });
        // Note glued to the block (it used to hang loose with the 24 spacing).
        block->addWidget(addNote(nt::tr("settings.volumeMaxNote")));
        cardLay->addLayout(block);
    }

    // --- INTERFACE ---
    addSectionTitle(nt::tr("settings.section.interface"));

    // Accent color: tone of interactive elements (sliders, progress
    // bar, primary button, selection, active nav, active icons on the
    // bottom bar). Applied hot (MainWindow regenerates the
    // global stylesheet and re-tints the nav icons when it receives
    // accentChanged).
    addOptionRow(
        nt::tr("settings.accent"), accentOptions(),
        m_settings->getString(QLatin1String(kAccentKey), nt::accentHex()),
        accentDisplay,
        [this](const QString &hex) {
            m_settings->set(QLatin1String(kAccentKey), hex);
            emit accentChanged(hex);
        },
        nt::tr("settings.accentNote"));

    // Thumbnail size: "Small" (6 per row, short rows), "Normal" (5 per row)
    // or "Large" (4 per row, tall rows). It is the setting the
    // user really perceives, and the one that DOES move the Home grid.
    //
    // Migration: the previous setting was called "grid density"
    // ("cozy"/"compact") and only scaled a factor, which on the Home
    // grids produced no visible change (the cell is 175 px with and without the
    // factor). Its value is read the first time so the choice is not lost:
    // "compact" → "small", "cozy" → "normal". The old key is left in
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
        nt::tr("settings.thumbSize"), thumbSizeOptions(),
        m_thumbSize, thumbSizeDisplay,
        [this](const QString &code) {
            m_thumbSize = code;
            m_settings->set(QString::fromLatin1(kThumbSizeKey), code);
            emit thumbSizeChanged(code);
        },
        nt::tr("settings.thumbSizeNote"));

    // Language: one option per catalog in :/localization (localization.qrc).
    // The identity is the code ("es", "en"); the shown text is the language's
    // own name (never translated). Adding a language = dropping its JSON file
    // and listing it in the .qrc: this row picks it up by itself.
    {
        const QList<nt::LanguageInfo> langs = nt::availableLanguages();
        QStringList codes;
        for (const nt::LanguageInfo &l : langs)
            codes << l.code;
        addOptionRow(
            nt::tr("settings.language"), codes,
            m_settings->getString(QStringLiteral("ui_lang"), nt::language()),
            [langs](const QString &code) {
                for (const nt::LanguageInfo &l : langs)
                    if (l.code == code)
                        return l.name;
                return code;
            },
            [this](const QString &code) {
                m_settings->set(QStringLiteral("ui_lang"), code);
                emit languageChanged(code);
            });
    }

    // --- DOWNLOADS ---
    addSectionTitle(nt::tr("settings.section.downloads"));

    // The folder where the files are written. It is a PATH and not a code,
    // because it is a place of the file system: the default one (the folder
    // of downloads of the system plus the subfolder "NeverTube") until it is
    // changed here.
    //
    // The row shows the FOLDER (not a code): the identity of the option is
    // the path itself.
    {
        const QString saved = m_settings->getString(
            QString::fromLatin1(downloadsettings::kFolderKey), QString());
        auto *block = new QVBoxLayout;
        block->setSpacing(4);
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        auto *lbl = new QLabel(nt::tr("settings.downloadFolder"));
        lbl->setObjectName(QStringLiteral("settingsRowLabel"));
        row->addWidget(lbl);
        auto *btn = makeValueButton(saved.isEmpty()
                                        ? nt::tr("settings.downloadFolderDefault")
                                        : saved);
        btn->setProperty("downloadSetting", QStringLiteral("folder"));
        btn->setToolTip(saved.isEmpty() ? downloads::folderOf(QString()) : saved);
        row->addWidget(btn, 1, Qt::AlignRight);
        block->addLayout(row);
        block->addWidget(addNote(nt::tr("settings.downloadFolderNote")));
        cardLay->addLayout(block);

        // The menu of the row: the current folder, the default one and the
        // two actions (choose a folder, go back to the default).
        auto *menu = new QMenu(btn);
        QAction *current = menu->addAction(
            saved.isEmpty() ? nt::tr("settings.downloadFolderDefault") : saved);
        current->setCheckable(true);
        current->setChecked(true);
        menu->addSeparator();
        QAction *choose = menu->addAction(nt::tr("settings.downloadFolderChoose"));
        QAction *reset = menu->addAction(nt::tr("settings.downloadFolderReset"));
        connect(btn, &QPushButton::clicked, btn,
                [btn, menu]() { menu->popup(btn->mapToGlobal(QPoint(0, btn->height()))); });
        connect(choose, &QAction::triggered, this, [this, btn, saved]() {
            // The dialog of the system to choose a folder: it is the ONLY
            // place where a QDialog is right (choosing a place of the file
            // system cannot be invented).
            const QString dir = QFileDialog::getExistingDirectory(
                this, nt::tr("settings.downloadFolder"),
                saved.isEmpty() ? downloads::folderOf(QString()) : saved);
            if (dir.isEmpty())
                return;
            m_settings->set(QString::fromLatin1(downloadsettings::kFolderKey), dir);
            btn->setText(dir);
            btn->setToolTip(dir);
            emit downloadFolderChanged(dir);
        });
        connect(reset, &QAction::triggered, this, [this, btn]() {
            m_settings->set(QString::fromLatin1(downloadsettings::kFolderKey), QString());
            btn->setText(nt::tr("settings.downloadFolderDefault"));
            btn->setToolTip(downloads::folderOf(QString()));
            emit downloadFolderChanged(QString());
        });
    }

    // The number of connections of the downloader: it is what the person
    // NOTICES (the speed) and there is no menu for it.
    {
        QStringList options;
        for (int n = downloadsettings::kMinConnections;
             n <= downloadsettings::kMaxConnections; ++n)
            options << QString::number(n);
        const QString saved = m_settings->getString(
            QString::fromLatin1(downloadsettings::kConnectionsKey), QString());
        const QString current = downloadsettings::valueForConnections(
            saved.isEmpty() ? downloadsettings::kDefaultConnections
                            : saved.toInt());
        addOptionRow(
            nt::tr("settings.downloadConnections"), options, current,
            [](const QString &code) {
                return QStringLiteral("%1").arg(code);
            },
            [this](const QString &code) {
                m_settings->set(QString::fromLatin1(downloadsettings::kConnectionsKey),
                                code);
                emit downloadConnectionsChanged(code);
            },
            nt::tr("settings.downloadConnectionsNote"), QStringLiteral("connections"));
    }

    // --- DATA ---
    addSectionTitle(nt::tr("settings.section.data"));

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
        nt::tr("settings.clearThumbs"), false);
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        nt::confirm(
            this, nt::tr("settings.clearThumbs"),
            nt::tr("settings.clearThumbsBody"),
            nt::tr("settings.delete"), [this]() { emit clearThumbsRequested(); });
    });

    // Reset app: deletes the local DB (history, searches,
    // subscriptions and settings) and the cache, and restarts the app with defaults.
    // Never touches personal files or downloaded videos.
    QPushButton *resetBtn = addActionButton(
        nt::tr("settings.resetApp"), true);
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        nt::confirm(
            this, nt::tr("settings.resetApp"),
            nt::tr("settings.resetBody"),
            nt::tr("settings.reset"), [this]() { emit resetRequested(); });
    });

    // --- ABOUT ---
    addSectionTitle(nt::tr("settings.section.about"));

    auto *verLabel = new QLabel(nt::tr("settings.version"), content);
    verLabel->setObjectName(QStringLiteral("settingsVersion"));

    auto *aboutBlock = new QVBoxLayout;
    aboutBlock->setSpacing(4);
    aboutBlock->addWidget(verLabel);
    aboutBlock->addWidget(addNote(nt::tr("settings.aboutNote")));
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