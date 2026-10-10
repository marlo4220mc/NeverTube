#include "format.h"

#include "../models.h"
#include "theme.h"

#include <cmath>

#include <QDateTime>
#include <QRegularExpression>

QString fmtTime(double seconds)
{
    if (seconds <= 0)
        return QStringLiteral("0:00");
    const int total = static_cast<int>(seconds);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QString durationLabel(int seconds)
{
    if (seconds <= 0)
        return QString();
    return fmtTime(seconds);
}

QString compactCount(qint64 count)
{
    if (count <= 0)
        return QString();
    if (count >= 1000000) {
        const double v = count / 1000000.0;
        return QString::number(v >= 10 ? v : std::round(v * 10) / 10.0, 'f', v >= 10 ? 0 : 1)
            + QStringLiteral(" M");
    }
    if (count >= 1000) {
        const double v = count / 1000.0;
        return QString::number(v >= 10 ? v : std::round(v * 10) / 10.0, 'f', v >= 10 ? 0 : 1)
            + QStringLiteral(" K");
    }
    return QString::number(count);
}

QString viewsLabel(qint64 count)
{
    const QString c = compactCount(count);
    return c.isEmpty() ? QString() : c + QStringLiteral(" vistas");
}

QString publishedLabel(const QString &publishedAt)
{
    if (publishedAt.isEmpty())
        return QString();
    const QDateTime date = QDateTime::fromString(publishedAt, Qt::ISODate);
    if (!date.isValid())
        return QString();
    const qint64 days = date.daysTo(QDateTime::currentDateTime());
    if (days < 1)
        return QStringLiteral("hoy");
    if (days < 7)
        return QStringLiteral("hace %1 d").arg(days);
    if (days < 30)
        return QStringLiteral("hace %1 sem").arg(days / 7);
    if (days < 365)
        return QStringLiteral("hace %1 mes").arg(days / 30);
    return QStringLiteral("hace %1 año").arg(days / 365);
}

QString metaLine(const VideoInfo &v)
{
    QStringList parts;
    if (!v.channel.isEmpty())
        parts << v.channel;
    if (v.viewCount > 0)
        parts << viewsLabel(v.viewCount);
    const QString pub = publishedLabel(v.publishedAt);
    if (!pub.isEmpty())
        parts << pub;
    return parts.join(QStringLiteral("  •  "));
}

QString descriptionHtml(const QString &plain)
{
    QString rich = plain.toHtmlEscaped();
    rich.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    const QRegularExpression urlRe(QStringLiteral(R"((https?://[^\s<]+))"));
    rich.replace(urlRe, QStringLiteral(
        "<a href=\"\\1\" style=\"color:%1;\">\\1</a>").arg(nt::accentHex()));
    return QStringLiteral("<style>a { text-decoration:none; }</style>") + rich;
}