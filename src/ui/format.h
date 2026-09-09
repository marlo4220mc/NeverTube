#pragma once

#include <QString>

QString fmtTime(double seconds);
QString durationLabel(int seconds);
QString compactCount(qint64 count);
QString publishedLabel(const QString &publishedAt);
QString viewsLabel(qint64 count);