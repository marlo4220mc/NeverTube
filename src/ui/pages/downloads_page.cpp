#include "downloads_page.h"

#include "../../i18n.h"
#include "../../ui/icons.h"
#include "../../ui/format.h"
#include "../../ui/theme.h"

#include <QProgressBar>

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
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

    QString meta = stateText(info);
    if (!info.quality.isEmpty())
        meta += QStringLiteral(" · ") + info.quality;
    if (info.bytesTotal > 0) {
        meta += QStringLiteral(" · ") + humanBytes(info.bytesDone)
                + QStringLiteral(" / ") + humanBytes(info.bytesTotal);
    }
    auto *metaLabel = new QLabel(meta, row);
    metaLabel->setObjectName(QStringLiteral("downloadMeta"));
    metaLabel->setWordWrap(true);
    textCol->addWidget(metaLabel);

    // The progress bar: it is only there while there is something to wait
    // for.
    if (info.status == QLatin1String("downloading")
        || info.status == QLatin1String("muxing")) {
        auto *bar = new QProgressBar(row);
        bar->setObjectName(QStringLiteral("downloadProgress"));
        bar->setRange(0, 1000);
        bar->setValue(int(info.progress * 1000.0));
        bar->setTextVisible(false);
        bar->setFixedHeight(4);
        textCol->addWidget(bar);
    }

    lay->addLayout(textCol, 1);

    if (info.status == QLatin1String("done")) {
        auto *play = new QPushButton(row);
        play->setObjectName(QStringLiteral("ghost"));
        play->setCursor(Qt::PointingHandCursor);
        play->setIcon(nt::icon(nt::Icon::Play, nt::kMuted, 16));
        play->setIconSize(QSize(16, 16));
        nt::bindTooltip(play, "downloads.play");
        nt::bindAccessibleName(play, "downloads.play");
        connect(play, &QPushButton::clicked, this,
                [this, info]() { emit playRequested(info.videoId, info.finalPath); });
        lay->addWidget(play);
    } else if (info.status == QLatin1String("error")) {
        auto *retry = new QPushButton(row);
        retry->setObjectName(QStringLiteral("ghost"));
        retry->setCursor(Qt::PointingHandCursor);
        nt::bindText(retry, "downloads.retry");
        connect(retry, &QPushButton::clicked, this,
                [this, info]() { emit retryRequested(info.videoId); });
        lay->addWidget(retry);
    }

    auto *remove = new QPushButton(row);
    remove->setObjectName(QStringLiteral("ghost"));
    remove->setCursor(Qt::PointingHandCursor);
    remove->setIcon(nt::icon(nt::Icon::Clear, nt::kMuted, 16));
    remove->setIconSize(QSize(16, 16));
    nt::bindTooltip(remove, "downloads.delete");
    nt::bindAccessibleName(remove, "downloads.delete");
    connect(remove, &QPushButton::clicked, this,
            [this, info]() { emit removeRequested(info.videoId); });
    lay->addWidget(remove);

    return row;
}

void DownloadsPage::reload()
{
    // The rows are rebuilt from scratch: they are few (a handful) and this
    // is the simplest way to keep them in sync with the repository.
    while (m_listLayout->count() > 1) {
        QLayoutItem *item = m_listLayout->takeAt(0);
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    const QVector<DownloadInfo> all = m_repo ? m_repo->all()
                                              : QVector<DownloadInfo>();
    for (int i = all.size() - 1; i >= 0; --i)
        m_listLayout->insertWidget(m_listLayout->count() - 1,
                                   buildRow(all.at(i)));

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
