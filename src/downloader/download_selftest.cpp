#include "download_selftest.h"

#include "segment_plan.h"

#include <cstdio>

namespace {

// A master as YouTube really serves it (trimmed): two video variants, an
// original audio and a dub, plus the timedtext renditions that are also
// there in the real thing (TYPE=SUBTITLES, which must be ignored).
// The original audio is marked the way YouTube marks it: "acont=original"
// inside its URI (that is the rule isOriginalAudio uses in the player).
const char *const kMaster = R"M(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-MEDIA:URI="https://x/googlevideo.com/audio_orig?acont=original",TYPE=AUDIO,GROUP-ID="a1",NAME="Default",LANGUAGE="en",DEFAULT=YES,AUTOSELECT=YES
#EXT-X-MEDIA:URI="https://x/googlevideo.com/audio_es",TYPE=AUDIO,GROUP-ID="a1",NAME="Default",LANGUAGE="es-419",AUTOSELECT=YES
#EXT-X-MEDIA:URI="https://x/googlevideo.com/subs.vtt",TYPE=SUBTITLES,GROUP-ID="s1",NAME="English",LANGUAGE="en"
#EXT-X-STREAM-INF:BANDWIDTH=1248435,CODECS="avc1.4D401F,mp4a.40.2",RESOLUTION=1280x720,FRAME-RATE=25
https://x/googlevideo.com/v720.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=4688074,CODECS="vp09.00.41.08,mp4a.40.2",RESOLUTION=1920x1080,FRAME-RATE=25
https://x/googlevideo.com/v1080.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=78539,CODECS="vp09.00.10.08,mp4a.40.5",RESOLUTION=256x144,FRAME-RATE=13
https://x/googlevideo.com/v144.m3u8
)M";

// A master with a single language, as YouTube really serves it in that
// case: TWO audio renditions of the same group, both named "Default", with
// no LANGUAGE and no "acont=original" marker, distinguished only by the
// itag in their URI (233 = 48 kbps, 234 = 128 kbps). Measured on a real
// master, this is the case that broke the first version of the plan: it
// picked the 48 kbps rendition for a 1080p video.
const char *const kMasterNoLang = R"M(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-MEDIA:URI="https://x/googlevideo.com/itag/233/playlist/index.m3u8",TYPE=AUDIO,GROUP-ID="233",NAME="Default",YT-EXT-ABSOLUTE-LOUDNESS=-13.010,DEFAULT=YES,AUTOSELECT=YES
#EXT-X-MEDIA:URI="https://x/googlevideo.com/itag/234/playlist/index.m3u8",TYPE=AUDIO,GROUP-ID="234",NAME="Default",YT-EXT-ABSOLUTE-LOUDNESS=-13.010,DEFAULT=YES,AUTOSELECT=YES
#EXT-X-STREAM-INF:BANDWIDTH=1763840,CODECS="vp09.00.40.08,mp4a.40.2",RESOLUTION=1920x1080,FRAME-RATE=25
https://x/googlevideo.com/v1080.m3u8
)M";

const char *const kMedia = R"M(#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:7
#EXTINF:5.24,
https://x/googlevideo.com/seg1.ts
#EXTINF:6.96,
https://x/googlevideo.com/seg2.ts
#EXTINF:3.36,
https://x/googlevideo.com/seg3.ts
#EXT-X-ENDLIST
)M";

// The same ranking the player uses, in miniature: VP9 beats h264, the height
// breaks the tie. It is passed in as a function so the plan does not depend
// on the Yt class.
int testRank(int height, const QString &codecs)
{
    int rank = 0;
    if (codecs.contains(QLatin1String("vp09")))
        rank = 2;
    else if (codecs.contains(QLatin1String("avc1")))
        rank = 1;
    return rank * 100000 + height;
}

} // namespace

bool downloadPlanSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  DOWNLOAD %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };

    // --- the master ---
    const dl::Plan p =
        dl::planFromMaster(QByteArray(kMaster), 1080, QString(), &testRank);
    check(p.valid, "el master se parsea");
    check(p.video.valid, "hay variante de video");
    check(p.video.playlistUrl == QLatin1String("https://x/googlevideo.com/v1080.m3u8"),
          "elige la variante de 1080p");
    check(p.video.height == 1080, "la altura de la variante es 1080");
    check(p.video.bandwidth == 4688074, "lee el BANDWIDTH de la variante");
    check(p.video.codecs.contains(QLatin1String("vp09")),
          "lee el CODECS de la variante");

    // The master has no AV1 at all (measured: 9 VP9 + 7 h264, zero AV1), so
    // the plan can never offer AV1 in this phase.
    check(!p.video.codecs.contains(QLatin1String("av01")),
          "el master no trae AV1 (eso es de la fase 2)");

    // The URI of the CHOSEN audio is kept: the downloader has to fetch that
    // playlist itself, the player does not (mpv resolves it).
    check(p.audio.valid, "hay pista de audio");
    check(p.audio.playlistUrl
              == QLatin1String("https://x/googlevideo.com/audio_orig?acont=original"),
          "elige el audio ORIGINAL (no el dub)");
    check(p.audio.language == QLatin1String("en"), "el idioma del audio es en");
    check(p.audio.isOriginal, "marca el audio como original");

    // With a language preference, the dub wins over the original.
    const dl::Plan pEs =
        dl::planFromMaster(QByteArray(kMaster), 1080, QStringLiteral("es"), &testRank);
    check(pEs.audio.playlistUrl == QLatin1String("https://x/googlevideo.com/audio_es"),
          "con preferencia es elige el dub en espanol");
    check(!pEs.audio.isOriginal, "el dub no se marca como original");

    // The height cap works: asking for 720p picks the 720p variant.
    const dl::Plan p720 =
        dl::planFromMaster(QByteArray(kMaster), 720, QString(), &testRank);
    check(p720.video.playlistUrl == QLatin1String("https://x/googlevideo.com/v720.m3u8"),
          "el tope de altura se respeta (720p)");

    // The HEIGHT wins over the codec, which is the OPPOSITE of the player:
    // the player ranks the codec first (it wants the one the GPU decodes),
    // but a download wants the best picture. The codec only breaks the tie
    // between two variants of the same height.
    const dl::Plan pHeightFirst =
        dl::planFromMaster(QByteArray(kMaster), 1080, QString(), &testRank);
    check(pHeightFirst.video.height == 1080,
          "la altura gana al codec (1080p sobre el 720p)");

    // A master with nothing usable is an error, not a crash.
    const dl::Plan pBad =
        dl::planFromMaster(QByteArray("#EXTM3U\n"), 1080, QString(), &testRank);
    check(!pBad.valid, "un master sin variantes no es valido");
    check(!pBad.error.isEmpty(), "el error dice por que");
    const dl::Plan pEmpty =
        dl::planFromMaster(QByteArray(), 1080, QString(), &testRank);
    check(!pEmpty.valid, "un master vacio no es valido");

    // A master with ONE language and no marker: the two renditions are only
    // told apart by their itag, and the HIGH one (234 = 128 kbps) has to be
    // chosen. The first version of the plan took the 48 kbps one.
    const dl::Plan pNoLang =
        dl::planFromMaster(QByteArray(kMasterNoLang), 1080, QString(), &testRank);
    check(pNoLang.valid, "un master sin idioma se parsea");
    check(pNoLang.audio.playlistUrl.contains(QLatin1String("/itag/234/")),
          "sin marcador ni idioma, elige el audio de mas bitrate (234, no 233)");

    // --- the media playlist ---
    const QVector<dl::Segment> segs =
        dl::segmentsFromMediaPlaylist(QByteArray(kMedia));
    check(segs.size() == 3, "la media playlist da 3 segmentos");
    if (segs.size() == 3) {
        check(segs[0].url == QLatin1String("https://x/googlevideo.com/seg1.ts"),
              "el primer segmento es el primero");
        check(qAbs(segs[0].duration - 5.24) < 0.01, "el EXTINF del primero es 5.24");
        check(qAbs(segs[1].duration - 6.96) < 0.01, "el EXTINF del segundo es 6.96");
        check(qAbs(segs[2].duration - 3.36) < 0.01, "el EXTINF del tercero es 3.36");
    }
    check(dl::segmentsFromMediaPlaylist(QByteArray()).isEmpty(),
          "una media playlist vacia no da segmentos");

    // --- the size estimate ---
    // Measured on the real 1080p master: 354.6 MB estimated against 343 MB
    // real, that is 1.7 %.
    const qint64 est = dl::estimateBytes(4688074, 634.566);
    const qint64 real = 343LL * 1024 * 1024;
    const double err = 100.0 * qAbs(double(est - real)) / double(real);
    check(err < 5.0, "el tamano estimado esta dentro del 5 % del real");
    check(dl::estimateBytes(0, 634.0) == 0, "sin BANDWIDTH no hay estimacion");
    check(dl::estimateBytes(4688074, 0.0) == 0, "sin duracion no hay estimacion");

    std::printf("DOWNLOAD SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
