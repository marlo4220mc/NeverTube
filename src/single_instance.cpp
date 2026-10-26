#include "single_instance.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>

namespace {

// How long the probe waits for the instance that may be there. It is short: it
// runs before the window exists, and when nobody is listening the system says
// so at once (Windows: CreateFile of the pipe fails; Unix: connect to the
// socket file).
constexpr int kConnectMs = 300;
constexpr int kWriteMs = 1000;
// How long the second process waits for the instance that is running to CLOSE
// the connection, which is its way of saying "I have the line". It is not
// decoration: on Windows a client that closes the pipe right after writing can
// make the server lose the bytes (measured in CI: the first handover and the
// one after it both failed, with the first instance listening all the time).
// When the instance is still starting (its event loop is not running yet) the
// wait expires and the process exits anyway: it cannot take the name, the
// server has it.
constexpr int kAckMs = 2000;
// The arguments of a process are small. A client that sends more is not one of
// ours (or it is broken), and its buffer must not grow without a limit.
constexpr int kMaxPayload = 64 * 1024;

constexpr char kLineEnd = '\n';

void sendTo(QLocalSocket *socket, const QStringList &args)
{
    // One line of JSON: the array of the arguments, compact. A line (and not a
    // length prefix) because the receiver only needs "the line is complete".
    const QByteArray payload =
        QJsonDocument(QJsonArray::fromStringList(args))
            .toJson(QJsonDocument::Compact)
        + kLineEnd;
    socket->write(payload);
    socket->flush();
    socket->waitForBytesWritten(kWriteMs);
    // Do NOT close before the other side has read it: the server closes the
    // connection when the line is complete, and that close is the ack.
    socket->waitForDisconnected(kAckMs);
}

} // namespace

QString SingleInstance::name()
{
    QString user = qEnvironmentVariable("USERNAME");   // Windows
    if (user.isEmpty())
        user = qEnvironmentVariable("USER");           // Unix
    if (user.isEmpty())
        user = QDir::home().dirName();

    // The name is a FILE on Unix (in the runtime directory): no separators and
    // no characters that break a path. On Windows it is a pipe name and the
    // same cleanup does no harm.
    QString clean;
    clean.reserve(user.size());
    for (const QChar c : user) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-'))
            clean.append(c);
        else
            clean.append(QLatin1Char('_'));
    }
    if (clean.isEmpty())
        clean = QStringLiteral("user");

    return QStringLiteral("nevertube-") + clean.left(64);
}

SingleInstance::SingleInstance(const QString &name, QObject *parent)
    : QObject(parent)
    , m_name(name)
{
}

SingleInstance::~SingleInstance()
{
    if (!m_server)
        return;
    m_server->close();
    // Unix: the file of the socket is left behind when the process dies without
    // closing it. removeServer() is a no-op on Windows (the pipe goes away with
    // its process).
    QLocalServer::removeServer(m_name);
}

bool SingleInstance::start(const QStringList &args)
{
    // The probe is the whole answer: if a socket ACCEPTS the connection there
    // is another instance, and the work of this process is to hand it the
    // arguments.
    {
        QLocalSocket probe;
        probe.connectToServer(m_name);
        if (probe.waitForConnected(kConnectMs)) {
            sendTo(&probe, args);
            return false;
        }
    }

    // Nobody answered. A file left by a process that died (Unix) would make
    // listen() fail and the app would never open again.
    QLocalServer::removeServer(m_name);
    m_server = new QLocalServer(this);
    // The payload is the arguments of a process of THIS user: no other user has
    // to be able to talk to it.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_name)) {
        // A pipe that cannot be created (a security policy, a name taken by a
        // process of another user) is not a reason to refuse to open the app:
        // it opens, and two instances is what the app did until now.
        delete m_server;
        m_server = nullptr;
        return true;
    }
    connect(m_server, &QLocalServer::newConnection,
            this, &SingleInstance::onNewConnection);
    return true;
}

void SingleInstance::onNewConnection()
{
    if (!m_server)
        return;
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket]() { readFrom(socket); });
        // A client that connects and says nothing is dropped with its buffer.
        // The check of the hash is what keeps this from deleting a socket that
        // readFrom() already took (it removes the entry first).
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            // What is still in the socket is read BEFORE dropping it: the client
            // can write and close in the same breath (a process that hands its
            // arguments over and exits), and on Windows the disconnect is
            // delivered first, so the pending bytes would go away with the
            // socket and the arguments would never arrive.
            readFrom(socket);
            if (m_buffers.contains(socket)) {
                m_buffers.remove(socket);
                socket->deleteLater();
            }
        });
        // The bytes can be there BEFORE this object sees the socket (the
        // connection was accepted while the window was being built, and the
        // event loop had not run yet). readyRead does not come again for them.
        readFrom(socket);
    }
}

void SingleInstance::readFrom(QLocalSocket *socket)
{
    QByteArray &buffer = m_buffers[socket];
    buffer.append(socket->readAll());
    const int eol = buffer.indexOf(kLineEnd);
    if (eol < 0) {
        if (buffer.size() > kMaxPayload) {
            m_buffers.remove(socket);
            socket->disconnect(this);
            socket->abort();
            socket->deleteLater();
        }
        return;
    }

    const QByteArray line = buffer.left(eol);
    m_buffers.remove(socket);
    socket->disconnect(this);

    const QJsonDocument doc = QJsonDocument::fromJson(line);
    bool wellFormed = doc.isArray();
    QStringList args;
    if (wellFormed) {
        const QJsonArray array = doc.array();
        args.reserve(array.size());
        for (const QJsonValue &value : array) {
            if (!value.isString()) {
                wellFormed = false;
                break;
            }
            args.append(value.toString());
        }
    }
    // An EMPTY list is forwarded on purpose: it is the app opened with no
    // arguments (the icon of the menu), and what it means is "come to the
    // front". A payload that is not an array of strings is not ours: it is
    // dropped, so a broken client cannot make the window jump.
    if (wellFormed)
        emit argumentsReceived(args);

    socket->disconnectFromServer();
    socket->deleteLater();
}
