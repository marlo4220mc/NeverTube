#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QSslSocket;
class QTimer;
class QFile;

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
    // Called with HOW MANY new bytes arrived, while the response is still
    // coming, and NOT with the bytes themselves.
    //
    // It used to hand over the chunk: the job posted it to the thread of the
    // interface and wrote it there, so the GUI thread was writing the ~45 MB/s
    // of the .part files (plus a QFileInfo of every fragment, O(n^2)) and the
    // player fell to 2-5 fps. The file is written HERE now (see sinkPath in
    // get()); the interface only sees a number, which is all the bar needs.
    //
    // The count is of the raw body (the chunked transfer is NOT decoded here:
    // the decoding happens when the response is complete, because a chunk can
    // be split in the middle). It is therefore an approximation until the
    // response ends, which is enough for a bar.
    using ProgressFn = std::function<void(qint64 bytes)>;

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
    // queue of the next fragments), which is the job's and belongs to the
    // thread of the interface: with this context they are POSTED there
    // instead of being called from the worker, so the order is kept and
    // nothing is touched from two threads. The BYTES of the fragment are the
    // exception: they are written by the transport in its own thread (see
    // sinkPath in get()), so they never pass through the interface.
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
    //
    // `sinkPath`, when it is not empty, is the file the body is WRITTEN to:
    // the file is opened, written and closed in the thread of the transport,
    // which is the whole point of it. The thread of the interface must not
    // receive the data: it is the one that paints the player, and writing the
    // fragments there (measured: 465 files and ~1.8 GB in 40 s) took the
    // frames away (2-5 fps) even with the sockets already in a worker thread.
    // Empty = the bytes are only counted (the selftests and a synchronous use
    // want that), and Response::body keeps carrying the body.
    void get(const QUrl &url, qint64 rangeStart, qint64 rangeEnd,
             const QString &ua, DoneFn done, ProgressFn progress = nullptr,
             qint64 expectedBytes = 0, const QString &sinkPath = QString());

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
    void deliverProgress(qint64 bytes);
    // The new body bytes: written to the sink in THIS thread (the thread of
    // the transport) and only counted in the interface.
    void deliverBody(const QByteArray &chunk);
    bool openSink();
    void closeSink();
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
    // The file of the fragment: opened, written and closed in the thread of
    // the transport (null = the body is only counted).
    QString m_sinkPath;
    QFile *m_sink = nullptr;
    // The progress is COALESCED: a request at ~45 MB/s delivers a chunk every
    // millisecond, and posting one event per chunk to the interface is work
    // for the interface. 100 ms (or 256 KB) is smooth for a bar and it is
    // ~10 events per second per connection instead of ~700.
    qint64 m_pendingProgress = 0;
    qint64 m_lastProgressMs = 0;
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
