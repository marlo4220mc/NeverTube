#include "video_list_view.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStandardItemModel>
#include <QStyledItemDelegate>

#include "../models.h"

Q_DECLARE_METATYPE(VideoInfo)
#include "format.h"
#include "icons.h"
#include "theme.h"
#include "thumbnail_cache.h"

namespace {

const int kGridW = 320;
const int kGridH = 258;
const int kListH = 86;

QString cacheKeyFor(const VideoInfo &v)
{
    return QStringLiteral("v_%1").arg(v.id);
}

class VideosDelegate : public QStyledItemDelegate
{
public:
    explicit VideosDelegate(ThumbnailCache *thumbs, QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_thumbs(thumbs)
    {
    }

    void setMode(int mode)
    {
        m_mode = mode;
    }

    void setCurrentRow(int row)
    {
        m_currentRow = row;
    }

    // Marca la fila que está siendo reproducida: recuadro + insignia.
    void drawCurrentMark(QPainter *p, const QRect &r) const
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QPen pen(nt::kAccent);
        pen.setWidth(2);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(r).adjusted(1.5, 1.5, -1.5, -1.5), 6, 6);

        QFont f = p->font();
        f.setPointSizeF(f.pointSizeF() - 1);
        f.setBold(true);
        p->setFont(f);
        const QFontMetricsF fm(f);
        const QString text = QStringLiteral("Reproduciendo");
        const QRectF bg(r.right() - fm.horizontalAdvance(text) - 14,
                        r.top() + 8, fm.horizontalAdvance(text) + 10,
                        fm.height() + 4);
        p->setPen(Qt::NoPen);
        p->setBrush(nt::kAccent);
        p->drawRoundedRect(bg, 4, 4);
        p->setPen(Qt::white);
        p->drawText(bg, Qt::AlignCenter, text);
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return m_mode == VideoListView::GridMode
            ? QSize(kGridW, kGridH)
            : QSize(0, kListH);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt,
               const QModelIndex &index) const override
    {
        const auto v = index.data(Qt::UserRole).value<VideoInfo>();
        if (m_mode == VideoListView::GridMode)
            paintGrid(p, opt, v);
        else
            paintList(p, opt, v, index.row());
    }

    static void drawBadge(QPainter *p, const QRectF &r, const QString &text)
    {
        if (text.isEmpty())
            return;
        const QFont f = p->font();
        QFont bf = f;
        bf.setPointSizeF(f.pointSizeF() - 1);
        bf.setBold(true);
        p->save();
        p->setFont(bf);
        const QFontMetricsF fm(bf);
        const qreal w = fm.horizontalAdvance(text) + 10;
        const QRectF bg(r.right() - w - 2, r.bottom() - fm.height() - 4,
                        w, fm.height() + 4);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 200));
        p->drawRoundedRect(bg, 4, 4);
        p->setPen(Qt::white);
        p->drawText(bg, Qt::AlignCenter, text);
        p->restore();
    }

    static void drawThumb(QPainter *p, const QRectF &r, const QPixmap &pm,
                          const VideoInfo &v, const QString &duration)
    {
        QPainterPath clip;
        clip.addRoundedRect(r, 8, 8);
        p->save();
        p->setClipPath(clip);
        if (pm.isNull()) {
            p->fillRect(r, nt::kElevated);
            const QPixmap play = nt::icon(nt::Icon::Play, nt::kMuted, 28)
                                     .pixmap(28, 28);
            p->drawPixmap(QPointF(r.center().x() - 14, r.center().y() - 14),
                          play);
        } else {
            const QPixmap scaled = pm.scaled(r.size().toSize(),
                                             Qt::KeepAspectRatioByExpanding,
                                             Qt::SmoothTransformation);
            const QRectF src((scaled.width() - r.width()) / 2.0,
                             (scaled.height() - r.height()) / 2.0, r.width(),
                             r.height());
            p->drawPixmap(r, scaled, src);
        }
        p->restore();
        if (!duration.isEmpty())
            drawBadge(p, r, duration);
    }

    static void elideTitle(QPainter *p, const QRect &r, const QString &text)
    {
        const QFontMetricsF fm(p->font());
        const QString first = fm.elidedText(text, Qt::ElideRight,
                                            r.width(), Qt::TextWordWrap);
        p->drawText(r, Qt::TextWordWrap | Qt::AlignTop, first);
    }

private:
    void paintGrid(QPainter *p, const QStyleOptionViewItem &opt,
                   const VideoInfo &v) const
    {
        const QRect r = opt.rect;
        if (opt.state & QStyle::State_MouseOver) {
            p->fillRect(r, nt::kHover);
        }

        const QRectF thumbR(r.left() + 6, r.top() + 6, kGridW - 12,
                            (kGridW - 12) * 9.0 / 16.0);
        drawThumb(p, thumbR, m_thumbs->pixmap(cacheKeyFor(v)), v,
                  durationLabel(v.durationSeconds));

        p->save();
        QFont t = p->font();
        t.setBold(true);
        p->setFont(t);
        p->setPen(nt::kText);
        const QRect titleR(r.left() + 8, qRound(thumbR.bottom()) + 8,
                           kGridW - 16, 34);
        elideTitle(p, titleR, v.title);
        p->restore();

        p->save();
        QFont m = p->font();
        m.setPointSizeF(m.pointSizeF() - 0.5);
        p->setFont(m);
        p->setPen(nt::kMuted);
        QStringList meta;
        if (!v.channel.isEmpty())
            meta << v.channel;
        if (v.viewCount > 0)
            meta << viewsLabel(v.viewCount);
        const QString pub = publishedLabel(v.publishedAt);
        if (!pub.isEmpty())
            meta << pub;
        const QFontMetricsF fm(m);
        const QString line = fm.elidedText(meta.join(QStringLiteral("  •  ")),
                                           Qt::ElideRight, kGridW - 16);
        p->drawText(QPointF(r.left() + 8, titleR.bottom() + 14), line);
        p->restore();
    }

    void paintList(QPainter *p, const QStyleOptionViewItem &opt,
                   const VideoInfo &v, int row) const
    {
        const QRect r = opt.rect;
        if (opt.state & QStyle::State_MouseOver)
            p->fillRect(r, nt::kHover);
        if (row == m_currentRow)
            drawCurrentMark(p, r);

        const QRectF thumbR(r.left() + 6, r.top() + (r.height() - 76) / 2.0,
                            136, 76);
        drawThumb(p, thumbR, m_thumbs->pixmap(cacheKeyFor(v)), v,
                  durationLabel(v.durationSeconds));

        p->save();
        QFont t = p->font();
        t.setBold(true);
        p->setFont(t);
        p->setPen(nt::kText);
        const QRect titleR(r.left() + 152, r.top() + 10, r.width() - 162, 34);
        elideTitle(p, titleR, v.title);
        p->restore();

        p->save();
        QFont m = p->font();
        m.setPointSizeF(m.pointSizeF() - 0.5);
        p->setFont(m);
        p->setPen(nt::kMuted);
        QStringList meta;
        if (!v.channel.isEmpty())
            meta << v.channel;
        if (v.viewCount > 0)
            meta << viewsLabel(v.viewCount);
        const QFontMetricsF fm(m);
        const QString line = fm.elidedText(meta.join(QStringLiteral("  •  ")),
                                           Qt::ElideRight, r.width() - 170);
        p->drawText(QPointF(r.left() + 152, r.top() + 56), line);
        p->restore();
    }

    ThumbnailCache *m_thumbs;
    int m_mode = VideoListView::ListMode;
    int m_currentRow = -1;
};

} // namespace

VideoListView::VideoListView(ThumbnailCache *thumbs, QWidget *parent)
    : QListView(parent)
    , m_thumbs(thumbs)
{
    setObjectName(QStringLiteral("cards"));
    m_model = new QStandardItemModel(this);
    setModel(m_model);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setUniformItemSizes(false);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setViewMode(QListView::ListMode);

    auto *delegate = new VideosDelegate(thumbs, this);
    delegate->setMode(ListMode);
    m_mode = ListMode;
    setItemDelegate(delegate);

    connect(m_thumbs, &ThumbnailCache::ready, this,
            &VideoListView::onThumbReady);
}

void VideoListView::showVideos(const QVector<VideoInfo> &videos, Mode mode)
{
    m_model->clear();
    m_videos = videos;

    if (mode != m_mode) {
        m_mode = mode;
        static_cast<VideosDelegate *>(itemDelegate())->setMode(mode);
        if (mode == GridMode) {
            setViewMode(QListView::IconMode);
            setGridSize(QSize(kGridW, kGridH));
            setResizeMode(QListView::Adjust);
            setSpacing(4);
        } else {
            setViewMode(QListView::ListMode);
            setGridSize(QSize());
            setSpacing(0);
        }
    }

    for (const VideoInfo &v : m_videos) {
        auto *item = new QStandardItem;
        item->setData(QVariant::fromValue(v), Qt::UserRole);
        item->setSizeHint(mode == GridMode ? QSize(kGridW, kGridH)
                           : QSize(0, kListH));
        m_model->appendRow(item);
    }
    refreshThumbnails();
    viewport()->update();
}

void VideoListView::clearVideos()
{
    m_model->clear();
    m_videos.clear();
    m_currentRow = -1;
    viewport()->update();
}

void VideoListView::setCurrentRow(int row)
{
    if (row < 0 || row >= m_videos.size())
        row = -1;
    if (row == m_currentRow)
        return;
    m_currentRow = row;
    static_cast<VideosDelegate *>(itemDelegate())->setCurrentRow(row);
    viewport()->update();
}

void VideoListView::appendVideos(const QVector<VideoInfo> &videos)
{
    for (const VideoInfo &v : videos) {
        auto *item = new QStandardItem;
        item->setData(QVariant::fromValue(v), Qt::UserRole);
        item->setSizeHint(m_mode == GridMode ? QSize(kGridW, kGridH)
                           : QSize(0, kListH));
        m_model->appendRow(item);
    }
    m_videos += videos;
    refreshThumbnails();
    viewport()->update();
}

bool VideoListView::isEmpty() const
{
    return m_videos.isEmpty();
}

QVector<VideoInfo> VideoListView::videos() const
{
    return m_videos;
}

void VideoListView::refreshThumbnails()
{
    for (const VideoInfo &v : m_videos) {
        QString url = v.thumbnailUrl;
        if (url.isEmpty() && v.id.size() == 11)
            url = QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(v.id);
        m_thumbs->request(cacheKeyFor(v), url);
    }
}

void VideoListView::onThumbReady(const QString &key)
{
    if (key.startsWith(QLatin1String("v_")))
        viewport()->update();
}

void VideoListView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QModelIndex idx = indexAt(event->pos());
        if (idx.isValid() && idx.row() >= 0 && idx.row() < m_videos.size())
            emit videoActivated(idx.row(), m_videos.at(idx.row()));
    }
    QListView::mouseReleaseEvent(event);
}