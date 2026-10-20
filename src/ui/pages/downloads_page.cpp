#include "downloads_page.h"

#include "../../i18n.h"
#include "../../ui/icons.h"
#include "../../ui/format.h"
#include "../../ui/theme.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// The bytes of a download in a form a person can read.
QString humanBytes(qint64 bytes)
{
    if (bytes <= 0)
        return QString();
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024) + QStringLiteral(" KB");
    if (bytes < 1024LL * 1024 * 1024)
        return QString::number(double(bytes) / (1024 * 1024), 'f', 1)
               + QStringLiteral(" MB");
    return QString::number(double(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 2)
           + QStringLiteral(" GB");
}

} // namespace

DownloadsPage::DownloadsPage(DownloadRepo *repo, QWidget *parent)
    : QWidget(parent)
    , m_repo(repo)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(10);

    auto *header = new QHBoxLayout();
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(m_title, "downloads.title");
    header->addWidget(m_title);
    header->addStretch(1);

    m_openFolder = new QPushButton(this);
    m_openFolder->setObjectName(QStringLiteral("ghost"));
    m_openFolder->setCursor(Qt::PointingHandCursor);
    nt::bindText(m_openFolder, "downloads.openFolder");
    connect(m_openFolder, &QPushButton::clicked, this, [this]() {
        if (m_repo) {
            DownloadInfo any;
            for (const DownloadInfo &info : m_repo->all()) {
                any = info;
                break;
            }
            const QString dir = QFileInfo(any.filePath).absolutePath();
            if (!dir.isEmpty() && QFileInfo::exists(dir))
                emit openFolderRequested(dir);
        }
    });
    header->addWidget(m_openFolder);
    root->addLayout(header);

    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("sectionSub"));
    root->addWidget(m_meta);

    m_empty = new QLabel(this);
    m_empty->setObjectName(QStringLiteral("emptyState"));
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    root->addWidget(m_empty);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_rows = new QWidget(scroll);
    m_listLayout = new QVBoxLayout(m_rows);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(8);
    m_listLayout->addStretch(1);
    scroll->setWidget(m_rows);
    root->addWidget(scroll, 1);

    reload();
}

QString DownloadsPage::stateText(const DownloadInfo &info) const
{
    // The state is a fixed vocabulary in the repository; HERE it is
    // translated for the person. Nothing else compares these strings.
    if (info.status == QLatin1String("downloading"))
        return nt::tr("downloads.downloading");
    if (info.status == QLatin1String("muxing"))
        return nt::tr("downloads.muxing");
    if (info.status == QLatin1String("done"))
        return nt::tr("downloads.done");
    if (info.status == QLatin1String("error"))
        return info.error.isEmpty() ? nt::tr("downloads.error")
                                     : info.error;
    if (info.status == QLatin1String("paused"))
        return nt::tr("downloads.paused");
    return nt::tr("downloads.pending");
}

// One row of the list. The parts that change (the title, the state, the
// progress) are kept in members so they can be UPDATED in place: rebuilding
// the row on every tick would restart the progress bar from zero and the
// bar would never look like it was moving.
QWidget *DownloadsPage::buildRow(const DownloadInfo &info)
{
    auto *row = new QWidget(this);
    row->setObjectName(QStringLiteral("downloadRow"));
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(12);

    auto *textCol = new QVBoxLayout();
    textCol->setSpacing(3);

    auto *title = new QLabel(info.title.isEmpty() ? info.videoId : info.title, row);
    title->setObjectName(QStringLiteral("downloadTitle"));
    title->setWordWrap(true);
    textCol->addWidget(title);

    auto *metaLabel = new QLabel(row);
    metaLabel->setObjectName(QStringLiteral("downloadMeta"));
    metaLabel->setWordWrap(true);
    textCol->addWidget(metaLabel);

    // The progress bar: it is only there while there is something to wait
    // for.
    auto *bar = new QProgressBar(row);
    bar->setObjectName(QStringLiteral("downloadProgress"));
    bar->setRange(0, 1000);
    bar->setValue(0);
    bar->setTextVisible(false);
    bar->setFixedHeight(4);
    textCol->addWidget(bar);

    lay->addLayout(textCol, 1);

    auto *play = new QPushButton(row);
    play->setObjectName(QStringLiteral("ghost"));
    play->setCursor(Qt::PointingHandCursor);
    play->setIcon(nt::icon(nt::Icon::Play, nt::kMuted, 16));
    play->setIconSize(QSize(16, 16));
    nt::bindTooltip(play, "downloads.play");
    nt::bindAccessibleName(play, "downloads.play");
    connect(play, &QPushButton::clicked, this,
            [this, info]() { emit playRequested(info.videoId, info.finalPath); });

    auto *retry = new QPushButton(row);
    retry->setObjectName(QStringLiteral("ghost"));
    retry->setCursor(Qt::PointingHandCursor);
    nt::bindText(retry, "downloads.retry");
    connect(retry, &QPushButton::clicked, this,
            [this, info]() { emit retryRequested(info.videoId); });

    auto *remove = new QPushButton(row);
    remove->setObjectName(QStringLiteral("ghost"));
    remove->setCursor(Qt::PointingHandCursor);
    remove->setIcon(nt::icon(nt::Icon::Clear, nt::kMuted, 16));
    remove->setIconSize(QSize(16, 16));
    nt::bindTooltip(remove, "downloads.delete");
    nt::bindAccessibleName(remove, "downloads.delete");
    connect(remove, &QPushButton::clicked, this,
            [this, info]() { emit removeRequested(info.videoId); });

    lay->addWidget(play);
    lay->addWidget(retry);
    lay->addWidget(remove);

    Row rowInfo;
    rowInfo.title = title;
    rowInfo.meta = metaLabel;
    rowInfo.bar = bar;
    rowInfo.play = play;
    rowInfo.retry = retry;
    rowInfo.remove = remove;
    m_rowParts.insert(info.videoId, rowInfo);

    updateRow(info, rowInfo);
    return row;
}

// Updates a row in place: the text, the progress bar and the buttons that
// depend on the state.
void DownloadsPage::updateRow(const DownloadInfo &info, const Row &row)
{
    row.title->setText(info.title.isEmpty() ? info.videoId : info.title);

    QString meta = stateText(info);
    if (!info.quality.isEmpty())
        meta += QStringLiteral(" · ") + info.quality;
    if (info.bytesTotal > 0) {
        meta += QStringLiteral(" · ") + humanBytes(info.bytesDone)
                + QStringLiteral(" / ") + humanBytes(info.bytesTotal);
    }
    // The speed, while the download runs: it is what tells whether it is
    // going well or it is stuck.
    if (info.status == QLatin1String("downloading")
        && info.bytesPerSecond > 0) {
        meta += QStringLiteral(" · ") + humanBytes(info.bytesPerSecond)
                + QStringLiteral("/s");
    }
    row.meta->setText(meta);

    const bool working = info.status == QLatin1String("downloading")
                         || info.status == QLatin1String("muxing");
    row.bar->setVisible(working);
    if (working) {
        // The bar goes from 0 to 1 with the bytes that are really on the
        // disk. While the mux is running it is full (the bytes are there).
        const int value = info.status == QLatin1String("muxing")
            ? 1000
            : int(qBound(0.0, info.progress, 1.0) * 1000.0);
        row.bar->setValue(value);
    }

    const bool done = info.status == QLatin1String("done");
    const bool failed = info.status == QLatin1String("error");
    row.play->setVisible(done);
    row.retry->setVisible(failed);
    row.remove->setVisible(true);
}

void DownloadsPage::reload()
{
    const QVector<DownloadInfo> all = m_repo ? m_repo->all()
                                              : QVector<DownloadInfo>();

    // The rows are NOT rebuilt: they are updated in place, so the progress
    // bar of a download that is running keeps moving (rebuilding the row
    // restarted it from zero every time).
    for (auto it = m_rowParts.begin(); it != m_rowParts.end();) {
        bool found = false;
        for (const DownloadInfo &info : all) {
            if (info.videoId == it.key()) {
                updateRow(info, it.value());
                found = true;
                break;
            }
        }
        if (!found)
            it = m_rowParts.erase(it);
        else
            ++it;
    }

    // The rows that are new are added at the end (the list is newest first,
    // and the new ones are the last).
    for (int i = all.size() - 1; i >= 0; --i) {
        const DownloadInfo &info = all.at(i);
        if (m_rowParts.contains(info.videoId))
            continue;
        m_listLayout->insertWidget(m_listLayout->count() - 1,
                                   buildRow(info));
    }

    m_meta->setText(nt::tr("common.videos").arg(all.size()));
    m_empty->setText(nt::emptyStateHtml(nt::tr("downloads.empty")));
    m_empty->setVisible(all.isEmpty());
    m_rows->setVisible(!all.isEmpty());
    m_openFolder->setVisible(!all.isEmpty());
}

void DownloadsPage::retranslate()
{
    // The title and the button are live-bound; the counter and the empty
    // state (wrapped by emptyStateHtml, which adds the logo) are composed
    // and have to be re-applied here.
    reload();
}
