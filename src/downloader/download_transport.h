#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QSslSocket;
class QTimer;

// Transport of the downloader: N SIMULTANEOUS HTTPS connections.
//
// Why its own and not the one of the app: `Ipv4Https` (src/yt_innertube.cpp)
// is a single socket with a FIFO queue, so it cannot download in parallel,
// and its callback is (status, body): it exposes NO response header (no
// Content-Length, no Content-Range) and cannot send a Range. The downloader
// needs all three.
//
// Why it forces IPv4: the same reason Ipv4Https does (yt_innertube.cpp:341):
// "YouTube blocks certain videos over IPv6 from this network". The host is
// resolved with QHostInfo and the IPv4 address is used explicitly, with the
// TLS verification name set to the HOST name (not the address).
//
// The downloader only fetches from googlevideo.com, so the API is small on
// purpose: a GET of an absolute URL, with an optional Range, and a callback
// with the body, the status and the headers.
class DownloadTransport : public QObject
{
    Q_OBJECT

public:
    // Headers of the response, lower-cased keys (so "content-length" and
    // "Content-Length" are the same).
    using Headers = QHash<QString, QByteArray>;

    struct Response
    {
        int status = 0;
        // The body, DECODED. YouTube answers the media segments with
        // "Transfer-Encoding: chunked" and NO Content-Length (measured on a
        // real segment), so the chunks have to be decoded to get the real
        // bytes: without that the file would carry the chunk headers inside
        // and the length would never be known.
        QByteArray body;
        Headers headers;
        QString error;  // empty when the request went through
        bool chunked = false;

        qint64 contentLength() const;
        bool isChunked() const { return chunked; }
        bool isOk() const { return error.isEmpty() && status >= 200 && status < 300; }
    };

    using DoneFn = std::function<void(const Response &)>;

    explicit DownloadTransport(QObject *parent = nullptr);
    ~DownloadTransport() override;

    // Starts a GET of an absolute https URL. The callback runs in the thread
    // of this object (the main one). Only one request per object at a time:
    // the pool is made of N objects, one per connection.
    //
    // rangeStart/rangeEnd are inclusive byte offsets, or -1 for a whole
    // resource. A ranged request answers 206 with a Content-Range header.
    void get(const QUrl &url, qint64 rangeStart, qint64 rangeEnd,
             const QString &ua, DoneFn done);

    // Aborts the request in progress (if any). The callback is NOT called.
    void abort();

    bool isBusy() const { return m_inFlight; }

    // How many connections the downloader should open at the same time.
    // The measurement behind it: the segments of the master are ~1.3 MB, and
    // with 8 in parallel the download ran at 60 MB/s against 0.37 MB/s
    // sustained (160x). 4 is the default to be less greedy with the
    // connection count, 8 is the measured sweet spot.
    static int defaultConcurrency();

    // The User-Agent the app uses for googlevideo requests.
    static QString userAgent();

private:
    void start();
    void onEncrypted();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void onTimeout();
    void finish();
    void fail(const QString &reason);
    void parseHeaders();
    qint64 headerValue(const QString &name) const;
    bool decodeChunked();

    QSslSocket *m_socket = nullptr;
    QTimer *m_timeout = nullptr;
    QUrl m_url;
    qint64 m_rangeStart = -1;
    qint64 m_rangeEnd = -1;
    QString m_userAgent;
    DoneFn m_done;
    QByteArray m_response;   // everything received, header included
    QByteArray m_decoded;    // the body, without the chunk headers
    DownloadTransport::Headers m_headers;
    int m_status = 0;
    int m_headerSize = -1;
    int m_bodyPos = 0;    // where the chunked body starts being decoded
    bool m_chunked = false;
    bool m_inFlight = false;
    bool m_finished = false;
};
