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

    // Rebuilds the list from the repository.
    void reload();

    // Re-applies the composed texts (the title, the counter and the empty
    // state, which is wrapped by emptyStateHtml) in the active language.
    void retranslate();

signals:
    // The user wants to play a downloaded file.
    void playRequested(const QString &videoId, const QString &filePath);
    // The user wants the folder of the downloads opened.
    void openFolderRequested(const QString &path);
    // The user wants a download retried.
    void retryRequested(const QString &videoId);
    // The user wants a download removed (the file goes with it).
    void removeRequested(const QString &videoId);

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
    };

    QWidget *buildRow(const DownloadInfo &info);
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
};
