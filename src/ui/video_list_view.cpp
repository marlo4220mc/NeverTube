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
// Hueco a la izquierda del bloque de texto de una fila de lista: miniatura
// (136 px) + 6 px de margen + 10 px de aire. Lo comparten el título y la meta,
// y de él se parte el ancho útil que calculan removeButtonRect/moreButtonRect.
const qreal kListTextX = 152;

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

// Rectángulo del botón de opciones "⋯". Cuando la fila también lleva la X va
// justo a su izquierda; sin X ocupa el borde derecho, como el botón de opciones
// de Biblioteca (que es un widget del layout, pegado al borde). Reservar el
// hueco de una X que no se pinta le robaba 34 px de texto a la fila.
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

    // Mapa (opcional) de fracciones de progreso por videoId que el delegado
    // dibuja como barra al pie de la miniatura en modo rejilla.
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

    // Pastilla de directo: el acento de fondo (como la marca "EN VIVO" del
    // reproductor y la píldora roja de YouTube) en vez del negro translúcido
    // de la duración. Mientras un vídeo está en directo no hay duración que
    // mostrar: el badge la sustituye.
    static void drawLiveBadge(QPainter *p, const QRectF &r)
    {
        drawBadgeCore(p, r, QStringLiteral("En directo"), nt::kAccent);
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
                   const QString &duration, bool isLive) const
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
        if (isLive)
            drawLiveBadge(p, r);
        else if (!duration.isEmpty())
            drawBadge(p, r, duration);
    }

    // Reparte el texto en `lines` líneas con wrap voraz por palabras; la ÚLTIMA
    // lleva todo lo que queda desde el principio de esa línea, elidado. Lo usan
    // la PINTURA (drawGridTitle) y la GEOMETRÍA (paintList decide con size()
    // cuántas líneas ocupa el título para pegar la meta justo debajo): una sola
    // función, así no pueden divergir.
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
                // Con la línea ya empezado no entra: se pasa a la siguiente. Con
                // la línea vacía se acepta la palabra aunque no quepa (una sola
                // palabra más ancha que la celda), si no el bucle no avanzaría.
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
        // Se pasa la clave, no el pixmap: el escalado va cacheado por
        // (video, tamano), asi que hace falta la clave para consultarlo.
        drawThumb(p, thumbR, cacheKeyFor(v), durationLabel(v.durationSeconds),
                  v.isLive);
        drawProgressBar(p, thumbR, v);

        // Título y línea de canal al tamaño que manda en ESTA celda (ver
        // gridTitlePx): 12 px en celda pequeña, 16 px en celda ancha. El
        // interlineado lo fija la misma función pura que usa la celda, así que la
        // segunda línea cae dentro de la caja reservada. En NEGRITA: se probó sin
        // ella y el usuario pidió volver a ella (con la letra ya 1 px más pequeña
        // el título se distingue de la línea de canal).
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
        // La línea de canal/vistas/fecha del Home va un poco más pequeña que el
        // título (2 px menos, con suelo de 10): el usuario la pidió más discreta
        // que la del título, y a 1 px menos cantaba igual que él.
        m.setPixelSize(qMax(10, titlePx - 2));
        p->setFont(m);
        p->setPen(nt::kMuted);
        const QFontMetricsF fm(m);
        p->drawText(QPointF(r.left() + 8 * m_scale, titleR.bottom() + 14),
                    fm.elidedText(metaLine(v), Qt::ElideRight,
                                  qRound(gw - 16 * m_scale)));
        p->restore();
    }

    // Métricas del bloque de texto de una fila de lista. La tipografía NO se
    // toca: la que ya usaban las listas (título en la fuente de la app y en
    // negrita, meta reducida). Lo único que unifica con Biblioteca es la FORMA
    // de la fila (miniatura, canal • visitas • fecha y botón de opciones), no el
    // tamaño de letra ni la negrita.
    //
    // QFontMetricsF no tiene constructor por defecto, así que aquí van las
    // medidas (alto y ascendencia) y no la métrica en sí: quien pinta crea la
    // suya con la misma fuente.
    struct ListTextMetrics
    {
        QFont title;
        QFont meta;
        qreal lineH = 0;      // interlineado del título
        qreal metaH = 0;      // alto de la línea de meta
        qreal metaAscent = 0; // ascendencia de la meta (para su línea base)
        qreal gap = 0;        // hueco entre título y meta
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
        // Hueco entre el título y la meta: 6 px, para que no se lean como una
        // sola línea de texto.
        m.gap = 6 * m_scale;
        return m;
    }

    // Meta de la fila de lista: UNA línea "canal • visitas • fecha" con la
    // misma función format::metaLine que usa la rejilla del Home y las
    // secciones de Biblioteca. Antes esta fila pintaba solo "canal • visitas"
    // a 11,3 px: el mismo vídeo se leía distinto según la página.
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

    // Ancho útil del texto de la fila: el de la celda menos la miniatura (por
    // la izquierda) y los botones de la derecha, con la MISMA cuenta que
    // removeButtonRect/moreButtonRect, para que el título y la meta nunca
    // queden debajo de ellos. Es lo que hace la fila de Biblioteca, donde el
    // botón es un widget del layout y el texto no puede invadir su hueco.
    //
    // La cuenta es la de los rectángulos de arriba: 12 px de margen y, si hay
    // botones, sus 26 px (más los 8 de separación cuando van los dos). Medido:
    // sin descontar el botón, el título llegaba a x=839 y los puntos del ⋯ están
    // en 838-841, o sea el texto se metía debajo.
    qreal textWidth(const QRect &r, qreal leftGutter) const
    {
        qreal right = 12 * m_scale; // margen del borde
        if (m_showMore)
            right += 26 * m_scale; // hueco del botón de opciones
        if (m_showMore && m_showRemove)
            right += 8 * m_scale; // separación de los dos botones
        if (m_showRemove)
            right += 26 * m_scale;
        return r.width() - leftGutter - right;
    }

    void paintList(QPainter *p, const QStyleOptionViewItem &opt,
                   const VideoInfo &v, int row) const
    {
        const QRect r = opt.rect;
        // Hover redondeado con 2 px de aire: se lee como una tarjeta
        // destacada y no como una franja de color a sangre (es lo que ya
        // llevaba la fila enriquecida de Search y lo que mejor casa con las
        // secciones de Biblioteca, cuya fila también va sobre una tarjeta).
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
        // Se pasa la clave, no el pixmap: el escalado va cacheado por
        // (video, tamano), asi que hace falta la clave para consultarlo.
        drawThumb(p, thumbR, cacheKeyFor(v), durationLabel(v.durationSeconds),
                  v.isLive);
        drawProgressBar(p, thumbR, v);

        // Título: la fuente de la app en negrita (la que ya tenía la fila) y 2
        // líneas, como la fila de Biblioteca. El interlineado es el natural de
        // la fuente y son 2 líneas (no las 3 de la rejilla): la fila de lista
        // tiene altura fija.
        const ListTextMetrics m = listMetrics(p->font());
        const qreal titleW = textWidth(r, kListTextX * m_scale);
        // Líneas que ocupa REALMENTE el título (1 o 2). El bloque se centra con
        // esa altura y la meta va pegada a su última línea: antes se reservaban
        // 2 líneas siempre, así que con un título de una línea la meta quedaba
        // un renglón entero más abajo (el usuario lo pidió pegado al título).
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
    // Miniaturas ya escaladas al tamaño de celda, por (key de video, tamaño).
    // mutable porque drawThumb/paint son const: la cache es un efecto del
    // pintado, no estado de la vista. Se vacia en setScale/setCellSize/setMode.
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
    // Sin ancho aún (layout inicial) no hay tope que aplicar: el resizeEvent
    // del Home lo ajustará en cuanto lo haya.
    if (viewportW <= 0 || minCellW <= 0)
        return wanted;
    return qMax(1, qMin(wanted, qMax(3, viewportW / minCellW)));
}

int VideoListView::gridTitlePx(int cellW)
{
    if (cellW <= 0)
        return 12;
    // 140 px de celda -> 12 px, 340 px -> 16 px (5 px por cada 200 px de celda).
    // Los topes importan en las dos direcciones: por debajo de ~150 px el título
    // no debe bajar de 12 px (a 11 px deja de leerse) y por encima de ~320 px no
    // tiene que seguir creciendo. Todo este rango bajó 1 px (y el techo 18 -> 16)
    // porque con 14 px en negrita el usuario veía el título demasiado grande.
    return qBound(12, qRound(12.0 + (cellW - 140) * 0.025), 16);
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
    const int spacing = kGridSpacing;
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
    // En IconMode el tamaño de la celda lo manda el sizeHint del MODELO, no
    // gridSize(): dejando el kGridH fijo (258) en el sizeHint mientras aquí se
    // calcula otro cellH, las filas ocupaban 258 y la página dimensionaba la
    // sección con este cellH, así que la sección se quedaba más corta que el
    // contenido y la ÚLTIMA fila no se veía (y con ella la línea de canal del
    // vídeo). Se escribe aquí para que sizeHint, gridSize, delegate y alto de
    // sección salgan todos del mismo cálculo.
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
    // visualRect (público) devuelve el rect del item YA COLOCADO por el layout,
    // que es lo que hay que comparar con la altura de la sección.
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
    // applyGridGeometry escribe el sizeHint de los items con el tamaño de celda
    // REAL, así que se llama DESPUÉS de crearlos: si se llama antes (con el
    // modelo ya vaciado) no tendría items que corregir.
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
    // Por el mismo motivo que en showVideos: el sizeHint de la rejilla lo fija
    // applyGridGeometry con el tamaño de celda real, y los items nuevos ya
    // nazcan con el (gridItemSize) pero la geometría puede haber cambiado.
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
            // En modo lista con la X activa, el click sobre ella borra en vez
            // de reproducir.
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