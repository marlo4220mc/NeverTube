#pragma once

#include <QHash>
#include <QVector>
#include <QWidget>

#include "../../db/repositories.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QVBoxLayout;

// The "Descargas" tab: the list of what was downloaded, with its progress
// and its state, and the actions on each one.
//
// It is NOT a VideoListView: a download is not a video of the catalog (it
// has a progress, a state and a file on disk), so the rows are built here.
class DownloadsPage : public QWidget
{
    Q_OBJECT

public:
    explicit DownloadsPage(DownloadRepo *repo, QWidget *parent = nullptr);

    // The folder where the files are written: the one of the downloader.
    // The button of "Abrir carpeta" opens THIS one (not the path of a
    // row: the file of a download in progress is a partial one, and the
    // path of a finished one is not known until it finishes).
    void setFolder(const QString &folder) { m_folder = folder; }

    // Rebuilds the list from the repository.
    void reload();

    // Re-applies the composed texts (the title, the counter and the empty
    // state, which is wrapped by emptyStateHtml) in the active language.
    void retranslate();

signals:
    // The user wants to play a downloaded file.
    //
    // The id of a download is the KEY of its row (DownloadInfo::identity())
    // and NOT necessarily the video: the video and the mp3 of the same video
    // are two rows, each one with its own file.
    void playRequested(const QString &key, const QString &filePath);
    // The user wants the folder of the downloads opened.
    void openFolderRequested(const QString &path);
    // The user wants a download retried.
    void retryRequested(const QString &key);
    // The user wants a download removed (the file goes with it).
    void removeRequested(const QString &key);
    // The user wants to pause a download (or to continue it).
    void pauseRequested(const QString &key);
    // The user wants to continue a paused download.
    void resumeRequested(const QString &key);

private:
    // The widgets of one row, so it can be UPDATED in place (rebuilding it
    // would restart the progress bar from zero on every tick, and the bar
    // would never look like it was moving).
    struct Row
    {
        QLabel *title = nullptr;
        QLabel *meta = nullptr;
        QProgressBar *bar = nullptr;
        QPushButton *play = nullptr;
        QPushButton *retry = nullptr;
        QPushButton *remove = nullptr;
        // The one that pauses the download and the one that continues it:
        // the SAME button, with the icon of the state (the row is updated
        // in place, so the button is not rebuilt).
        QPushButton *pause = nullptr;
    };

    QWidget *buildRow(const DownloadInfo &info);
    // The CURRENT state of a row: the rows are updated in place (the same
    // widget), so the buttons cannot decide with the copy that was
    // captured when the row was built (it is the old one).
    bool rowIsPaused(const QString &key) const;
    QString finalPathOf(const QString &key) const;
    void updateRow(const DownloadInfo &info, const Row &row);
    QString stateText(const DownloadInfo &info) const;

    DownloadRepo *m_repo;
    QHash<QString, Row> m_rowParts;
    QVBoxLayout *m_listLayout = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_empty = nullptr;
    QWidget *m_rows = nullptr;
    QPushButton *m_openFolder = nullptr;
    // The folder of the downloads (the one of the downloader).
    QString m_folder;
};
