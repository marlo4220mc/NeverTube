#include "download_transport.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QSslSocket>
#include <QTimer>

qint64 DownloadTransport::Response::contentLength() const
{
    const QByteArray v = headers.value(QStringLiteral("content-length"));
    if (v.isEmpty())
        return -1;
    bool ok = false;
    const qint64 n = v.toLongLong(&ok);
    return ok ? n : -1;
}

DownloadTransport::DownloadTransport(QObject *parent)
    : QObject(parent)
    , m_timeout(new QTimer(this))
{
    m_timeout->setSingleShot(true);
    // 60 s: the media playlists are small, but the network can be slow and
    // a timeout here would fail the whole download for a moment of slowness.
    m_timeout->setInterval(60000);
    connect(m_timeout, &QTimer::timeout, this, &DownloadTransport::onTimeout);
}

DownloadTransport::~DownloadTransport()
{
    abort();
}

// How many connections the downloader opens at the same time.
//
// The speed of a download is limited by TWO things, and they are different:
//
//  1. The pacing that YouTube signs into the URL (initcwndbps, measured
//     here: 2117625 bytes/s = 2,1 MB/s). It is PER CONNECTION, so more
//     connections means more speed, up to the point where the connection
//     itself becomes the limit.
//  2. The throttle by the size of the request: a request of more than ~8 MB
//     drops to 0,36 MB/s (measured on two different IPs). Every fragment
//     of a download is well under that, so this one is avoided by
//     construction.
//
// With a fresh IP, 8 connections gave 15 - 24 MB/s and 4 gave 15 MB/s;
// with 12 the connection itself became the limit (and it was not
// reliable). 8 is the measured optimum.
//
// The absolute number depends on the state of the IP: the same
// configuration gave 17 MB/s on a fresh IP and 2 - 4 MB/s after a long
// run of downloads, so the speed is NOT a property of the app.
int DownloadTransport::defaultConcurrency()
{
    return 8;
}

QString DownloadTransport::userAgent()
{
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                          "AppleWebKit/537.36 (KHTML, like Gecko) "
                          "Chrome/120.0.0.0 Safari/537.36");
}

void DownloadTransport::get(const QUrl &url, qint64 rangeStart, qint64 rangeEnd,
                            const QString &ua, DoneFn done, ProgressFn progress)
{
    abort();
    m_url = url;
    m_rangeStart = rangeStart;
    m_rangeEnd = rangeEnd;
    m_userAgent = ua.isEmpty() ? userAgent() : ua;
    m_done = std::move(done);
    m_progress = std::move(progress);
    m_inFlight = true;
    m_finished = false;
    m_response.clear();
    m_decoded.clear();
    m_headers.clear();
    m_status = 0;
    m_headerSize = -1;
    m_bodyPos = 0;
    m_chunked = false;
    start();
}

void DownloadTransport::abort()
{
    m_done = nullptr;
    m_progress = nullptr;
    m_inFlight = false;
    m_finished = true;
    if (m_timeout)
        m_timeout->stop();
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void DownloadTransport::start()
{
    const QString host = m_url.host();
    if (host.isEmpty()) {
        fail(QStringLiteral("URL sin host"));
        return;
    }

    m_socket = new QSslSocket(this);
    QSslSocket *socket = m_socket;
    connect(socket, &QSslSocket::encrypted, this, [this, socket]() {
        if (socket == m_socket)
            onEncrypted();
    });
    connect(socket, &QIODevice::readyRead, this, [this, socket]() {
        if (socket == m_socket)
            onReadyRead();
    });
    connect(socket, &QAbstractSocket::disconnected, this, [this, socket]() {
        if (socket == m_socket)
            onDisconnected();
    });
    connect(socket, &QAbstractSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
                if (socket == m_socket)
                    onSocketError();
            });

    m_timeout->start();

    // Resolve the host to an IPv4 address, like Ipv4Https does: YouTube
    // blocks some videos over IPv6 from this network, and the TLS
    // verification name is the HOST name, not the address.
    QHostInfo::lookupHost(host, this, [this, socket, host](const QHostInfo &info) {
        if (socket != m_socket)
            return;
        QHostAddress address;
        const QList<QHostAddress> addrs = info.addresses();
        for (const QHostAddress &a : addrs) {
            if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                address = a;
                break;
            }
        }
        if (address.isNull()) {
            for (const QHostAddress &a : addrs) {
                if (a.protocol() == QAbstractSocket::IPv6Protocol) {
                    address = a;
                    break;
                }
            }
        }
        if (address.isNull()) {
            fail(QStringLiteral("sin IP para %1").arg(host));
            return;
        }
        if (socket != m_socket)
            return;
        socket->setPeerVerifyName(host);
        socket->connectToHostEncrypted(address.toString(), 443);
    });
}

void DownloadTransport::onEncrypted()
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    // The request line must send the server EXACTLY what the caller wrote:
    // path() and query() return the DECODED form, so a percent-encoded
    // character comes back raw and the endpoint answers 400. FullyEncoded
    // keeps the original bytes.
    QString path = m_url.path(QUrl::FullyEncoded);
    if (m_url.hasQuery())
        path += QLatin1Char('?') + m_url.query(QUrl::FullyEncoded);

    QByteArray request;
    request += "GET " + path.toUtf8() + " HTTP/1.1\r\n";
    request += "Host: " + m_url.host().toUtf8() + "\r\n";
    request += "User-Agent: " + m_userAgent.toUtf8() + "\r\n";
    request += "Accept-Encoding: identity\r\n";
    if (m_rangeStart >= 0) {
        QByteArray range = "bytes=" + QByteArray::number(m_rangeStart) + '-';
        if (m_rangeEnd >= m_rangeStart)
            range += QByteArray::number(m_rangeEnd);
        request += "Range: " + range + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    m_socket->write(request);
    m_socket->flush();
}

void DownloadTransport::onReadyRead()
{
    if (!m_socket)
        return;
    m_response += m_socket->readAll();
    if (m_headerSize < 0) {
        const int headerEnd = m_response.indexOf("\r\n\r\n");
        if (headerEnd >= 0) {
            m_headerSize = headerEnd + 4;
            m_bodyPos = m_headerSize;
            m_reportedBytes = 0;
            parseHeaders();
        }
    }
    if (m_headerSize < 0)
        return;

    // The new bytes of the body, so the bar moves while the segment is
    // coming. The chunked transfer is NOT decoded here (a chunk can be
    // split in the middle): the count is an approximation until the
    // response ends, which is enough for a bar.
    if (m_progress && m_response.size() > m_headerSize + m_reportedBytes) {
        const QByteArray chunk =
            m_response.mid(m_headerSize + m_reportedBytes);
        m_reportedBytes += chunk.size();
        m_progress(chunk);
    }

    if (m_chunked) {
        // The body is a stream of chunks: "size\r\nDATA\r\n" and it ends
        // with a zero-size chunk. The size is what tells the end, so the
        // response can be decoded as it arrives.
        if (decodeChunked())
            finish();
        return;
    }

    // With Content-Length the end of the body is known. Without it the
    // connection close marks the end.
    const qint64 length = headerValue(QStringLiteral("content-length"));
    if (length >= 0 && m_response.size() >= m_headerSize + length) {
        finish();
        return;
    }
}

void DownloadTransport::parseHeaders()
{
    const QByteArray head = m_response.left(m_headerSize);
    const QByteArrayList headLines = head.split('\n');
    if (!headLines.isEmpty()) {
        const QByteArrayList statusParts = headLines.first().trimmed().split(' ');
        if (statusParts.size() >= 2)
            m_status = statusParts.at(1).toInt();
    }
    for (int i = 1; i < headLines.size(); ++i) {
        const QByteArray line = headLines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        m_headers.insert(key, line.mid(colon + 1).trimmed());
    }
    m_chunked = QString::fromLatin1(
                    m_headers.value(QStringLiteral("transfer-encoding")))
                    .toLower()
                    .contains(QLatin1String("chunked"));
}

qint64 DownloadTransport::headerValue(const QString &name) const
{
    const QByteArray v = m_headers.value(name);
    if (v.isEmpty())
        return -1;
    bool ok = false;
    const qint64 n = v.toLongLong(&ok);
    return ok ? n : -1;
}

// Decodes the chunked body that is in the buffer.
//
// The decoding is INCREMENTAL: every call takes what it can from the buffer
// (m_response, from m_bodyPos) and appends it to m_decoded, so the bytes are
// decoded exactly once even if the response arrives in many pieces.
//
// Returns true when the last chunk (the zero-size one) has arrived, that
// is, when the body is complete.
bool DownloadTransport::decodeChunked()
{
    for (;;) {
        // The chunk size is a line in hexadecimal, optionally followed by
        // extensions after a ';'.
        const int eol = m_response.indexOf("\r\n", m_bodyPos);
        if (eol < 0)
            return false;  // the size line is not complete yet
        const QByteArray sizeLine = m_response.mid(m_bodyPos, eol - m_bodyPos);
        const int semi = sizeLine.indexOf(';');
        const QByteArray hex = (semi >= 0 ? sizeLine.left(semi) : sizeLine).trimmed();
        bool ok = false;
        const qint64 size = hex.toLongLong(&ok, 16);
        if (!ok)
            return false;
        const int dataPos = eol + 2;
        if (size == 0) {
            // The last chunk: the body is complete.
            m_bodyPos = dataPos;
            return true;
        }
        if (m_response.size() < dataPos + size + 2)
            return false;  // the chunk is not complete yet
        m_decoded += m_response.mid(dataPos, int(size));
        m_bodyPos = dataPos + int(size) + 2;  // skip the data and its CRLF
    }
}

void DownloadTransport::onDisconnected()
{
    if (m_finished)
        return;
    finish();
}

void DownloadTransport::onSocketError()
{
    if (m_finished || !m_socket)
        return;
    // The server closes the connection when it is done (the request sends
    // "Connection: close") and Qt reports that as an error. When the
    // response has ALREADY arrived that is not a failure: it is the end of
    // the body. Failing here would throw away the bytes that came.
    if (m_headerSize >= 0) {
        finish();
        return;
    }
    fail(m_socket->errorString());
}

void DownloadTransport::onTimeout()
{
    if (m_finished)
        return;
    fail(QStringLiteral("timeout HTTPS"));
}

void DownloadTransport::finish()
{
    if (m_finished)
        return;
    m_finished = true;
    m_inFlight = false;
    m_timeout->stop();

    Response response;
    if (m_headerSize < 0) {
        response.error = QStringLiteral("respuesta sin cabecera");
    } else {
        response.status = m_status;
        response.headers = m_headers;
        response.chunked = m_chunked;
        // The chunked body was decoded while it arrived; otherwise the body
        // is the raw bytes after the header.
        response.body = m_chunked ? m_decoded : m_response.mid(m_headerSize);
    }

    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    DoneFn done = std::move(m_done);
    m_done = nullptr;
    if (done)
        done(response);
}

void DownloadTransport::fail(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    m_inFlight = false;
    m_timeout->stop();
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    Response response;
    response.error = reason;
    DoneFn done = std::move(m_done);
    m_done = nullptr;
    if (done)
        done(response);
}
