#include "video_list_view.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStandardItemModel>
#include <QStyledItemDelegate>

#include "../i18n.h"
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
// Gap to the left of a list row's text block: thumbnail (136 px) + 6 px
// margin + 10 px breathing room. Shared by the title and the meta line, and
// from it starts the usable width computed by removeButtonRect/moreButtonRect.
const qreal kListTextX = 152;

QString cacheKeyFor(const VideoInfo &v)
{
    return QStringLiteral("v_%1").arg(v.id);
}

// Shrunken font without warnings: if the base font has no point size set
// (returns -1, the usual case when the app uses pixel size), we start from a
// default logical size and never go below 4 pt.
QFont shrunkFont(const QFont &base, qreal delta)
{
    QFont f = base;
    qreal pt = f.pointSizeF();
    if (pt <= 0)
        pt = 9.0;
    f.setPointSizeF(qMax(pt - delta, 4.0));
    return f;
}

// Rectangle of the "X" delete button (list mode), vertically centered on the
// right side of the row. Shared between the delegate (paint) and the view's
// click hit-test so they match exactly.
QRectF removeButtonRect(const QRect &cell, qreal scale)
{
    const qreal side = 26 * scale;
    const qreal right = cell.right() - 12 * scale;
    return QRectF(right - side, cell.top() + (cell.height() - side) / 2.0,
                  side, side);
}

// Rectangle of the options button "⋯". When the row also carries the X it
// goes right to its left; without the X it takes the right edge, like
// Library's options button (a layout widget flush against the edge).
// Reserving the space of an X that is never painted was stealing 34 px of
// text from the row.
QRectF moreButtonRect(const QRect &cell, qreal scale, bool withRemove)
{
    const qreal side = 26 * scale;
    if (withRemove) {
        const QRectF rem = removeButtonRect(cell, scale);
        return QRectF(rem.left() - 8 * scale - side, rem.top(), side, side);
    }
    const qreal right = cell.right() - 12 * scale;
    return QRectF(right - side, cell.top() + (cell.height() - side) / 2.0,
                  side, side);
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
        if (mode != m_mode)
            m_thumbCache.clear(); // the painted thumbnail size changes
        m_mode = mode;
    }

    void setScale(qreal scale)
    {
        if (!qFuzzyCompare(m_scale, scale))
            m_thumbCache.clear(); // the painted thumbnail size changes
        m_scale = scale;
    }

    // Effective cell width/height when the grid fills the available width
    // (0,0 = fixed base size).
    void setCellSize(int w, int h)
    {
        if (w != m_cellW || h != m_cellH)
            m_thumbCache.clear(); // the painted thumbnail size changes
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

    // Optional map of progress fractions per videoId that the delegate
    // draws as a bar at the foot of the thumbnail in grid mode.
    void setProgressMap(const QHash<QString, double> *progress)
    {
        m_progress = progress;
    }

    void drawProgressBar(QPainter *p, const QRectF &thumbR, const VideoInfo &v) const
    {
        if (!m_progress || v.isLive)
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

    // Marks the row that is currently being played: outline + badge.
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
        const QString text = nt::tr("queue.playing");
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

    static void drawBadgeCore(QPainter *p, const QRectF &r, const QString &text,
                              const QColor &bg)
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
        const QRectF box(r.right() - w - 2, r.bottom() - fm.height() - 4,
                        w, fm.height() + 4);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(box, 4, 4);
        p->setPen(Qt::white);
        p->drawText(box, Qt::AlignCenter, text);
        p->restore();
    }

    static void drawBadge(QPainter *p, const QRectF &r, const QString &text)
    {
        drawBadgeCore(p, r, text, QColor(0, 0, 0, 200));
    }

    // Live pill: accent as background (like the player's "EN VIVO" mark and
    // YouTube's red pill) instead of the translucent black used for the
    // duration. While a video is live there is no duration to show: the
    // badge replaces it.
    static void drawLiveBadge(QPainter *p, const QRectF &r)
    {
        drawBadgeCore(p, r, nt::tr("video.liveBadge"), nt::kAccent);
    }

    // Thumbnail scaling cached by target size. `m_thumbs->pixmap()` returns
    // the ORIGINAL from disk (hq720 = 1280x720) and drawThumb used to
    // re-scale it with SmoothTransformation on EVERY paint: ~2.4 ms per frame
    // for 20 visible cells, 70% of the cost of painting the grid, which
    // dragged the Home scroll. With the cache the frame drops to ~1.04 ms and
    // the thumbnail comes out IDENTICAL (checked pixel by pixel in the three
    // "Thumbnail size" settings —0.88/1.0/1.15— and in window widths from 900
    // to 1900 px).
    //
    // The key includes the video's key: without it, the entry for one size
    // would be reused by the video painted next and ALL thumbnails would come
    // out the same. The target size is the one the cell sets, so changing the
    // window width or the setting only invalidates what no longer applies
    // (m_thumbCache is cleared in setScale/setCellSize/setMode, the only
    // points where the size changes). It doesn't depend on the network: if
    // the source pixmap hasn't arrived yet, there is nothing to cache and it
    // gets scaled once when it does.
    static inline QString claveEscalado(const QString &key, const QSize &s)
    {
        return key + QLatin1Char('@') + QString::number(s.width())
               + QLatin1Char('x') + QString::number(s.height());
    }

    const QPixmap &thumbEscalada(const QString &key, const QSize &destino,
                                const QPixmap &orig) const
    {
        // It is cleared in setScale/setCellSize when the size changes, so
        // here it only grows with the new videos of a same grid/list.
        if (m_thumbCache.size() > 64)
            m_thumbCache.clear(); // defense: should never trigger
        const QString k = claveEscalado(key, destino);
        const auto it = m_thumbCache.constFind(k);
        if (it != m_thumbCache.constEnd())
            return it.value();
        return m_thumbCache.insert(k, orig.scaled(destino,
                                                  Qt::KeepAspectRatioByExpanding,
                                                  Qt::SmoothTransformation))
                   .value();
    }

    void drawThumb(QPainter *p, const QRectF &r, const VideoInfo &v) const
    {
        const QString key = cacheKeyFor(v);
        const QPixmap pm = m_thumbs->pixmap(key);
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
            const QSize destino = r.size().toSize();
            if (destino.width() < 1 || destino.height() < 1) {
                p->restore();
                return;
            }
            const QPixmap &scaled = thumbEscalada(key, destino, pm);
            const QRectF src((scaled.width() - r.width()) / 2.0,
                             (scaled.height() - r.height()) / 2.0, r.width(),
                             r.height());
            p->drawPixmap(r, scaled, src);
        }
        p->restore();
        if (v.isPlaylist()) {
            const QString badge = v.playlistVideos > 0
                ? QString::number(v.playlistVideos)
                : nt::tr("fmt.playlist");
            drawBadge(p, r, badge);
        } else if (v.isLive)
            drawLiveBadge(p, r);
        else if (v.durationSeconds > 0)
            drawBadge(p, r, durationLabel(v.durationSeconds));
    }

    // Lays out the text in `lines` lines with greedy word wrap; the LAST one
    // carries everything left from the start of that line, elided. Used by
    // both PAINTING (drawGridTitle) and GEOMETRY (paintList decides with
    // size() how many lines the title takes to stick the meta right below
    // it): a single function, so they cannot diverge.
    static QStringList wrapTitle(const QFontMetricsF &fm, const QString &text,
                                 qreal w, int lines)
    {
        QStringList out;
        lines = qMax(1, lines);
        if (w <= 0)
            return out;
        if (fm.horizontalAdvance(text) <= w) {
            out << text;
            return out;
        }
        const QStringList words =
            text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.isEmpty())
            return out;
        int i = 0;
        for (int ln = 0; ln < lines; ++ln) {
            const int start = i;
            QString line;
            while (i < words.size()) {
                const QString cand = line.isEmpty()
                                         ? words.at(i)
                                         : line + QLatin1Char(' ') + words.at(i);
                // Once the line has started, a word that doesn't fit moves to
                // the next line. With an empty line the word is accepted even
                // if it doesn't fit (a single word wider than the cell),
                // otherwise the loop would never advance.
                if (fm.horizontalAdvance(cand) > w && !line.isEmpty())
                    break;
                line = cand;
                ++i;
            }
            if (ln == lines - 1) {
                QString rest;
                for (int k = start; k < words.size(); ++k) {
                    if (!rest.isEmpty())
                        rest += QLatin1Char(' ');
                    rest += words.at(k);
                }
                out << fm.elidedText(rest, Qt::ElideRight, w);
                return out;
            }
            out << line;
            if (i >= words.size())
                return out;
        }
        return out;
    }

    // Title with line wrapping: if it doesn't fit on one row it uses the
    // following ones (greedy word wrap) instead of being cut with "…" mid
    // word. On the LAST line goes everything left from the start of that
    // line, elided, so the beginning of the rest is readable instead of a
    // hard cut. The cell reserves the box (applyGridGeometry uses the same
    // gridTextBlockH), so there is no overlap with the channel line.
    static void drawGridTitle(QPainter *p, const QRect &r, const QString &text,
                              int lineH, int lines)
    {
        const QFontMetricsF fm(p->font());
        const QStringList ls = wrapTitle(fm, text, r.width(), lines);
        for (int ln = 0; ln < ls.size(); ++ln) {
            const QRect lr(r.left(), r.top() + ln * lineH, r.width(), lineH);
            p->drawText(lr, Qt::AlignLeft | Qt::AlignTop, ls.at(ln));
        }
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
        // The key is passed, not the pixmap: the scaling is cached by
        // (video, size), so the key is needed to look it up.
        drawThumb(p, thumbR, v);
        drawProgressBar(p, thumbR, v);

        // Title and channel line at the size ruled by THIS cell (see
        // gridTitlePx): 12 px in a small cell, 16 px in a wide one. The line
        // height is set by the same pure function the cell uses, so the
        // second line falls inside the reserved box. In BOLD: it was tried
        // without it and the user asked to bring it back (with the font
        // already 1 px smaller the title stands out from the channel line).
        const int titlePx = VideoListView::gridTitlePx(qRound(gw));
        const int lineH = VideoListView::gridTitleLineH(qRound(gw));

        p->save();
        QFont t = p->font();
        t.setPixelSize(titlePx);
        t.setBold(true);
        p->setFont(t);
        p->setPen(nt::kText);
        const QRect titleR(r.left() + 8 * m_scale,
                           qRound(thumbR.bottom()) + 8 * m_scale,
                           qRound(gw - 16 * m_scale),
                           VideoListView::kGridTitleLines * lineH);
        drawGridTitle(p, titleR, v.title, lineH, VideoListView::kGridTitleLines);
        p->restore();

        p->save();
        QFont m = p->font();
        // The Home channel/views/date line is a little smaller than the title
        // (2 px less, floor of 10): the user asked for it to be more discreet
        // than the title's, and at 1 px less it still stood out as much as it.
        m.setPixelSize(qMax(10, titlePx - 2));
        p->setFont(m);
        p->setPen(nt::kMuted);
        const QFontMetricsF fm(m);
        p->drawText(QPointF(r.left() + 8 * m_scale, titleR.bottom() + 14),
                    fm.elidedText(metaLine(v), Qt::ElideRight,
                                  qRound(gw - 16 * m_scale)));
        p->restore();
    }

    // Metrics of a list row's text block. The typography is NOT touched: the
    // one the lists already used (title in the app font and bold, reduced
    // meta). The only thing unified with Library is the row's SHAPE
    // (thumbnail, channel • views • date and options button), not the font
    // size nor the boldness.
    //
    // QFontMetricsF has no default constructor, so here go the measurements
    // (height and ascent) rather than the metric itself: whoever paints
    // creates its own with the same font.
    struct ListTextMetrics
    {
        QFont title;
        QFont meta;
        qreal lineH = 0;      // title line height
        qreal metaH = 0;      // meta line height
        qreal metaAscent = 0; // meta ascent (for its baseline)
        qreal gap = 0;        // gap between title and meta
    };

    ListTextMetrics listMetrics(const QFont &base) const
    {
        ListTextMetrics m;
        m.title = base;
        m.title.setBold(true);
        m.lineH = qMax(1.0, QFontMetricsF(m.title).height());
        m.meta = shrunkFont(base, 0.5);
        const QFontMetricsF metaFm(m.meta);
        m.metaH = metaFm.height();
        m.metaAscent = metaFm.ascent();
        // Gap between the title and the meta: 6 px, so they don't read as a
        // single line of text.
        m.gap = 6 * m_scale;
        return m;
    }

    // Meta of the list row: ONE line "channel • views • date" with the same
    // format::metaLine function used by the Home grid and the Library
    // sections. This row used to paint only "channel • views" at 11.3 px: the
    // same video read differently depending on the page.
    void paintListMeta(QPainter *p, const QRect &r, const VideoInfo &v,
                       const ListTextMetrics &m, qreal blockTop,
                       int titleLines) const
    {
        const qreal x = r.left() + kListTextX * m_scale;
        const qreal avail = textWidth(r, kListTextX * m_scale);
        if (avail < 20)
            return;
        p->save();
        p->setFont(m.meta);
        p->setPen(nt::kMuted);
        const QFontMetricsF fm(m.meta);
        p->drawText(QPointF(x, blockTop + m.lineH * titleLines + m.gap
                                      + m.metaAscent),
                    fm.elidedText(metaLine(v), Qt::ElideRight,
                                  qRound(avail)));
        p->restore();
    }

    // Usable width of the row's text: the cell's minus the thumbnail (on the
    // left) and the buttons on the right, with the SAME arithmetic as
    // removeButtonRect/moreButtonRect, so the title and the meta never end up
    // beneath them. That's what Library's row does, where the button is a
    // layout widget and the text cannot invade its space.
    //
    // The arithmetic is the one of the rectangles above: 12 px of margin and,
    // if there are buttons, their 26 px (plus the 8 of separation when both
    // are shown). Measured: without discounting the button, the title reached
    // x=839 with the ⋮ dots at 838-841, i.e. the text ran underneath.
    qreal textWidth(const QRect &r, qreal leftGutter) const
    {
        qreal right = 12 * m_scale; // edge margin
        if (m_showMore)
            right += 26 * m_scale; // options button space
        if (m_showMore && m_showRemove)
            right += 8 * m_scale; // separation of the two buttons
        if (m_showRemove)
            right += 26 * m_scale;
        return r.width() - leftGutter - right;
    }

    void paintList(QPainter *p, const QStyleOptionViewItem &opt,
                   const VideoInfo &v, int row) const
    {
        const QRect r = opt.rect;
        // Rounded hover with 2 px of breathing room: it reads as a highlighted
        // card and not as a full-bleed color strip (this is what Search's
        // enriched row already had and what best matches the Library sections,
        // whose row also sits on a card).
        if (opt.state & QStyle::State_MouseOver) {
            p->save();
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(nt::kHover);
            p->drawRoundedRect(QRectF(r).adjusted(2, 2, -2, -2), 8, 8);
            p->restore();
        }
        if (row == m_currentRow)
            drawCurrentMark(p, r);

        const QRectF thumbR(r.left() + 6 * m_scale,
                            r.top() + (r.height() - 76 * m_scale) / 2.0,
                            136 * m_scale, 76 * m_scale);
        // The key is passed, not the pixmap: the scaling is cached by
        // (video, size), so the key is needed to look it up.
        drawThumb(p, thumbR, v);
        drawProgressBar(p, thumbR, v);

        // Title: the app font in bold (the one the row already had) and 2
        // lines, like Library's row. The line spacing is the font's natural
        // one and it is 2 lines (not the grid's 3): the list row has a fixed
        // height.
        const ListTextMetrics m = listMetrics(p->font());
        const qreal titleW = textWidth(r, kListTextX * m_scale);
        // Lines the title ACTUALLY occupies (1 or 2). The block is centered
        // with that height and the meta goes stuck to its last line: before,
        // 2 lines were always reserved, so with a one-line title the meta
        // ended a whole row lower (the user asked for it stuck to the title).
        const int titleLines = titleW >= 20
            ? qMax(1, wrapTitle(QFontMetricsF(m.title), v.title, titleW, 2).size())
            : 1;
        const qreal blockH = titleLines * m.lineH + m.gap + m.metaH;
        const qreal blockTop = r.top() + (r.height() - blockH) / 2.0;
        p->save();
        p->setFont(m.title);
        p->setPen(nt::kText);
        const QRect titleR(r.left() + qRound(kListTextX * m_scale),
                           qRound(blockTop), qRound(titleW),
                           qRound(titleLines * m.lineH));
        if (titleW >= 20)
            drawGridTitle(p, titleR, v.title, m.lineH, 2);
        p->restore();

        paintListMeta(p, r, v, m, blockTop, titleLines);

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
            const QRectF mr = moreButtonRect(r, m_scale, m_showRemove);
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
    // Thumbnails already scaled to the cell size, by (video key, size).
    // mutable because drawThumb/paint are const: the cache is a painting
    // effect, not view state. Cleared in setScale/setCellSize/setMode.
    mutable QHash<QString, QPixmap> m_thumbCache;
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

int VideoListView::clampGridColumns(int wanted, int viewportW, int minCellW)
{
    if (wanted <= 0)
        return 1;
    // With no width yet (initial layout) there is no clamp to apply: Home's
    // resizeEvent will adjust it as soon as there is one.
    if (viewportW <= 0 || minCellW <= 0)
        return wanted;
    return qMax(1, qMin(wanted, qMax(3, viewportW / minCellW)));
}

int VideoListView::gridTitlePx(int cellW)
{
    if (cellW <= 0)
        return 12;
    // 140 px cell -> 12 px, 340 px -> 16 px (5 px per 200 px of cell). The
    // bounds matter in both directions: below ~150 px the title must not drop
    // under 12 px (at 11 px it stops being readable) and above ~320 px it
    // doesn't have to keep growing. This whole range dropped 1 px (and the
    // ceiling 18 -> 16) because at 14 px bold the user saw the title too big.
    return qBound(12, qRound(12.0 + (cellW - 140) * 0.025), 16);
}

int VideoListView::gridTitleLineH(int cellW)
{
    return qMax(1, qRound(gridTitlePx(cellW) * 1.28));
}

int VideoListView::gridTextBlockH(int cellW)
{
    // kGridTitleLines title lines + 14 px up to the channel line + 20 px of
    // foot. The cell reserves exactly this and painting draws into a
    // kGridTitleLines*lineH box, so the text cannot escape the cell.
    return kGridTitleLines * gridTitleLineH(cellW) + 14 + 20;
}

void VideoListView::applyGridGeometry()
{
    if (m_mode != GridMode)
        return;
    const int vw = viewport()->width();
    if (vw <= 0)
        return;
    const int spacing = kGridSpacing;
    const int cols = gridColumns();
    const int avail = vw - (cols - 1) * spacing;
    const int cellW = qMax(1, avail / qMax(1, cols));
    // The cell height derives from the real content: 16:9 thumbnail + text
    // block (gap + title + channel line). The block is computed with the SAME
    // pure function painting uses (gridTextBlockH), so the cell really
    // reserves what the title takes and there is never overlap. With a fixed
    // ratio, narrowing the window pushes the text out of the cell and the
    // next video covers the channel name.
    const int cellH = qRound(qreal(cellW - 12 * m_scale) * 9.0 / 16.0)
                      + qRound(6 * m_scale) + 8 + gridTextBlockH(cellW);
    setGridSize(QSize(cellW, cellH));
    // In IconMode the cell size is ruled by the MODEL's sizeHint, not
    // gridSize(): leaving the fixed kGridH (258) in the sizeHint while here a
    // different cellH was computed, rows took 258 while the page sized the
    // section with this cellH, so the section came out shorter than the
    // content and the LAST row was not visible (nor the video's channel line
    // with it). It is written here so sizeHint, gridSize, delegate and section
    // height all come out of the same calculation.
    const QSize cell(cellW, cellH);
    if (cell != m_gridCell) {
        m_gridCell = cell;
        for (int r = 0; r < m_model->rowCount(); ++r)
            if (QStandardItem *it = m_model->item(r))
                it->setSizeHint(cell);
    }
    static_cast<VideosDelegate *>(itemDelegate())->setCellSize(cellW, cellH);
    viewport()->update();
}

QRect VideoListView::gridItemRect(int row) const
{
    // visualRect (public) returns the rect of the item ALREADY PLACED by the
    // layout, which is what has to be compared against the section height.
    return visualRect(m_model->index(row, 0));
}

QSize VideoListView::gridItemSize() const
{
    return m_gridCell.isValid()
               ? m_gridCell
               : QSize(qRound(kGridW * m_scale), qRound(kGridH * m_scale));
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
            setSpacing(kGridSpacing);
        } else {
            setViewMode(QListView::ListMode);
            setGridSize(QSize());
            setSpacing(0);
        }
    }

    for (const VideoInfo &v : m_videos) {
        auto *item = new QStandardItem;
        item->setData(QVariant::fromValue(v), Qt::UserRole);
        item->setSizeHint(mode == GridMode
                              ? gridItemSize()
                              : QSize(0, qRound(kListH * m_scale)));
        m_model->appendRow(item);
    }
    // applyGridGeometry writes the items' sizeHint with the REAL cell size,
    // so it is called AFTER creating them: if called before (with the model
    // already cleared) it would have no items to fix.
    if (mode == GridMode)
        applyGridGeometry();
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
                              ? gridItemSize()
                              : QSize(0, qRound(kListH * m_scale)));
        m_model->appendRow(item);
    }
    m_videos += videos;
    // For the same reason as in showVideos: the grid's sizeHint is set by
    // applyGridGeometry with the real cell size, and although the new items
    // are already born with it (gridItemSize) the geometry may have changed.
    if (m_mode == GridMode)
        applyGridGeometry();
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
        if (url.isEmpty())
            url = fallbackThumbnailUrl(v.id);
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
            // In list mode with the X enabled, clicking on it deletes instead
            // of playing.
            if (m_rowMore && m_mode == ListMode) {
                const QRect cell = visualRect(idx);
                if (moreButtonRect(cell, m_scale, m_rowRemove)
                        .contains(event->pos())) {
                    const QPoint pos = mapToGlobal(
                        moreButtonRect(cell, m_scale, m_rowRemove)
                            .bottomLeft().toPoint());
                    emit moreRequested(idx.row(), m_videos.at(idx.row()), pos);
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