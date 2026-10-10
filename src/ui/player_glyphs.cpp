#include "player_glyphs.h"

#include <QColor>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>

namespace nt {
namespace {

// Material Symbols canvas: all paths use viewBox "0 -960 960 960", with the
// y axis pointing UP (the origin is at the bottom left).
constexpr qreal kGrid = 960.0;

// Geometry per icon, in the ORDER of the PlayerIcon enum. Source:
// google/material-design-icons (Apache-2.0),
// symbols/web/<name>/materialsymbolsoutlined/<name>[_fill1]_24px.svg
const char *const kGlyphs[] = {
    // Shuffle
    "M560-160v-80h104L537-367l57-57 126 126v-102h80v240H560Zm-344 0-56-56 504-504H560"
    "v-80h240v240h-80v-104L216-160Zm151-377L160-744l56-56 207 207-56 56Z",
    // Prev (fill1)
    "M220-240v-480h80v480h-80Zm520 0L380-480l360-240v480Z",
    // Play (fill1)
    "M320-200v-560l440 280-440 280Z",
    // Pause (fill1)
    "M560-200v-560h160v560H560Zm-320 0v-560h160v560H240Z",
    // Next (fill1)
    "M660-240v-480h80v480h-80Zm-440 0v-480l360 240-360 240Z",
    // Repeat
    "M280-80 120-240l160-160 56 58-62 62h406v-160h80v240H274l62 62-56 58Zm-80-440v-240h486"
    "l-62-62 56-58 160 160-160 160-56-58 62-62H280v160h-80Z",
    // Volume
    "M560-131v-82q90-26 145-100t55-168q0-94-55-168T560-749v-82q124 28 202 125.5T840-481q0"
    " 127-78 224.5T560-131ZM120-360v-240h160l200-200v640L280-360H120Zm440 40v-322q47 22"
    " 73.5 66t26.5 96q0 51-26.5 94.5T560-320ZM400-606l-86 86H200v80h114l86 86v-252ZM300-480Z",
    // Muted
    "M792-56 671-177q-25 16-53 27.5T560-131v-82q14-5 27.5-10t25.5-12L480-368v208L280-360H120"
    "v-240h128L56-792l56-56 736 736-56 56Zm-8-232-58-58q17-31 25.5-65t8.5-70q0-94-55-168"
    "T560-749v-82q124 28 202 125.5T840-481q0 53-14.5 102T784-288ZM650-422l-90-90v-130q47 22"
    " 73.5 66t26.5 96q0 15-2.5 29.5T650-422ZM480-592 376-696l104-104v208Zm-80 238v-94"
    "l-72-72H200v80h114l86 86Zm-36-130Z",
    // Captions
    "M200-160q-33 0-56.5-23.5T120-240v-480q0-33 23.5-56.5T200-800h560q33 0 56.5 23.5T840-720"
    "v480q0 33-23.5 56.5T760-160H200Zm0-80h560v-480H200v480Zm80-120h120q17 0 28.5-11.5"
    "T440-400v-40h-60v20h-80v-120h80v20h60v-40q0-17-11.5-28.5T400-600H280q-17 0-28.5 11.5"
    "T240-560v160q0 17 11.5 28.5T280-360Zm280 0h120q17 0 28.5-11.5T720-400v-40h-60v20h-80v-120"
    "h80v20h60v-40q0-17-11.5-28.5T680-600H560q-17 0-28.5 11.5T520-560v160q0 17 11.5 28.5"
    "T560-360ZM200-240v-480 480Z",
    // PlaylistAdd
    "M120-320v-80h280v80H120Zm0-160v-80h440v80H120Zm0-160v-80h440v80H120Zm520 480v-160H480"
    "v-80h160v-160h80v160h160v80H720v160h-80Z",
    // Settings
    "m370-80-16-128q-13-5-24.5-12T307-235l-119 50L78-375l103-78q-1-7-1-13.5v-27q0-6.5 1-13.5"
    "L78-585l110-190 119 50q11-8 23-15t24-12l16-128h220l16 128q13 5 24.5 12t22.5 15l119-50"
    " 110 190-103 78q1 7 1 13.5v27q0 6.5-2 13.5l103 78-110 190-118-50q-11 8-23 15t-24 12"
    "L590-80H370Zm70-80h79l14-106q31-8 57.5-23.5T639-327l99 41 39-68-86-65q5-14 7-29.5t2-31.5"
    "q0-16-2-31.5t-7-29.5l86-65-39-68-99 42q-22-23-48.5-38.5T533-694l-13-106h-79l-14 106"
    "q-31 8-57.5 23.5T321-633l-99-41-39 68 86 64q-5 15-7 30t-2 32q0 16 2 31t7 30l-86 65 39 68"
    " 99-42q22 23 48.5 38.5T427-266l13 106Zm42-180q58 0 99-41t41-99q0-58-41-99t-99-41q-59 0"
    "-99.5 41T342-480q0 58 40.5 99t99.5 41Zm-2-140Z",
    // Fullscreen
    "M120-120v-200h80v120h120v80H120Zm520 0v-80h120v-120h80v200H640ZM120-640v-200h200v80H200"
    "v120h-80Zm640 0v-120H640v-80h200v200h-80Z",
    // FullscreenExit
    "M240-120v-120H120v-80h200v200h-80Zm400 0v-200h200v80H720v120h-80ZM120-640v-80h120v-120h80"
    "v200H120Zm520 0v-200h80v120h120v80H640Z",
    // Close
    "m256-200-56-56 224-224-224-224 56-56 224 224 224-224 56 56-224 224 224 224-56 56-224-224"
    "-224 224Z",
};

// Parser for the SVG path subset these 15 glyphs use:
// M m L l H h V v Q q T t Z. Qt does not expose a `d` parser without QtSvg,
// and linking it would add Qt5Svg.dll to both Windows packages; this subset is
// all Material Symbols needs here.
struct PathBuilder {
    QPainterPath &path;
    QString d;
    int i = 0;
    qreal x = 0.0;  // current point
    qreal y = 0.0;
    qreal sx = 0.0; // start of the current subpath
    qreal sy = 0.0;
    qreal cx = 0.0; // last quadratic control point (for T/t)
    qreal cy = 0.0;
    QChar prev;     // previous command
    bool hasCtrl = false;
    bool ok = true;

    explicit PathBuilder(QPainterPath &p) : path(p) {}

    qreal number()
    {
        while (i < d.size()
               && (d.at(i).isSpace() || d.at(i) == QLatin1Char(',')))
            ++i;
        const int start = i;
        if (i < d.size()
            && (d.at(i) == QLatin1Char('-') || d.at(i) == QLatin1Char('+')))
            ++i;
        while (i < d.size() && d.at(i).isDigit())
            ++i;
        if (i < d.size() && d.at(i) == QLatin1Char('.')) {
            ++i;
            while (i < d.size() && d.at(i).isDigit())
                ++i;
        }
        bool good = false;
        const qreal v = d.mid(start, i - start).toDouble(&good);
        if (!good)
            ok = false;
        return v;
    }

    bool numberAhead() const
    {
        int j = i;
        while (j < d.size()
               && (d.at(j).isSpace() || d.at(j) == QLatin1Char(',')))
            ++j;
        if (j >= d.size())
            return false;
        const QChar c = d.at(j);
        return c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+')
               || c == QLatin1Char('.');
    }

    bool build()
    {
        QChar cmd;
        while (i < d.size()) {
            if (d.at(i).isLetter()) {
                cmd = d.at(i);
                ++i;
            } else if (!numberAhead()) {
                ++i; // stray separator
                continue;
            }
            if (cmd.isNull())
                break;
            switch (cmd.unicode()) {
            case 'M':
            case 'm': {
                const bool rel = (cmd == QLatin1Char('m'));
                qreal px = number(), py = number();
                if (rel) {
                    px += x;
                    py += y;
                }
                path.moveTo(px, py);
                x = sx = px;
                y = sy = py;
                hasCtrl = false;
                prev = cmd;
                // Further pairs of the same moveto are lineto (SVG).
                cmd = rel ? QLatin1Char('l') : QLatin1Char('L');
                break;
            }
            case 'L':
            case 'l': {
                const bool rel = (cmd == QLatin1Char('l'));
                qreal px = number(), py = number();
                if (rel) {
                    px += x;
                    py += y;
                }
                path.lineTo(px, py);
                x = px;
                y = py;
                hasCtrl = false;
                break;
            }
            case 'H':
            case 'h': {
                qreal px = number();
                if (cmd == QLatin1Char('h'))
                    px += x;
                path.lineTo(px, y);
                x = px;
                hasCtrl = false;
                break;
            }
            case 'V':
            case 'v': {
                qreal py = number();
                if (cmd == QLatin1Char('v'))
                    py += y;
                path.lineTo(x, py);
                y = py;
                hasCtrl = false;
                break;
            }
            case 'Q':
            case 'q': {
                const bool rel = (cmd == QLatin1Char('q'));
                qreal c1x = number(), c1y = number();
                qreal px = number(), py = number();
                if (rel) {
                    c1x += x;
                    c1y += y;
                    px += x;
                    py += y;
                }
                path.quadTo(c1x, c1y, px, py);
                cx = c1x;
                cy = c1y;
                x = px;
                y = py;
                hasCtrl = true;
                prev = cmd;
                break;
            }
            case 'T':
            case 't': {
                const bool rel = (cmd == QLatin1Char('t'));
                qreal px = number(), py = number();
                if (rel) {
                    px += x;
                    py += y;
                }
                // The control point is the reflection of the previous one only
                // if the last curve was quadratic (Q/q/T/t); otherwise it is
                // the current point.
                qreal c1x = x, c1y = y;
                const QChar p = prev.toLower();
                if (hasCtrl
                    && (p == QLatin1Char('q') || p == QLatin1Char('t'))) {
                    c1x = 2 * x - cx;
                    c1y = 2 * y - cy;
                }
                path.quadTo(c1x, c1y, px, py);
                cx = c1x;
                cy = c1y;
                x = px;
                y = py;
                hasCtrl = true;
                prev = cmd;
                break;
            }
            case 'Z':
            case 'z':
                path.closeSubpath();
                x = sx;
                y = sy;
                hasCtrl = false;
                prev = cmd;
                break;
            default:
                ok = false;
                return false;
            }
            if (!ok)
                return false;
        }
        return ok;
    }
};

} // namespace

QIcon playerIcon(PlayerIcon kind, const QColor &color, int size)
{
    if (size <= 0)
        size = 20;
    // Cache by (icon, size, color). The color is keyed by its hex, so a hot
    // accent change creates a new entry instead of freezing the color of the
    // first call (bug already seen in the nav).
    const QString key = QStringLiteral("%1|%2|%3")
                            .arg(static_cast<int>(kind))
                            .arg(size)
                            .arg(color.name(QColor::HexArgb));
    static QHash<QString, QIcon> cache;
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return hit.value();

    const int idx = static_cast<int>(kind);
    QPainterPath path;
    // Material designs for a "nonzero" fill (the SVG one); in Qt that is
    // WindingFill. With OddEvenFill the holes of e.g. the gear and the speaker
    // would change.
    path.setFillRule(Qt::WindingFill);
    PathBuilder builder(path);
    builder.d = QString::fromLatin1(kGlyphs[idx]);
    const bool parsed = builder.build();

    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    if (parsed) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        // 960 canvas with y up -> size x size box with the origin at the top
        // left: translate and scale in positive values (no "flip"), so that
        // y=-960 (top of the glyph) lands on 0 and y=0 on `size`.
        const qreal s = size / kGrid;
        p.translate(0.0, size);
        p.scale(s, s);
        p.setPen(Qt::NoPen);
        p.fillPath(path, color);
    }
    const QIcon ic(pm);
    cache.insert(key, ic);
    return ic;
}

} // namespace nt
