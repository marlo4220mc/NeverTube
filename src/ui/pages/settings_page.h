#pragma once

#include <QWidget>

#include "../../downloader/download_settings.h"

#include <functional>

class QMenu;
class QPushButton;
class QLabel;
class SettingsRepo;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(SettingsRepo *settings, QWidget *parent = nullptr);

    // Refreshes the "Aceleración de vídeo" row with the current profile.
    void updateCodecProfile(const QString &profile);
    // Re-applies the "Restablecer aplicación" button with the current accent
    // (MainWindow calls it when the accent color changes in Settings).
    void applyAccent();

    // Rebuilds the page after a language change. The option IDENTITY is a
    // stable code and the visible text is a translation, so recreating the UI
    // from the settings is exact and leaves every menu in the new language
    // (the menus and checks are ephemeral and would otherwise keep the old
    // wording). MainWindow calls it deferred (QTimer::singleShot(0)) so the
    // menu that emitted languageChanged is not destroyed while open.
    void retranslate();

    // "Tamaño de las miniaturas" (Settings → Interface): persisted value
    // ("small" | "normal" | "large"), already migrated from the old setting
    // "Densidad de cuadrícula". MainWindow reads it at startup to draw with
    // the right size from the first frame, without waiting for a change.
    QString thumbSize() const { return m_thumbSize; }
    // The value maps to TWO effects, because grids and lists are not
    // sized the same (see AGENTS.md):
    //  - the LIST VIEWS (search, channel, history, playlists, the "A
    //    continuación" queue) scale their row and thumbnail by a factor;
    //  - Home's GRIDS are sized by width with a FIXED number
    //    of columns, so a factor does nothing visible: the lever
    //    is the number of columns.
    static double thumbListScale(const QString &value);
    static int thumbGridColumns(const QString &value);

signals:
    void qualityChanged(const QString &value);
    void nextModeChanged(const QString &mode);
    void miniVideoModeChanged(const QString &mode);
    void audioLanguageChanged(const QString &code);
    // The folder of the downloads changed (empty = the default one).
    void downloadFolderChanged(const QString &folder);
    // The number of connections of the downloader changed.
    void downloadConnectionsChanged(const QString &connections);
    void volumeMaxChanged(const QString &value);
    void thumbSizeChanged(const QString &value);
    void accentChanged(const QString &hex);
    void languageChanged(const QString &code);
    void clearThumbsRequested();
    void resetRequested();
    void redetectHwRequested();
    // "Buscar actualizaciones" (About section): the query and the dialog are
    // MainWindow's, which owns the player (the automatic check holds the
    // dialog while a video plays) and the settings (it remembers when the
    // last check was and which version was postponed).
    void checkUpdatesRequested();

private:
    void buildUI();
    // "Label + value button" row: the button opens a QMenu with the
    // alternatives (same pattern as the three dots in Library). Choosing
    // an option calls onPick(displayText).
    // Simple action button: used e.g. by "Volver a detectar".
    QPushButton *makeActionButton(const QString &text);
    // VALUE button: opens a QMenu with the alternatives. It looks the same
    // as the action one and has NO trailing triangle (user's decision:
    // the "▼" was redundant; see the comment in the definition).
    QPushButton *makeValueButton(const QString &text);
    void setupValueMenu(QPushButton *btn, const QStringList &options,
                        const QString &current,
                        const std::function<QString(const QString &)> &display,
                        const std::function<void(const QString &)> &onPick);
    // Variant with asynchronous (modal) confirmation. `confirm(opt, applyVisual)`
    // decides per option: applies directly or shows the modal; on Accept it calls
    // applyVisual (which records the option and updates button + checks), on
    // Cancel it does NOT call it. `current` is an option CODE, and `display`
    // turns a code into the text to show (menu entries and button).
    void setupValueMenuWithConfirm(
        QPushButton *btn, const QStringList &options, const QString &current,
        const std::function<QString(const QString &)> &display,
        const std::function<void(const QString &,
                                  const std::function<void(const QString &)> &)>
            &confirm);
    // Confirmations: nt::confirm, the SAME modal overlay of the app that
    // Library, History, Playlist and Search use. Before, this page had its
    // own popup (380 px instead of nt::confirm's 400, no veil, no
    // modal and with its own re-centering piece), that is, a fourth place where
    // "confirm before deleting" looked and behaved differently.

    SettingsRepo *m_settings;
    QLabel *m_codecProfileLabel = nullptr;
    // Persisted value of "Tamaño de las miniaturas" (see thumbSize()).
    QString m_thumbSize;
};