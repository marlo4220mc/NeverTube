#include "download_transport.h"

#include "../debug.h"
#include "../i18n.h"

#include <QHostAddress>
#include <QDateTime>
#include <QFile>
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

    // m_stallTimer is NOT created, on purpose. It used to be declared and
    // never created, so the discard of a fragment that does not advance never
    // ran (see onStall()); it is left that way DELIBERATELY: measured, the
    // sidx of a real video gives fragments of ~60 KB, so a download at 20 MB/s
    // asks 300+ times per second, YouTube punishes that rate by leaving
    // requests without an answer (0 bytes for 12 s), and discarding them made
    // the download go from 20 MB/s to 0.9 MB/s. The fix is the SIZE of the
    // request (merge consecutive fragments), not discarding them; when that is
    // done, arming this is two lines (new QTimer + connect here).
}

bool DownloadTransport::stallIsEnough(qint64 received, qint64 expected)
{
    if (expected <= 0)
        return true;   // no threshold to compare with: never discarded
    return double(received) >= double(expected) * stallMinRatio();
}

DownloadTransport::~DownloadTransport()
{
    // No callback can run from a destructor: the job would be called with an
    // object that is being destroyed.
    m_done = nullptr;
    m_progress = nullptr;
    closeSink();
    if (m_socket) {
        // Destroyed HERE and not with deleteLater(): the thread of the
        // transports quits right after this, and a DeferredDelete left
        // pending would never be processed (the socket and its descriptor
        // would leak on every stop).
        m_socket->abort();
        delete m_socket;
        m_socket = nullptr;
    }
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
                            const QString &ua, DoneFn done, ProgressFn progress,
                            qint64 expectedBytes, const QString &sinkPath)
{
    abort();
    m_expectedBytes = expectedBytes;
    m_receivedBytes = 0;
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
    m_reportedBytes = 0;
    m_sinkPath = sinkPath;
    m_pendingProgress = 0;
    m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    // The file of the fragment is opened BEFORE the request: a folder that is
    // not writable has to fail here, not after downloading ~1 GB.
    if (!openSink()) {
        const QString reason =
            QStringLiteral("no se pudo abrir el archivo del fragmento");
        fail(reason);
        return;
    }
    start();
}

bool DownloadTransport::openSink()
{
    closeSink();
    if (m_sinkPath.isEmpty())
        return true;
    m_sink = new QFile(m_sinkPath);
    if (!m_sink->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete m_sink;
        m_sink = nullptr;
        m_sinkPath.clear();
        return false;
    }
    return true;
}

void DownloadTransport::closeSink()
{
    if (!m_sink)
        return;
    // close() flushes the file. A write that failed was already treated as a
    // failure of the fragment in deliverBody(), so there is nothing to
    // inspect here: the fragment is only "done" when every byte was written.
    m_sink->close();
    delete m_sink;
    m_sink = nullptr;
}

void DownloadTransport::abort()
{
    m_done = nullptr;
    m_progress = nullptr;
    m_inFlight = false;
    m_finished = true;
    closeSink();
    m_pendingProgress = 0;
    if (m_timeout)
        m_timeout->stop();
    if (m_stallTimer)
        m_stallTimer->stop();
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
    if (m_stallTimer)
        m_stallTimer->start();

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
    // The count is taken from the bytes that JUST arrived: bytesAvailable()
    // is read AFTER readAll() and it is always 0 then, so the rule of the
    // discard saw "nothing arrived" in every request.
    const QByteArray incoming = m_socket->readAll();
    m_receivedBytes += incoming.size();
    m_response += incoming;
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

    // The new bytes of the body. They are WRITTEN here (the thread of the
    // transport) and only counted for the interface: the interface writing
    // them is what took the frames away from the player.
    //
    // The chunked transfer is NOT decoded here (a chunk can be split in the
    // middle); the count is an approximation until the response ends, which
    // is enough for a bar.
    const int newBody = m_response.size() - (m_headerSize + m_reportedBytes);
    if (newBody > 0) {
        const QByteArray chunk =
            m_response.mid(m_headerSize + m_reportedBytes);
        m_reportedBytes += chunk.size();
        deliverBody(chunk);
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

// The request is not advancing: it is killed and the fragment is asked
// again (that is what keeps the speed when the CDN serves some of the
// requests at a crawl).
void DownloadTransport::onStall()
{
    if (m_finished)
        return;
    m_stallTimer->stop();
    // The comparison is the rule of stallIsEnough(): before it was
    // `m_receivedBytes * 10 >= m_expectedBytes * int(stallMinRatio())`, whose
    // right side is 0 (int(0.10) is 0), so it was true always.
    if (stallIsEnough(m_receivedBytes, m_expectedBytes))
        return;  // it IS advancing (slowly, but advancing): it is left alone
    NT_DEBUG("E2E dl stall recibido=%lld esperado=%lld -> se descarta\n",
             m_receivedBytes, m_expectedBytes);
    fail(nt::tr("error.dlStalled"));
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
    if (m_stallTimer)
        m_stallTimer->stop();

    Response response;
    if (m_headerSize < 0) {
        response.error = QStringLiteral("respuesta sin cabecera");
    } else {
        response.status = m_status;
        response.headers = m_headers;
        response.chunked = m_chunked;
        // The body is carried only when nobody wrote it to a file: with a
        // sink the bytes are already on the disk, and copying ~4 MB per
        // fragment just to free them in the thread of the interface is work
        // for nothing (and a 4 MB allocation in the GUI thread per fragment).
        if (m_sinkPath.isEmpty())
            response.body = m_chunked ? m_decoded : m_response.mid(m_headerSize);
    }

    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    // The file of the fragment is closed HERE and before the answer is
    // posted: the job measures it as soon as the fragment is done.
    closeSink();
    if (m_pendingProgress > 0)
        deliverProgress(m_pendingProgress);
    m_pendingProgress = 0;
    // No more chunks after the end of the response.
    m_progress = nullptr;
    deliverDone(response);
}

// The callback runs in the thread of m_context when it is set: the job (and
// its counters and its queue) is the job's, and it belongs to the thread of
// the interface. The transport itself lives in a worker thread, so calling it
// straight from here would touch the state of the job from the wrong thread.
// The bytes are NOT in this path: they were already written by the transport,
// in its own thread, when this is posted.
//
// A queued call also keeps the ORDER: the progress of a fragment is posted
// before its done, and both go to the same object.
void DownloadTransport::deliverDone(const Response &response)
{
    DoneFn done = std::move(m_done);
    m_done = nullptr;
    if (!done)
        return;
    if (!m_context) {
        done(response);
        return;
    }
    QMetaObject::invokeMethod(m_context, [done, response]() { done(response); },
                              Qt::QueuedConnection);
}

void DownloadTransport::deliverBody(const QByteArray &chunk)
{
    // The bytes go to the disk in THIS thread (the thread of the transport).
    // Handing them to the thread of the interface is what made the GUI thread
    // write ~45 MB/s of fragments while it had to paint the player (measured:
    // 2-5 fps with a 41 s freeze at the start of every download).
    if (m_sink) {
        const qint64 written = m_sink->write(chunk);
        if (written != chunk.size()) {
            const QString reason = QStringLiteral("no se pudo escribir el fragmento en el disco");
            fail(reason);
            return;
        }
    }

    m_pendingProgress += chunk.size();
    if (!m_progress)
        return;
    // The progress of a request at ~45 MB/s arrives every millisecond: one
    // event per chunk is ~700 events per second for the interface. Coalesced,
    // it is ~10 per second per connection, which is smooth for a bar.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_pendingProgress < 256 * 1024 && now - m_lastProgressMs < 100)
        return;
    const qint64 bytes = m_pendingProgress;
    m_pendingProgress = 0;
    m_lastProgressMs = now;
    deliverProgress(bytes);
}

void DownloadTransport::deliverProgress(qint64 bytes)
{
    if (!m_progress || bytes <= 0)
        return;
    if (!m_context) {
        m_progress(bytes);
        return;
    }
    ProgressFn progress = m_progress;
    QMetaObject::invokeMethod(m_context, [progress, bytes]() { progress(bytes); },
                              Qt::QueuedConnection);
}

void DownloadTransport::fail(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    m_inFlight = false;
    m_timeout->stop();
    if (m_stallTimer)
        m_stallTimer->stop();
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    // The partial file goes away with the request: the job asks the fragment
    // again and it is opened with Truncate, so half a fragment must not stay
    // on the disk as if it were a finished one.
    closeSink();
    if (!m_sinkPath.isEmpty())
        QFile::remove(m_sinkPath);
    m_pendingProgress = 0;
    Response response;
    response.error = reason;
    m_progress = nullptr;
    deliverDone(response);
}
