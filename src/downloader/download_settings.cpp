#include "download_settings.h"

#include <QStringList>

namespace downloadsettings {

int connectionsOf(const QString &value)
{
    bool ok = false;
    const int n = value.toInt(&ok);
    if (!ok)
        return kDefaultConnections;
    if (n < kMinConnections)
        return kMinConnections;
    if (n > kMaxConnections)
        return kMaxConnections;
    return n;
}

QString valueForConnections(int connections)
{
    return QString::number(connectionsOf(QString::number(connections)));
}

} // namespace downloadsettings
