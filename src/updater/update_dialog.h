#pragma once

#include <QDialog>

#include "update_check.h"

class QLabel;
class QProgressBar;
class QPushButton;

namespace nt {

// The window the updater shows: installed version, available version, the
// published notes, and the two decisions (update / later). It is a QDialog and
// not the app's overlay on purpose: it appears while SOMETHING ELSE may be
// playing, it has its own title and it is the one window that can stay open
// while the person keeps using the app underneath... which is why it is not
// modal either: exec() is called by MainWindow with the window as parent, and
// the download runs in the event loop like everything else.
//
// The dialog owns the download and the verification; launching the installer
// and closing the app is MainWindow's, which is the one that knows the player
// and the downloader.
class UpdateDialog : public QDialog
{
    Q_OBJECT

public:
    UpdateDialog(const UpdateRelease &release, const QString &installedVersion,
                 QWidget *parent = nullptr);

signals:
    // "Later": the version must not be offered again by the AUTOMATIC check
    // (a manual check always answers). MainWindow persists it.
    void postponed(const QString &version);
    // The file is on disk and its SHA-256 matched the manifest; the person
    // has confirmed. MainWindow runs it and closes.
    void installRequested(const QString &installerPath);

private:
    void buildUI();
    void refreshButtons();
    void startDownload();
    void onProgress(qint64 received, qint64 total);
    void onReady(const QString &path);
    void onFailed(const QString &message);
    void onCancelled();
    void confirmInstall(const QString &path);

    UpdateRelease m_release;
    QString m_installed;
    bool m_hasUpdate = false;
    bool m_canInstall = false;

    UpdateDownloader *m_downloader = nullptr;
    QLabel *m_notes = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_primary = nullptr;
    QPushButton *m_secondary = nullptr;
    QString m_verifiedPath;
};

} // namespace nt
