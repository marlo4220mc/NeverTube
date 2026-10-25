#include "youtube_url.h"

#include <QUrl>
#include <QUrlQuery>

#include <cstdio>

namespace {

// The ids of YouTube are 11 characters of [A-Za-z0-9_-]. Everything is checked
// literally: a permissive rule here is how a URL parser starts eating normal
// text ("buscar esto" would not, but an 11-letter word would).
bool isVideoId(const QString &id)
{
    if (id.size() != 11)
        return false;
    for (const QChar c : id) {
        const ushort u = c.unicode();
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                || (u >= '0' && u <= '9') || u == '_' || u == '-';
        if (!ok)
            return false;
    }
    return true;
}

bool isYouTubeHost(const QString &host)
{
    const QString h = host.toLower();
    return h == QLatin1String("youtube.com")
            || h == QLatin1String("www.youtube.com")
            || h == QLatin1String("m.youtube.com")
            || h == QLatin1String("music.youtube.com")
            || h == QLatin1String("youtube-nocookie.com")
            || h == QLatin1String("www.youtube-nocookie.com")
            || h == QLatin1String("youtu.be")
            || h == QLatin1String("www.youtu.be");
}

// The first path segment after the prefix, stopping at the next '/'.
QString segmentAfter(const QString &path, const QString &prefix)
{
    const QString rest = path.mid(prefix.size());
    const int slash = rest.indexOf(QLatin1Char('/'));
    return slash < 0 ? rest : rest.left(slash);
}

} // namespace

QString youtubeVideoIdFromText(const QString &text)
{
    QString s = text.trimmed();
    if (s.isEmpty())
        return QString();

    // Without a scheme QUrl reads the whole thing as a path. Only the hosts of
    // YouTube are completed; any other text stays exactly as it was (so it is
    // searched as it was typed).
    if (!s.contains(QLatin1String("://"))) {
        static const char *const kHosts[] = {
            "youtube.com/",   "www.youtube.com/",   "m.youtube.com/",
            "music.youtube.com/", "youtu.be/",      "www.youtu.be/",
        };
        const QString low = s.toLower();
        for (const char *host : kHosts) {
            if (low.startsWith(QLatin1String(host))) {
                s.prepend(QLatin1String("https://"));
                break;
            }
        }
    }

    const QUrl url(s, QUrl::TolerantMode);
    if (!url.isValid() || url.host().isEmpty())
        return QString();
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return QString();
    if (!isYouTubeHost(url.host()))
        return QString();

    const QString host = url.host().toLower();
    const QString path = url.path();

    if (host == QLatin1String("youtu.be") || host == QLatin1String("www.youtu.be")) {
        const QString id = segmentAfter(path, QStringLiteral("/"));
        return isVideoId(id) ? id : QString();
    }
    if (path == QLatin1String("/watch") || path == QLatin1String("/watch/")) {
        const QString id =
            QUrlQuery(url).queryItemValue(QStringLiteral("v"), QUrl::FullyDecoded);
        return isVideoId(id) ? id : QString();
    }
    static const char *const kPrefixes[] = {"/shorts/", "/embed/", "/live/", "/v/"};
    for (const char *prefix : kPrefixes) {
        const QString p = QLatin1String(prefix);
        if (path.startsWith(p)) {
            const QString id = segmentAfter(path, p);
            return isVideoId(id) ? id : QString();
        }
    }
    return QString();
}

bool youtubeUrlSelfTest()
{
    struct Case
    {
        const char *text;
        const char *expect; // empty = must NOT be taken as a video
    };
    static const Case cases[] = {
        // --- the accepted forms ---
        {"https://www.youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"https://youtube.com/watch?v=dQw4w9WgXcQ&t=42s", "dQw4w9WgXcQ"},
        {"https://m.youtube.com/watch?feature=share&v=dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"https://music.youtube.com/watch?v=dQw4w9WgXcQ&list=RDAMVMdQw4w9WgXcQ",
         "dQw4w9WgXcQ"},
        {"https://youtu.be/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"https://youtu.be/dQw4w9WgXcQ?t=42", "dQw4w9WgXcQ"},
        {"https://www.youtube.com/shorts/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"https://www.youtube.com/embed/dQw4w9WgXcQ?autoplay=1", "dQw4w9WgXcQ"},
        {"https://www.youtube.com/live/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"https://www.youtube.com/v/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"http://youtu.be/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        // Pasted without the scheme (the address bar sometimes drops it).
        {"youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"www.youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        {"youtu.be/dQw4w9WgXcQ", "dQw4w9WgXcQ"},
        // Surrounding blanks (a paste usually brings a newline).
        {"  https://youtu.be/dQw4w9WgXcQ\n", "dQw4w9WgXcQ"},
        // --- must NOT be a video ---
        {"dQw4w9WgXcQ", ""},                             // a bare id is a query
        {"abcdefghijk", ""},                             // 11 letters, not a link
        {"https://www.youtube.com/watch?v=corto", ""},    // not 11 characters
        {"https://www.youtube.com/watch?v=", ""},         // no id at all
        {"https://www.youtube.com/watch", ""},            // no v=
        {"https://www.youtube.com/playlist?list=PL123", ""},
        {"https://www.youtube.com/@unCanal", ""},
        {"https://www.youtube.com/results?search_query=hola", ""},
        {"https://example.com/watch?v=dQw4w9WgXcQ", ""},  // another site
        {"https://notyoutube.com/watch?v=dQw4w9WgXcQ", ""},
        {"ftp://youtu.be/dQw4w9WgXcQ", ""},               // not http
        {"nevertube", ""},                                // ordinary text
        {"hace 2 semanas", ""},                           // ordinary text
        {"", ""},
        {"   ", ""},
    };

    int failed = 0;
    for (const Case &c : cases) {
        const QString got = youtubeVideoIdFromText(QString::fromUtf8(c.text));
        const QString want = QString::fromUtf8(c.expect);
        if (got != want) {
            ++failed;
            std::fprintf(stderr, "[video-url] FAIL \"%s\" -> \"%s\" (esperado \"%s\")\n",
                         c.text, got.toUtf8().constData(), c.expect);
        }
    }
    std::fprintf(stderr, "[video-url] %d/%d casos\n",
                 (int)(sizeof(cases) / sizeof(cases[0])) - failed,
                 (int)(sizeof(cases) / sizeof(cases[0])));
    return failed == 0;
}
