#include "hls_server.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>

HlsServer::HlsServer(QObject *parent)
    : QObject(parent)
{
}

HlsServer::~HlsServer()
{
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
    }
}

QString HlsServer::masterDirPath()
{
    const QString dir = QDir::tempPath() + QStringLiteral("/nevertube/master");
    QDir().mkpath(dir);
    return dir;
}

QString HlsServer::start()
{
    if (m_server)
        return m_origin;

    masterDirPath();
    auto *server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, 0)) {
        delete server;
        return QString();
    }
    m_server = server;
    m_origin = QStringLiteral("http://127.0.0.1:%1").arg(server->serverPort());
    connect(server, &QTcpServer::newConnection,
            this, &HlsServer::onNewConnection);
    return m_origin;
}

void HlsServer::onNewConnection()
{
    static QHash<QTcpSocket *, QByteArray> buffers;
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
            auto *buf = &buffers[socket];
            buf->append(socket->readAll());
            const int end = buf->indexOf("\r\n\r\n");
            const int endLf = buf->indexOf("\n\n");
            int limit = -1;
            if (end >= 0)
                limit = end;
            else if (endLf >= 0)
                limit = endLf;
            if (limit < 0)
                return;

            QByteArray headers;
            QByteArray body;
            const QByteArray head = buf->left(limit).trimmed();
            const QByteArray firstLine = head.left(head.indexOf('\n')).trimmed();
            const QList<QByteArray> parts = firstLine.split(' ');
            if (parts.size() >= 2 && parts[0] == QByteArray("GET")) {
                const QByteArray path = parts[1];
                const int slash = path.lastIndexOf('/');
                const QByteArray name = slash >= 0 ? path.mid(slash + 1) : path;
                if (name.endsWith(QByteArray(".m3u8"))) {
                    const QByteArray filePath =
                        (masterDirPath() + QLatin1Char('/')).toUtf8() + name;
                    QFile file(QString::fromUtf8(filePath));
                    if (file.exists() && file.open(QIODevice::ReadOnly)) {
                        body = file.readAll();
                        headers =
                            QByteArrayLiteral(
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: application/vnd.apple.mpegurl\r\n"
                                "Connection: close\r\n")
                            + QByteArrayLiteral("Content-Length: ")
                            + QByteArray::number(body.size())
                            + QByteArrayLiteral("\r\n\r\n");
                    }
                }
            }
            if (headers.isEmpty()) {
                headers = QByteArrayLiteral(
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n");
                body.clear();
            }
            socket->write(headers + body);
            socket->flush();
            socket->disconnectFromHost();
            buffers.remove(socket);
        });
        connect(socket, &QTcpSocket::disconnected, socket, [socket]() {
            buffers.remove(socket);
            socket->deleteLater();
        });
        connect(socket, &QTcpSocket::errorOccurred, socket, [socket]() {
            buffers.remove(socket);
            socket->deleteLater();
        });
    }
}