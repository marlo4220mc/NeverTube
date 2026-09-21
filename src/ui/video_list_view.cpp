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

// Fuente reducida sin warnings: si la fuente base no trae point size definido
// (devuelve -1, lo normal si la app usa pixel size), partimos de un tamaño
// lógico por defecto y nunca bajamos de 4pt.
QFont shrunkFont(const QFont &base, qreal delta)
{
    QFont f = base;
    qreal pt = f.pointSizeF();
    if (pt <= 0)
        pt = 9.0;
    f.setPointSizeF(qMax(pt - delta, 4.0));
    return f;
}

// Rectángulo del botón "X" de borrado (modo lista), centrado verticalmente a
// la derecha de la fila. Compartido entre el delegate (paint) y el hit-test de
// clics del view para que coincidan exactamente.
QRectF removeButtonRect(const QRect &cell, qreal scale)
{
    const qreal side = 26 * scale;
    const qreal right = cell.right() - 12 * scale;
    return QRectF(right - side, cell.top() + (cell.height() - side) / 2.0,
                  side, side);
}

// Rectángulo del botón de opciones "⋯", justo a la izquierda de la X.
QRectF moreButtonRect(const QRect &cell, qreal scale)
{
    const qreal side = 26 * scale;
    const QRectF rem = removeButtonRect(cell, scale);
    return QRectF(rem.left() - 8 * scale - side, rem.top(), side, side);
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

    void setScale(qreal scale)
    {
        m_scale = scale;
    }

    // Ancho/alto de celda efectivos cuando la rejilla llena el ancho
    // disponible (0,0 = tamaño base fijo).
    void setCellSize(int w, int h)
    {
        m_cellW = w;
        m_cellH = h;
    }

    void setCurrentRow(int row)
    {
        m_currentRow = row;
    }

    void setShowRemove(bool show)
    {
        m_showRemove = show;
    }

    void setShowMore(bool show)
    {
        m_showMore = show;
    }

    // Mapa (opcional) de fracciones de progreso por videoId que el delegado
    // dibuja como barra al pie de la miniatura en modo rejilla.
    void setProgressMap(const QHash<QString, double> *progress)
    {
        m_progress = progress;
    }

    void drawProgressBar(QPainter *p, const QRectF &thumbR, const VideoInfo &v) const
    {
        if (!m_progress)
            return;
        const auto it = m_progress->constFind(v.id);
        if (it == m_progress->constEnd())
            return;
        const double f = qBound(0.0, it.value(), 1.0);
        if (f <= 0.0)
            return;
        const QRectF bar(thumbR.left() + 2, thumbR.bottom() - 4,
                         thumbR.width() - 4, 3);
        p->save();
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 170));
        p->drawRoundedRect(bar, 1.5, 1.5);
        p->setBrush(nt::kAccent);
        p->drawRoundedRect(QRectF(bar.left(), bar.top(),
                                  bar.width() * f, bar.height()), 1.5, 1.5);
        p->restore();
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

        p->setFont(shrunkFont(p->font(), 2));
        const QFontMetricsF fm(p->font());
        const QString text = QStringLiteral("Reproduciendo");
        const QRectF bg(r.right() - fm.horizontalAdvance(text) - 6,
                        r.bottom() - fm.height() - 7,
                        fm.horizontalAdvance(text) + 6,
                        fm.height() + 3);
        p->setPen(Qt::NoPen);
        p->setBrush(nt::kAccent);
        p->drawRoundedRect(bg, 4, 4);
        QFont bf = p->font();
        bf.setBold(true);
        p->setFont(bf);
        p->setPen(Qt::white);
        p->drawText(bg, Qt::AlignCenter, text);
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        if (m_mode == VideoListView::GridMode) {
            if (m_cellW > 0)
                return QSize(m_cellW, m_cellH);
            return QSize(qRound(kGridW * m_scale), qRound(kGridH * m_scale));
        }
        return QSize(0, qRound(kListH * m_scale));
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
        QFont bf = shrunkFont(f, 1);
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

    // Título de rejilla con salto de línea: si no cabe en una fila, usa una
    // SEGUNDA línea (wrap por palabras) en lugar de cortarse con "…" a media
    // palabra. Solo si aun así no cabe en las dos, se elide la última. La celda
    // ya reserva 34px de título (applyGridGeometry), así que no hay solape.
    static void drawGridTitle(QPainter *p, const QRect &r, const QString &text)
    {
        const QFontMetricsF fm(p->font());
        const qreal w = r.width();
        if (fm.horizontalAdvance(text) <= w) {
            p->drawText(r, Qt::AlignLeft | Qt::AlignTop, text);
            return;
        }

        // Primera línea: se llenan palabras hasta que la siguiente ya no cabe.
        const QStringList words = text.split(QLatin1Char(' '));
        QString first;
        for (const QString &wd : words) {
            const QString cand = first.isEmpty()
                                     ? wd
                                     : first + QLatin1Char(' ') + wd;
            if (fm.horizontalAdvance(cand) <= w)
                first = cand;
            else
                break;
        }
        if (first.isEmpty()) {
            // Una sola palabra más ancha que la celda: elide en línea única.
            p->drawText(r, Qt::AlignLeft | Qt::AlignTop,
                        fm.elidedText(text, Qt::ElideRight, w));
            return;
        }

        p->drawText(r, Qt::AlignLeft | Qt::AlignTop, first);
        QString second = text.mid(first.size());
        while (!second.isEmpty() && second.at(0) == QLatin1Char(' '))
            second.remove(0, 1);
        if (second.isEmpty())
            return;
        const int lh = qRound(fm.height());
        const QRect secondR(r.left(), r.top() + lh, r.width(),
                            qMax(0, r.height() - lh));
        const QString rest = fm.elidedText(second, Qt::ElideRight, w);
        p->drawText(secondR, Qt::AlignLeft | Qt::AlignTop, rest);
    }

private:
    void paintGrid(QPainter *p, const QStyleOptionViewItem &opt,
                   const VideoInfo &v) const
    {
        const QRect r = opt.rect;
        if (opt.state & QStyle::State_MouseOver) {
            p->fillRect(r, nt::kHover);
        }

        const qreal gw = m_cellW > 0 ? m_cellW : kGridW * m_scale;
        const QRectF thumbR(r.left() + 6 * m_scale, r.top() + 6 * m_scale,
                            gw - 12 * m_scale, (gw - 12 * m_scale) * 9.0 / 16.0);
        drawThumb(p, thumbR, m_thumbs->pixmap(cacheKeyFor(v)), v,
                  durationLabel(v.durationSeconds));
        drawProgressBar(p, thumbR, v);

        p->save();
        QFont t = p->font();
        t.setBold(true);
        p->setFont(t);
        p->setPen(nt::kText);
        const QRect titleR(r.left() + 8 * m_scale,
                           qRound(thumbR.bottom()) + 8 * m_scale,
                           qRound(gw - 16 * m_scale), 34);
        drawGridTitle(p, titleR, v.title);
        p->restore();

        p->save();
        QFont m = shrunkFont(p->font(), 0.5);
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
                                           Qt::ElideRight,
                                           qRound(gw - 16 * m_scale));
        p->drawText(QPointF(r.left() + 8 * m_scale, titleR.bottom() + 14),
                    line);
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

        const qreal lh = kListH * m_scale;
        const QRectF thumbR(r.left() + 6 * m_scale,
                            r.top() + (r.height() - 76 * m_scale) / 2.0,
                            136 * m_scale, 76 * m_scale);
        drawThumb(p, thumbR, m_thumbs->pixmap(cacheKeyFor(v)), v,
                  durationLabel(v.durationSeconds));
        drawProgressBar(p, thumbR, v);

        p->save();
        QFont t = p->font();
        t.setBold(true);
        p->setFont(t);
        p->setPen(nt::kText);
        const QRect titleR(r.left() + 152 * m_scale, r.top() + 10 * m_scale,
                           qRound(r.width() - 162 * m_scale), 34);
        elideTitle(p, titleR, v.title);
        p->restore();

        p->save();
        QFont m = shrunkFont(p->font(), 0.5);
        p->setFont(m);
        p->setPen(nt::kMuted);
        QStringList meta;
        if (!v.channel.isEmpty())
            meta << v.channel;
        if (v.viewCount > 0)
            meta << viewsLabel(v.viewCount);
        const QFontMetricsF fm(m);
        const QString line = fm.elidedText(meta.join(QStringLiteral("  •  ")),
                                           Qt::ElideRight,
                                           qRound(r.width() - 170 * m_scale));
        p->drawText(QPointF(r.left() + 152 * m_scale, r.top() + 56 * m_scale),
                    line);
        p->restore();

        if (m_showRemove) {
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            const QRectF xr = removeButtonRect(r, m_scale);
            p->setPen(Qt::NoPen);
            p->setBrush(nt::kHover);
            p->drawEllipse(xr);
            const int is = qRound(15 * m_scale);
            const QPixmap pm =
                nt::icon(nt::Icon::Clear, nt::kMuted, is).pixmap(is, is);
            p->drawPixmap(QPointF(xr.center().x() - is / 2.0,
                                  xr.center().y() - is / 2.0),
                          pm);
            p->restore();
        }

        if (m_showMore) {
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            const QRectF mr = moreButtonRect(r, m_scale);
            const QColor col((opt.state & QStyle::State_MouseOver)
                                 ? nt::kText : nt::kMuted);
            p->setPen(Qt::NoPen);
            p->setBrush(col);
            const qreal cx = mr.center().x();
            const qreal top = mr.center().y() - 5;
            for (int i = 0; i < 3; ++i)
                p->drawEllipse(QPointF(cx, top + i * 5), 2, 2);
            p->restore();
        }
    }

    ThumbnailCache *m_thumbs;
    const QHash<QString, double> *m_progress = nullptr;
    int m_mode = VideoListView::ListMode;
    int m_currentRow = -1;
    bool m_showRemove = false;
    bool m_showMore = false;
    int m_cellW = 0;
    int m_cellH = 0;
    qreal m_scale = 1.0;
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
    delegate->setProgressMap(&m_progress);
    m_mode = ListMode;
    setItemDelegate(delegate);

    connect(m_thumbs, &ThumbnailCache::ready, this,
            &VideoListView::onThumbReady);
}

void VideoListView::setGridFillWidth(int minCellWidth)
{
    m_gridMinCellW = minCellWidth;
    applyGridGeometry();
}

void VideoListView::setGridColumns(int columns)
{
    m_gridColumns = columns;
    applyGridGeometry();
}

int VideoListView::gridColumns() const
{
    if (m_gridColumns > 0)
        return m_gridColumns;
    const int vw = viewport()->width();
    if (m_gridMinCellW > 0 && vw > 0)
        return qMax(1, vw / qMax(1, m_gridMinCellW));
    return qMax(1, vw / qMax(1, kGridW));
}

void VideoListView::applyGridGeometry()
{
    if (m_mode != GridMode)
        return;
    const int vw = viewport()->width();
    if (vw <= 0)
        return;
    const int spacing = 4;
    const int cols = gridColumns();
    const int avail = vw - (cols - 1) * spacing;
    const int cellW = qMax(1, avail / qMax(1, cols));
    // La altura de la celda se deriva del contenido real: miniatura 16:9 +
    // bloque de texto fijo (hueco + título + línea de canal), que no escala
    // con el ancho de la celda. Con un ratio fijo, al estrechar la ventana el
    // texto se sale de la celda y el vídeo siguiente tapa el nombre del canal.
    const int cellH = qRound(qreal(cellW - 12 * m_scale) * 9.0 / 16.0)
                      + qRound(6 * m_scale) + 8 + 34 + 14 + 20;
    setGridSize(QSize(cellW, cellH));
    static_cast<VideosDelegate *>(itemDelegate())->setCellSize(cellW, cellH);
    viewport()->update();
}

void VideoListView::showVideos(const QVector<VideoInfo> &videos, Mode mode)
{
    m_model->clear();
    m_videos = videos;
    m_progress.clear();

    if (mode != m_mode) {
        m_mode = mode;
        static_cast<VideosDelegate *>(itemDelegate())->setMode(mode);
        if (mode == GridMode) {
            setViewMode(QListView::IconMode);
            setGridSize(QSize(qRound(kGridW * m_scale), qRound(kGridH * m_scale)));
            setResizeMode(QListView::Adjust);
            setSpacing(4);
        } else {
            setViewMode(QListView::ListMode);
            setGridSize(QSize());
            setSpacing(0);
        }
    }

    if (mode == GridMode)
        applyGridGeometry();

    for (const VideoInfo &v : m_videos) {
        auto *item = new QStandardItem;
        item->setData(QVariant::fromValue(v), Qt::UserRole);
        item->setSizeHint(mode == GridMode
                              ? QSize(qRound(kGridW * m_scale),
                                      qRound(kGridH * m_scale))
                              : QSize(0, qRound(kListH * m_scale)));
        m_model->appendRow(item);
    }
    refreshThumbnails();
    viewport()->update();
}

void VideoListView::clearVideos()
{
    m_model->clear();
    m_videos.clear();
    m_progress.clear();
    m_currentRow = -1;
    viewport()->update();
}

void VideoListView::setScale(qreal scale)
{
    m_baseScale = scale;
    m_scale = m_baseScale * m_density;
    static_cast<VideosDelegate *>(itemDelegate())->setScale(m_scale);
    if (m_mode == GridMode)
        setGridSize(QSize(qRound(kGridW * m_scale), qRound(kGridH * m_scale)));
    applyGridGeometry();
    updateGeometry();
    viewport()->update();
}

void VideoListView::setDensity(qreal density)
{
    if (density <= 0.0)
        return;
    m_density = density;
    setScale(m_baseScale);
}

void VideoListView::setRowRemoveVisible(bool visible)
{
    if (m_rowRemove == visible)
        return;
    m_rowRemove = visible;
    static_cast<VideosDelegate *>(itemDelegate())->setShowRemove(visible);
    viewport()->update();
}

void VideoListView::setRowMoreVisible(bool visible)
{
    if (m_rowMore == visible)
        return;
    m_rowMore = visible;
    static_cast<VideosDelegate *>(itemDelegate())->setShowMore(visible);
    viewport()->update();
}

void VideoListView::setProgress(const QString &videoId, double fraction)
{
    m_progress.insert(videoId, fraction);
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
        item->setSizeHint(m_mode == GridMode
                              ? QSize(qRound(kGridW * m_scale),
                                      qRound(kGridH * m_scale))
                              : QSize(0, qRound(kListH * m_scale)));
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
        if (idx.isValid() && idx.row() >= 0 && idx.row() < m_videos.size()) {
            // En modo lista con la X activa, el click sobre ella borra en vez
            // de reproducir.
            if (m_rowMore && m_mode == ListMode) {
                const QRect cell = visualRect(idx);
                if (moreButtonRect(cell, m_scale).contains(event->pos())) {
                    emit moreRequested(idx.row(), m_videos.at(idx.row()));
                    return;
                }
            }
            if (m_rowRemove && m_mode == ListMode) {
                const QRect cell = visualRect(idx);
                if (removeButtonRect(cell, m_scale).contains(event->pos())) {
                    emit removeRequested(idx.row(), m_videos.at(idx.row()));
                    return;
                }
            }
            emit videoActivated(idx.row(), m_videos.at(idx.row()));
        }
    }
    QListView::mouseReleaseEvent(event);
}

void VideoListView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    applyGridGeometry();
}