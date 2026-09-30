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
        if (mode != m_mode)
            m_thumbCache.clear(); // cambia el tamaño pintado de la miniatura
        m_mode = mode;
    }

    void setScale(qreal scale)
    {
        if (!qFuzzyCompare(m_scale, scale))
            m_thumbCache.clear(); // cambia el tamaño pintado de la miniatura
        m_scale = scale;
    }

    // Ancho/alto de celda efectivos cuando la rejilla llena el ancho
    // disponible (0,0 = tamaño base fijo).
    void setCellSize(int w, int h)
    {
        if (w != m_cellW || h != m_cellH)
            m_thumbCache.clear(); // cambia el tamaño pintado de la miniatura
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

    // 1 = bloque de meta clásico; 2 = bloque enriquecido de Search (ver
    // VideoListView::setMetaLines). Solo afecta al modo lista.
    void setMetaLines(int lines)
    {
        m_metaLines = qBound(1, lines, 2);
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

    // Escalado de miniatura cacheado por tamaño de destino. `m_thumbs->pixmap()`
    // devuelve el ORIGINAL de disco (hq720 = 1280x720) y drawThumb lo reescalaba
    // con SmoothTransformation en CADA paint: ~2,4 ms por frame de 20 celdas
    // visibles, el 70 % del coste de pintar la rejilla, lo que tironaba el
    // scroll del Home. Con la cache el frame baja a ~1,04 ms y la miniatura
    // sale IDENTICA (comprobado pixel a pixel en los tres ajustes de "Tamaño de
    // las miniaturas" —0,88/1,0/1,15— y en anchos de ventana de 900 a 1900 px).
    //
    // La clave incluye la key del video: sin eso, la entrada de un tamaño
    // seria reutilizada por el video pintado despues y TODAS las miniaturas
    // saldrian iguales. El tamaño de destino es el que fija la celda, asi que
    // cambiar el ancho de ventana o el ajuste solo invalida lo que ya no
    // sirve (m_thumbCache se vacia en setScale/setCellSize/setMode, los
    // unicos puntos donde cambia el tamaño). No depende de la red: si el
    // pixmap de origen aun no llego, no hay nada que cachear y al llegar se
    // escala una vez.
    static inline QString claveEscalado(const QString &key, const QSize &s)
    {
        return key + QLatin1Char('@') + QString::number(s.width())
               + QLatin1Char('x') + QString::number(s.height());
    }

    const QPixmap &thumbEscalada(const QString &key, const QSize &destino,
                                const QPixmap &orig) const
    {
        // Se vacia en setScale/setCellSize cuando cambia el tamaño, asi que
        // aqui solo crece con los videos nuevos de una misma rejilla/lista.
        if (m_thumbCache.size() > 64)
            m_thumbCache.clear(); // defensa: no deberia lanzarse nunca
        const QString k = claveEscalado(key, destino);
        const auto it = m_thumbCache.constFind(k);
        if (it != m_thumbCache.constEnd())
            return it.value();
        return m_thumbCache.insert(k, orig.scaled(destino,
                                                  Qt::KeepAspectRatioByExpanding,
                                                  Qt::SmoothTransformation))
                   .value();
    }

    void drawThumb(QPainter *p, const QRectF &r, const QString &key,
                   const QString &duration) const
    {
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

    // Título con salto de línea: si no cabe en una fila, usa las siguientes
    // (wrap voraz por palabras) en lugar de cortarse con "…" a media palabra. En
    // la ÚLTIMA línea entra todo lo que queda desde el principio de esa línea,
    // elidado, para que se lea el arranque del resto y no un corte en seco. La
    // caja la reserva la celda (applyGridGeometry usa la misma
    // gridTextBlockH), así que no hay solape con la línea de canal.
    static void drawGridTitle(QPainter *p, const QRect &r, const QString &text,
                              int lineH, int lines)
    {
        const QFontMetricsF fm(p->font());
        const qreal w = r.width();
        lines = qMax(1, lines);
        if (fm.horizontalAdvance(text) <= w) {
            p->drawText(r, Qt::AlignLeft | Qt::AlignTop, text);
            return;
        }
        const QStringList words =
            text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (words.isEmpty())
            return;
        int i = 0;
        for (int ln = 0; ln < lines; ++ln) {
            const int start = i;
            QString line;
            while (i < words.size()) {
                const QString cand = line.isEmpty()
                                         ? words.at(i)
                                         : line + QLatin1Char(' ') + words.at(i);
                // Con la línea ya empezado no entra: se pasa a la siguiente. Con
                // la línea vacía se acepta la palabra aunque no quepa (una sola
                // palabra más ancha que la celda), si no el bucle no avanzaría.
                if (fm.horizontalAdvance(cand) > w && !line.isEmpty())
                    break;
                line = cand;
                ++i;
            }
            const QRect lr(r.left(), r.top() + ln * lineH, r.width(), lineH);
            if (ln == lines - 1) {
                QString rest;
                for (int k = start; k < words.size(); ++k) {
                    if (!rest.isEmpty())
                        rest += QLatin1Char(' ');
                    rest += words.at(k);
                }
                p->drawText(lr, Qt::AlignLeft | Qt::AlignTop,
                            fm.elidedText(rest, Qt::ElideRight, w));
                return;
            }
            p->drawText(lr, Qt::AlignLeft | Qt::AlignTop, line);
            if (i >= words.size())
                return;
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
        // Se pasa la clave, no el pixmap: el escalado va cacheado por
        // (video, tamano), asi que hace falta la clave para consultarlo.
        drawThumb(p, thumbR, cacheKeyFor(v), durationLabel(v.durationSeconds));
        drawProgressBar(p, thumbR, v);

        // Título y línea de canal al tamaño que manda en ESTA celda (ver
        // gridTitlePx): 13 px en celda pequeña, 18 px en celda ancha. Antes eran
        // 13 px fijos con la miniatura creciendo, y el bloque de texto se
        // quedaba corto. El interlineado lo fija la misma función pura que usa
        // la celda, así que la segunda línea cae dentro de la caja reservada.
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
        m.setPixelSize(qMax(10, titlePx - 1));
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

    // Meta clásica de la fila de lista: una sola línea "canal • vistas" a
    // 11,3 px bajo el título. Es el aspecto del resto de páginas.
    void paintPlainMeta(QPainter *p, const QRect &r, const VideoInfo &v) const
    {
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
    }

    // Meta enriquecida de Search: la fila de 86 px deja ~28 px libres bajo la
    // línea de meta clásica (el título se pintaba en una sola línea dentro de
    // una caja de 34), así que aquí se aprovechan con "vistas • fecha" y el
    // canal (avatar + nombre) en su propia línea. La fila NO crece: no se gana
    // scroll, se gana información por fila.
    void paintRichMeta(QPainter *p, const QRect &r, const VideoInfo &v) const
    {
        const qreal x = r.left() + 152 * m_scale;
        const qreal w = qRound(r.width() - 162 * m_scale);

        p->save();
        QFont m = shrunkFont(p->font(), 0.5);
        p->setFont(m);
        p->setPen(nt::kMuted);
        QStringList meta;
        if (v.viewCount > 0)
            meta << viewsLabel(v.viewCount);
        const QString pub = publishedLabel(v.publishedAt);
        if (!pub.isEmpty())
            meta << pub;
        if (!meta.isEmpty()) {
            const QFontMetricsF fm(m);
            p->drawText(QPointF(x, r.top() + 57 * m_scale),
                        fm.elidedText(meta.join(QStringLiteral("  •  ")),
                                      Qt::ElideRight, qRound(w)));
        }
        p->restore();

        if (v.channel.isEmpty())
            return;
        // Canal en su línea, con avatar si ya está en la caché (lo pide la
        // sección "Canales" de la página); si no llegó, disco neutro: el
        // pintado nunca dispara red.
        const qreal av = qRound(14 * m_scale);
        const QRectF avR(x, r.top() + 64 * m_scale, av, av);
        drawChannelAvatar(p, avR, v.channelId);
        p->save();
        QFont cf = p->font();
        cf.setPixelSize(qMax(10, qRound(11 * m_scale)));
        p->setFont(cf);
        p->setPen(nt::kMuted);
        const qreal tx = avR.right() + 6 * m_scale;
        const QFontMetricsF cfm(cf);
        p->drawText(QRectF(tx, avR.top() - 1, r.right() - tx - 8 * m_scale,
                           av + 2),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    cfm.elidedText(v.channel, Qt::ElideRight,
                                   qRound(r.right() - tx - 8 * m_scale)));
        p->restore();
    }

    // Avatar de canal de 14 px dentro de una fila. Usa el mismo cacheo de
    // escalado que las miniaturas (clave "ch_<id>", tamaño de destino en la
    // clave) para no reescalar en cada paint.
    void drawChannelAvatar(QPainter *p, const QRectF &r,
                           const QString &channelId) const
    {
        if (channelId.isEmpty() || r.width() < 1)
            return;
        const QString key = QStringLiteral("ch_%1").arg(channelId);
        const QPixmap pm = m_thumbs->pixmap(key);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addEllipse(r);
        p->setClipPath(clip);
        if (pm.isNull()) {
            p->fillRect(r, nt::kElevated);
        } else {
            const QSize dst = r.size().toSize();
            const QPixmap &scaled = thumbEscalada(key, dst, pm);
            const QRectF src((scaled.width() - r.width()) / 2.0,
                             (scaled.height() - r.height()) / 2.0,
                             r.width(), r.height());
            p->drawPixmap(r, scaled, src);
        }
        p->restore();
    }

    void paintList(QPainter *p, const QStyleOptionViewItem &opt,
                   const VideoInfo &v, int row) const
    {
        const QRect r = opt.rect;
        // En la fila enriquecida (Search) el hover va redondeado y con 2 px de
        // aire, para que se lea como una tarjeta destacada y no como una franja
        // de color a sangre. El resto de listas mantienen el relleno cuadrado.
        if (opt.state & QStyle::State_MouseOver) {
            if (m_metaLines >= 2) {
                p->save();
                p->setRenderHint(QPainter::Antialiasing);
                p->setPen(Qt::NoPen);
                p->setBrush(nt::kHover);
                p->drawRoundedRect(QRectF(r).adjusted(2, 2, -2, -2), 8, 8);
                p->restore();
            } else {
                p->fillRect(r, nt::kHover);
            }
        }
        if (row == m_currentRow)
            drawCurrentMark(p, r);

        const QRectF thumbR(r.left() + 6 * m_scale,
                            r.top() + (r.height() - 76 * m_scale) / 2.0,
                            136 * m_scale, 76 * m_scale);
        // Se pasa la clave, no el pixmap: el escalado va cacheado por
        // (video, tamano), asi que hace falta la clave para consultarlo.
        drawThumb(p, thumbR, cacheKeyFor(v), durationLabel(v.durationSeconds));
        drawProgressBar(p, thumbR, v);

        p->save();
        QFont t = p->font();
        t.setBold(true);
        p->setFont(t);
        p->setPen(nt::kText);
        const QRect titleR(r.left() + 152 * m_scale, r.top() + 10 * m_scale,
                           qRound(r.width() - 162 * m_scale), 34);
        // Con la meta de 2 líneas el título también puede partir en dos: la
        // caja de 34 px ya daba para dos líneas de 13 px, solo se usaba una.
        // Aquí el interlineado es el natural de la fuente y son 2 líneas, no las
        // 3 de la rejilla: la fila de lista tiene altura fija de lista y su
        // bloque de meta no crece con la celda.
        if (m_metaLines >= 2)
            drawGridTitle(p, titleR, v.title,
                          qMax(1, qRound(QFontMetricsF(t).height())), 2);
        else
            elideTitle(p, titleR, v.title);
        p->restore();

        if (m_metaLines >= 2)
            paintRichMeta(p, r, v);
        else
            paintPlainMeta(p, r, v);

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
    // Miniaturas ya escaladas al tamaño de celda, por (key de video, tamaño).
    // mutable porque drawThumb/paint son const: la cache es un efecto del
    // pintado, no estado de la vista. Se vacia en setScale/setCellSize/setMode.
    mutable QHash<QString, QPixmap> m_thumbCache;
    int m_mode = VideoListView::ListMode;
    int m_currentRow = -1;
    bool m_showRemove = false;
    bool m_showMore = false;
    int m_metaLines = 1;
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
    // Sin ancho aún (layout inicial) no hay tope que aplicar: el resizeEvent
    // del Home lo ajustará en cuanto lo haya.
    if (viewportW <= 0 || minCellW <= 0)
        return wanted;
    return qMax(1, qMin(wanted, qMax(3, viewportW / minCellW)));
}

int VideoListView::gridTitlePx(int cellW)
{
    if (cellW <= 0)
        return 13;
    // 140 px de celda -> 13 px, 340 px -> 18 px (5 px por cada 200 px de celda).
    // Los topes importan en las dos direcciones: por debajo de ~150 px el título
    // no debe bajar de 13 px (a 11 px un título de 2 líneas deja de leerse) y por
    // encima de ~320 px no tiene que seguir creciendo (con celdas de 500 px un
    // título de 25 px no cabe en dos líneas y se elide media frase).
    return qBound(13, qRound(13.0 + (cellW - 140) * 0.025), 18);
}

int VideoListView::gridTitleLineH(int cellW)
{
    return qMax(1, qRound(gridTitlePx(cellW) * 1.28));
}

int VideoListView::gridTextBlockH(int cellW)
{
    // kGridTitleLines líneas de título + 14 px hasta la línea de canal + 20 px
    // de pie. La celda reserva exactamente esto y la pintura dibuja en una caja
    // de kGridTitleLines*lineH, así que el texto no puede salirse de la celda.
    return kGridTitleLines * gridTitleLineH(cellW) + 14 + 20;
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
    // bloque de texto (hueco + título + línea de canal). El bloque se calcula
    // con la MISMA función pura que usa la pintura (gridTextBlockH), así que la
    // celda reserva de verdad lo que ocupa el título y nunca hay solape. Con un
    // ratio fijo, al estrechar la ventana el texto se sale de la celda y el
    // vídeo siguiente tapa el nombre del canal.
    const int cellH = qRound(qreal(cellW - 12 * m_scale) * 9.0 / 16.0)
                      + qRound(6 * m_scale) + 8 + gridTextBlockH(cellW);
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

void VideoListView::setMetaLines(int lines)
{
    lines = qBound(1, lines, 2);
    if (lines == m_metaLines)
        return;
    m_metaLines = lines;
    static_cast<VideosDelegate *>(itemDelegate())->setMetaLines(lines);
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