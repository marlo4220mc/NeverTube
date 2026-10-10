#include "icons.h"

#include <cmath>

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

namespace nt {

static void paintPlay(QPainter &p, const QRectF &r)
{
    QPainterPath path;
    path.moveTo(r.left() + r.width() * 0.30, r.top() + r.height() * 0.12);
    path.lineTo(r.left() + r.width() * 0.30, r.bottom() - r.height() * 0.12);
    path.lineTo(r.right() - r.width() * 0.12, r.center().y());
    path.closeSubpath();
    p.fillPath(path, p.pen().color());
}

static void paintPause(QPainter &p, const QRectF &r)
{
    const qreal w = r.width() * 0.28;
    const qreal x = r.left() + r.width() * 0.18;
    p.fillRect(QRectF(x, r.top() + r.height() * 0.15, w, r.height() * 0.7),
               p.pen().color());
    p.fillRect(QRectF(r.right() - x - w, r.top() + r.height() * 0.15, w,
                      r.height() * 0.7),
               p.pen().color());
}

static void paintPlayCircle(QPainter &p, const QRectF &r)
{
    const qreal cw = r.width();
    p.setPen(QPen(p.pen().color(), cw * 0.09, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(r.adjusted(cw * 0.05, cw * 0.05, -cw * 0.05, -cw * 0.05));
    const qreal d = cw * 0.24;
    const QRectF pr(r.left() + (cw - d) / 2, r.top() + (cw - d) / 2, d, d);
    QPainterPath path;
    path.moveTo(pr.left() + pr.width() * 0.32, pr.top() + pr.height() * 0.14);
    path.lineTo(pr.left() + pr.width() * 0.32, pr.bottom() - pr.height() * 0.14);
    path.lineTo(pr.right() - pr.width() * 0.12, pr.center().y());
    path.closeSubpath();
    p.setBrush(p.pen().color());
    p.setPen(Qt::NoPen);
    p.fillPath(path, p.pen().color());
}

static void paintArrow(QPainter &p, const QRectF &r, bool right)
{
    QPainterPath path;
    if (right) {
        path.moveTo(r.left() + r.width() * 0.25, r.top() + r.height() * 0.2);
        path.lineTo(r.right() - r.width() * 0.25, r.center().y());
        path.lineTo(r.left() + r.width() * 0.25, r.bottom() - r.height() * 0.2);
    } else {
        path.moveTo(r.right() - r.width() * 0.25, r.top() + r.height() * 0.2);
        path.lineTo(r.left() + r.width() * 0.25, r.center().y());
        path.lineTo(r.right() - r.width() * 0.25, r.bottom() - r.height() * 0.2);
    }
    p.setPen(QPen(p.pen().color(), r.width() * 0.14, Qt::SolidLine,
                  Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

// Pulgar relleno (estilo YouTube): palma redondeada + barra del pulgar,
// con una muesca que separa ambos y da la silueta clásica del fave.
// Pulgar relleno (geometría Material "thumb_up" normalizada a 24px): la
// silueta en dos piezas (pulgar + mano con muesca) es legible a 20px.
static void paintThumb(QPainter &p, const QRectF &r, bool down)
{
    const qreal w = r.width();
    const qreal h = r.height();
    const QColor col = p.pen().color();
    auto P = [&](qreal x, qreal y) {
        if (down)
            y = 24.0 - y;
        return QPointF(r.left() + x / 24.0 * w, r.top() + y / 24.0 * h);
    };

    // Mano (pieza grande).
    QPainterPath hand;
    hand.setFillRule(Qt::WindingFill);
    hand.moveTo(P(23, 10));
    hand.cubicTo(P(23, 8.9), P(22.1, 8), P(21, 8));     // c0,-1.1 -.9,-2 -2,-2
    hand.lineTo(P(14.69, 8));                            // h-6.31
    hand.lineTo(P(15.64, 3.43));                         // l0.95 -4.57
    hand.lineTo(P(15.67, 3.11));                         // l.03 -.32
    hand.cubicTo(P(15.67, 2.70), P(15.50, 2.32),
                 P(15.23, 2.05));                        // c0,-.41 -.17,-.79 -.44,-1.06
    hand.lineTo(P(14.17, 1));                            // L14.17 1
    hand.lineTo(P(7.59, 7.59));                          // L7.59 7.59
    hand.cubicTo(P(7.22, 7.95), P(7, 8.45), P(7, 9));    // C...
    hand.lineTo(P(7, 19));                               // v10
    hand.cubicTo(P(7, 20.1), P(7.9, 21), P(9, 21));      // c0,1.1 .9,2 2,2
    hand.lineTo(P(18, 21));                              // h9
    hand.cubicTo(P(18.83, 21), P(19.54, 20.5),
                 P(19.84, 19.78));                       // c.83,0 1.54,-.5 1.84,-1.22
    hand.lineTo(P(22.86, 12.73));                        // l3.02 -7.05
    hand.cubicTo(P(22.95, 12.50), P(23, 12.26), P(23, 12)); // c.09,-.23 .14,-.47 .14,-.73
    hand.lineTo(P(23, 10));                              // v-2
    hand.closeSubpath();

    // Base del pulgar (pieza pequeña a la izquierda).
    QPainterPath base;
    base.setFillRule(Qt::WindingFill);
    base.moveTo(P(1, 21));
    base.lineTo(P(5, 21));
    base.lineTo(P(5, 9));
    base.lineTo(P(1, 9));
    base.closeSubpath();

    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(col);
    p.drawPath(hand);
    p.drawPath(base);
    p.restore();
}

QIcon icon(Icon kind, const QColor &color, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(color, size * 0.09, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    const QRectF r(1.0, 1.0, size - 2.0, size - 2.0);

    switch (kind) {
    case Icon::Home: {
        p.setPen(QPen(color, size * 0.10, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath roof;
        roof.moveTo(r.left() + r.width() * 0.08, r.center().y());
        roof.lineTo(r.center().x(), r.top() + r.height() * 0.10);
        roof.lineTo(r.right() - r.width() * 0.08, r.center().y());
        p.drawPath(roof);
        p.drawRect(QRectF(r.left() + r.width() * 0.22, r.center().y(),
                          r.width() * 0.56, r.height() * 0.36));
        p.drawRect(QRectF(r.left() + r.width() * 0.40, r.center().y() + r.height() * 0.08,
                          r.width() * 0.20, r.height() * 0.28));
        break;
    }
    case Icon::Search:
        p.drawEllipse(r.adjusted(r.width() * 0.14, r.height() * 0.14,
                                 -r.width() * 0.32, -r.height() * 0.32));
        p.drawLine(r.right() - r.width() * 0.30, r.bottom() - r.height() * 0.30,
                   r.right() - r.width() * 0.10, r.bottom() - r.height() * 0.10);
        break;
    case Icon::Library:
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.14, r.top() + r.height() * 0.22,
                                 r.width() * 0.66, r.height() * 0.30),
                          size * 0.06, size * 0.06);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.14, r.top() + r.height() * 0.56,
                                 r.width() * 0.66, r.height() * 0.26),
                          size * 0.06, size * 0.06);
        p.drawLine(r.right() - r.width() * 0.10, r.top() + r.height() * 0.22,
                   r.right() - r.width() * 0.10, r.bottom() - r.height() * 0.06);
        break;
    case Icon::Settings: {
        const qreal c = r.width() * 0.30;
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4.0;
            const QPointF cc(r.center().x() + qCos(a) * r.width() * 0.36,
                             r.center().y() + qSin(a) * r.height() * 0.36);
            p.drawEllipse(cc, c * 0.30, c * 0.30);
        }
        p.drawEllipse(r.center(), c * 0.86, c * 0.86);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::transparent);
        QPainterPath hole;
        hole.addEllipse(r.center(), c * 0.40, c * 0.40);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillPath(hole, Qt::black);
        break;
    }
    case Icon::Play:
        paintPlay(p, r);
        break;
    case Icon::Pause:
        paintPause(p, r);
        break;
    case Icon::Next: {
        // Barra vertical (izquierda) + triángulo relleno hacia la derecha: |▶
        p.setPen(QPen(color, size * 0.10, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(r.left() + r.width() * 0.10, r.top() + r.height() * 0.22,
                          r.width() * 0.20, r.height() * 0.56));
        QPainterPath path;
        path.moveTo(r.left() + r.width() * 0.46, r.top() + r.height() * 0.20);
        path.lineTo(r.left() + r.width() * 0.46, r.bottom() - r.height() * 0.20);
        path.lineTo(r.right() - r.width() * 0.10, r.center().y());
        path.closeSubpath();
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.fillPath(path, color);
        break;
    }
    case Icon::Prev: {
        // Barra vertical (derecha) + triángulo relleno hacia la izquierda: ◀|
        p.setPen(QPen(color, size * 0.10, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(r.right() - r.width() * 0.30, r.top() + r.height() * 0.22,
                          r.width() * 0.20, r.height() * 0.56));
        QPainterPath path;
        path.moveTo(r.right() - r.width() * 0.46, r.top() + r.height() * 0.20);
        path.lineTo(r.right() - r.width() * 0.46, r.bottom() - r.height() * 0.20);
        path.lineTo(r.left() + r.width() * 0.10, r.center().y());
        path.closeSubpath();
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.fillPath(path, color);
        break;
    }
    case Icon::Mute: {
        QPainterPath speaker;
        speaker.moveTo(r.left() + r.width() * 0.16, r.center().y() + r.height() * 0.20);
        speaker.lineTo(r.left() + r.width() * 0.16, r.center().y() - r.height() * 0.20);
        speaker.lineTo(r.left() + r.width() * 0.40, r.center().y() - r.height() * 0.20);
        speaker.lineTo(r.left() + r.width() * 0.58, r.top() + r.height() * 0.10);
        speaker.lineTo(r.left() + r.width() * 0.58, r.bottom() - r.height() * 0.10);
        speaker.lineTo(r.left() + r.width() * 0.40, r.center().y() + r.height() * 0.20);
        speaker.closeSubpath();
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.fillPath(speaker, color);
        p.setPen(QPen(color, size * 0.09, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(r.left() + r.width() * 0.40, r.top() + r.height() * 0.18,
                         r.width() * 0.52, r.height() * 0.64),
                  -45 * 16, 90 * 16);
        p.drawArc(QRectF(r.left() + r.width() * 0.58, r.top() + r.height() * 0.08,
                         r.width() * 0.36, r.height() * 0.84),
                  -45 * 16, 90 * 16);
        break;
    }
    case Icon::Unmute: {
        QPainterPath speaker;
        speaker.moveTo(r.left() + r.width() * 0.16, r.center().y() + r.height() * 0.20);
        speaker.lineTo(r.left() + r.width() * 0.16, r.center().y() - r.height() * 0.20);
        speaker.lineTo(r.left() + r.width() * 0.40, r.center().y() - r.height() * 0.20);
        speaker.lineTo(r.left() + r.width() * 0.58, r.top() + r.height() * 0.10);
        speaker.lineTo(r.left() + r.width() * 0.58, r.bottom() - r.height() * 0.10);
        speaker.lineTo(r.left() + r.width() * 0.40, r.center().y() + r.height() * 0.20);
        speaker.closeSubpath();
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.fillPath(speaker, color);
        p.setPen(QPen(color, size * 0.09, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(r.left() + r.width() * 0.44, r.top() + r.height() * 0.18,
                         r.width() * 0.44, r.height() * 0.64),
                  -45 * 16, 90 * 16);
        p.drawLine(r.left() + r.width() * 0.72, r.center().y() + r.height() * 0.14,
                   r.right() - r.width() * 0.06, r.top() + r.height() * 0.08);
        break;
    }
    case Icon::Subtitles: {
        p.setPen(QPen(color, size * 0.09, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.22,
                                 r.top() + r.height() * 0.22,
                                 r.width() * 0.56, r.height() * 0.56),
                          size * 0.08, size * 0.08);
        const qreal sy = r.center().y();
        p.drawLine(r.left() + r.width() * 0.32, sy - size * 0.09,
                   r.right() - r.width() * 0.32, sy - size * 0.09);
        p.drawLine(r.left() + r.width() * 0.32, sy + size * 0.09,
                   r.right() - r.width() * 0.42, sy + size * 0.09);
        break;
    }
    case Icon::Fullscreen: {
        p.setPen(QPen(color, size * 0.09, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const qreal arm = r.width() * 0.22;
        const qreal l = r.left();
        const qreal t = r.top();
        const qreal rr = r.right();
        const qreal b = r.bottom();
        // Esquinas: (l,t) sup-izq, (rr,t) sup-der, (l,b) inf-izq, (rr,b) inf-der.
        p.drawLine(QPointF(l, t), QPointF(l + arm, t));
        p.drawLine(QPointF(l, t), QPointF(l, t + arm));
        p.drawLine(QPointF(rr, t), QPointF(rr - arm, t));
        p.drawLine(QPointF(rr, t), QPointF(rr, t + arm));
        p.drawLine(QPointF(l, b), QPointF(l + arm, b));
        p.drawLine(QPointF(l, b), QPointF(l, b - arm));
        p.drawLine(QPointF(rr, b), QPointF(rr - arm, b));
        p.drawLine(QPointF(rr, b), QPointF(rr, b - arm));
        break;
    }
    case Icon::Star:
    case Icon::StarFilled: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPainterPath star;
        const qreal R = r.width() * 0.47;
        const qreal rx = r.width() * 0.5;
        for (int i = 0; i < 10; ++i) {
            const qreal rad = (i % 2 == 0) ? R : R * 0.46;
            const qreal a = i * M_PI / 5.0 - M_PI / 2.0;
            const QPointF pt(r.left() + rx + qCos(a) * rad,
                             r.top() + r.height() * 0.5 + qSin(a) * rad);
            if (i == 0)
                star.moveTo(pt);
            else
                star.lineTo(pt);
        }
        star.closeSubpath();
        p.fillPath(star, color);
        break;
    }
    case Icon::Thumb:
        paintThumb(p, r, false);
        break;
    case Icon::ThumbDown:
        paintThumb(p, r, true);
        break;
    case Icon::Share: {
        // Nodo central con dos conexiones (estilo compartir): nodos más
        // grandes y limpios para que coincidan con el grosor del resto del set.
        const qreal rad = size * 0.10;
        p.setPen(QPen(color, size * 0.09, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const QPointF a(r.left() + r.width() * 0.24, r.center().y());
        const QPointF b(r.right() - r.width() * 0.22, r.center().y() - r.height() * 0.26);
        const QPointF c(r.right() - r.width() * 0.22, r.center().y() + r.height() * 0.26);
        p.drawLine(a, b);
        p.drawLine(a, c);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(a, rad, rad);
        p.drawEllipse(b, rad, rad);
        p.drawEllipse(c, rad, rad);
        break;
    }
    case Icon::QueueList: {
        p.setPen(QPen(color, size * 0.10, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 3; ++i) {
            const qreal y = r.top() + r.height() * (0.22 + 0.28 * i);
            p.drawLine(r.left() + r.width() * 0.12, y,
                       r.left() + r.width() * 0.42, y);
            p.drawEllipse(QPointF(r.left() + r.width() * 0.62, y),
                          size * 0.06, size * 0.06);
        }
        break;
    }
    case Icon::Download: {
        p.setPen(QPen(color, size * 0.10, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawLine(r.center().x(), r.top() + r.height() * 0.10,
                   r.center().x(), r.center().y() + r.height() * 0.24);
        QPainterPath d;
        d.moveTo(r.center().x() - r.width() * 0.16, r.center().y() + r.height() * 0.10);
        d.lineTo(r.center().x(), r.center().y() + r.height() * 0.30);
        d.lineTo(r.center().x() + r.width() * 0.16, r.center().y() + r.height() * 0.10);
        p.drawPath(d);
        p.drawRoundedRect(QRectF(r.left() + r.width() * 0.10,
                                 r.center().y() + r.height() * 0.20,
                                 r.width() * 0.80, r.height() * 0.16),
                          size * 0.04, size * 0.04);
        p.drawLine(r.left() + r.width() * 0.10, r.bottom() - r.height() * 0.10,
                   r.right() - r.width() * 0.10, r.bottom() - r.height() * 0.10);
        break;
    }
    case Icon::Refresh: {
        p.setPen(QPen(color, size * 0.10, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawArc(r.adjusted(r.width() * 0.16, r.height() * 0.16,
                             -r.width() * 0.16, -r.height() * 0.16),
                  30 * 16, 270 * 16);
        QPainterPath head;
        head.moveTo(r.center().x() + r.width() * 0.30, r.top() + r.height() * 0.10);
        head.lineTo(r.center().x() + r.width() * 0.44, r.top() + r.height() * 0.22);
        head.lineTo(r.center().x() + r.width() * 0.18, r.top() + r.height() * 0.20);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.fillPath(head, color);
        break;
    }
    case Icon::Clear: {
        p.setPen(QPen(color, size * 0.10, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawLine(r.left() + r.width() * 0.25, r.top() + r.height() * 0.25,
                   r.right() - r.width() * 0.25, r.bottom() - r.height() * 0.25);
        p.drawLine(r.right() - r.width() * 0.25, r.top() + r.height() * 0.25,
                   r.left() + r.width() * 0.25, r.bottom() - r.height() * 0.25);
        break;
    }
    case Icon::User:
        p.drawEllipse(QRectF(r.center().x() - r.width() * 0.13,
                             r.top() + r.height() * 0.12,
                             r.width() * 0.26, r.height() * 0.26));
        p.drawArc(QRectF(r.left() + r.width() * 0.12,
                         r.center().y() - r.height() * 0.02,
                         r.width() * 0.76, r.height() * 0.5),
                  160 * 16, 220 * 16);
        break;
    case Icon::ListAdd: {
        p.setPen(QPen(color, size * 0.10, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 3; ++i) {
            const qreal y = r.top() + r.height() * (0.2 + 0.28 * i);
            p.drawLine(r.left() + r.width() * 0.1, y,
                       r.left() + r.width() * 0.62, y);
        }
        const QPointF c(r.right() - r.width() * 0.22, r.center().y());
        p.drawEllipse(c, size * 0.13, size * 0.13);
        p.drawLine(c.x() - size * 0.09, c.y(), c.x() + size * 0.09, c.y());
        p.drawLine(c.x(), c.y() - size * 0.09, c.x(), c.y() + size * 0.09);
        break;
    }
    case Icon::WatchLater: {
        QPainterPath clock;
        clock.addEllipse(r.adjusted(size * 0.06, size * 0.06, -size * 0.06,
                                    -size * 0.06));
        p.setPen(QPen(color, size * 0.07, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(clock);
        p.drawLine(r.center().x(), r.center().y() - size * 0.06,
                   r.center().x(), r.center().y() + size * 0.12);
        p.drawLine(r.center().x(), r.center().y(),
                   r.center().x() + size * 0.10, r.center().y());
        break;
    }
    case Icon::Back:
        paintArrow(p, r, false);
        break;
    default:
        break;
    }
    p.end();
    return QIcon(pm);
}

QPixmap appLogo(int size)
{
    if (size <= 0)
        return QPixmap();
    // La entrada de 256 es la mayor del .qrc (el maestro de 1024 no se embebe).
    static const QPixmap src(QStringLiteral(":/icon/nt_256.png"));
    if (src.isNull())
        return QPixmap();
    return src.scaled(size, size, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

QString emptyStateHtml(const QString &text)
{
    static const bool hasLogo =
        !QPixmap(QStringLiteral(":/icon/nt_48.png")).isNull();
    if (!hasLogo)
        return text;
    return QStringLiteral(
               "<img src=':/icon/nt_48.png' width='40' height='40'><br>%1")
        .arg(text.toHtmlEscaped());
}

QPixmap circularAvatar(const QPixmap &src, int size)
{
    if (src.isNull() || size <= 0)
        return QPixmap();
    const QPixmap scaled = src.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                     Qt::SmoothTransformation);
    const QPixmap base = scaled.copy((scaled.width() - size) / 2,
                                     (scaled.height() - size) / 2, size, size);
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    p.setClipPath(clip);
    p.drawPixmap(0, 0, base);
    return out;
}

} // namespace nt