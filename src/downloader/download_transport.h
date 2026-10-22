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
    // Called with the NEW bytes, while the response is still arriving. It is
    // what makes the progress bar of a download MOVE: the whole response of
    // a segment is ~1.3 MB, so waiting for it to finish would make the bar
    // jump every 1.3 MB instead of advancing smoothly.
    //
    // The chunk is the raw body (the chunked transfer is NOT decoded here:
    // the decoding happens when the response is complete, because a chunk
    // can be split in the middle). The bytes counted with it are therefore
    // an approximation until the response ends, which is enough for a bar.
    using ProgressFn = std::function<void(const QByteArray &chunk)>;

    explicit DownloadTransport(QObject *parent = nullptr);
    ~DownloadTransport() override;

    // The object in whose thread the callbacks (done/progress) run.
    //
    // The transport LIVES in a worker thread: that is where the socket, the
    // TLS and the buffers are handled, because doing that on the thread of
    // the interface takes the frames away from the player (measured: with 8
    // connections the main thread was decrypting and copying ~30 MB/s while
    // it had to paint).
    //
    // The callbacks, though, touch the state of the job (the counters, the
    // file of the fragment, the queue of the next fragments), which is the
    // job's and belongs to the thread of the interface: with this context
    // they are POSTED there instead of being called from the worker, so the
    // order is kept and nothing is touched from two threads.
    //
    // Null = the callbacks run in the thread of the transport (what the
    // selftests and a synchronous use want).
    void setCallbackContext(QObject *context) { m_context = context; }

    // Starts a GET of an absolute https URL. The callback runs in the thread
    // of setCallbackContext() (or of this object when it is not set). Only
    // one request per object at a time: the pool is made of N objects, one
    // per connection.
    //
    // rangeStart/rangeEnd are inclusive byte offsets, or -1 for a whole
    // resource. A ranged request answers 206 with a Content-Range header.
    // `expectedBytes` is the size of the range that is asked (0 = unknown).
    // It is what the rule of the discard of a slow fragment compares with
    // (the transport knows how many bytes were asked for).
    void get(const QUrl &url, qint64 rangeStart, qint64 rangeEnd,
             const QString &ua, DoneFn done, ProgressFn progress = nullptr,
             qint64 expectedBytes = 0);

    // Aborts the request in progress (if any). The callback is NOT called.
    void abort();

    // Only valid INSIDE the thread of the transport: it is written by get()
    // and by the answer of the socket. The job keeps its own set (m_busy),
    // because it decides which transport is free from another thread.
    bool isBusy() const { return m_inFlight; }

    // The rules of the discard of a fragment that does not advance.
    //
    // The CDN serves SOME of the requests at a crawl (a few KB/s). Those
    // hold a connection of the pool for the whole request and the speed of
    // the download falls (measured: it started at 45 MB/s and settled at
    // 20). The fragment is discarded and asked again.
    //
    // The rule is NOT "not one byte in N seconds" (that killed the
    // fragments that were arriving fine but with a gap in the middle): it is
    // "not even the stallMinRatio() of the expected bytes in
    // stallTimeoutMs()". A fragment that advances, even slowly, is left
    // alone.
    static constexpr int stallTimeoutMs() { return 12000; }
    static constexpr double stallMinRatio() { return 0.10; }

    // The rule itself, in one place (and testable): has the request advanced
    // enough at the deadline not to be discarded? `received` is what arrived
    // so far and `expected` the size of the range that was asked (0 =
    // unknown, and then nothing is discarded: a video is never dropped by
    // ignorance).
    static bool stallIsEnough(qint64 received, qint64 expected);

    // How many connections the downloader should open at the same time.
    // The measurement behind it: the segments of the master are ~1.3 MB, and
    // with 8 in parallel the download ran at 60 MB/s against 0.37 MB/s
    // sustained (160x). 4 is the default to be less greedy with the
    // connection count, 8 is the measured sweet spot.
    static int defaultConcurrency();

    // The User-Agent the app uses for googlevideo requests.
    static QString userAgent();

private:
    // The callbacks, delivered in the thread of m_context when there is one
    // (see setCallbackContext).
    void deliverDone(const Response &response);
    void deliverProgress(const QByteArray &chunk);
    void start();
    void onEncrypted();
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void onTimeout();
    // The request is not advancing: it is killed (the job asks the fragment
    // again).
    void onStall();
    void finish();
    void fail(const QString &reason);
    void parseHeaders();
    qint64 headerValue(const QString &name) const;
    bool decodeChunked();

    QSslSocket *m_socket = nullptr;
    QTimer *m_timeout = nullptr;
    // The timer of the discard: it checks that the request advances.
    QTimer *m_stallTimer = nullptr;
    // The size of the range that was asked (0 = unknown).
    qint64 m_expectedBytes = 0;
    // The bytes that arrived since the request started.
    qint64 m_receivedBytes = 0;
    QUrl m_url;
    qint64 m_rangeStart = -1;
    qint64 m_rangeEnd = -1;
    QString m_userAgent;
    DoneFn m_done;
    ProgressFn m_progress;
    QByteArray m_response;   // everything received, header included
    int m_reportedBytes = 0; // how much of the body was already reported
    QByteArray m_decoded;    // the body, without the chunk headers
    // The object in whose thread the callbacks run (null = this thread).
    QObject *m_context = nullptr;
    DownloadTransport::Headers m_headers;
    int m_status = 0;
    int m_headerSize = -1;
    int m_bodyPos = 0;    // where the chunked body starts being decoded
    bool m_chunked = false;
    bool m_inFlight = false;
    bool m_finished = false;
};
