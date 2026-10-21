#include "reels_filter.h"

#include "models.h"

#include <cstdio>

bool reelFilterSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  REELS %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };

    // --- the vertical thumbnail ---
    check(reels::thumbnailIsVertical(1080, 1920),
          "una miniatura 1080x1920 es vertical (un Short)");
    check(!reels::thumbnailIsVertical(1280, 720),
          "una miniatura 1280x720 NO es vertical");
    check(!reels::thumbnailIsVertical(640, 480),
          "una miniatura 640x480 (4:3) NO es vertical");
    check(!reels::thumbnailIsVertical(1080, 1080),
          "una miniatura cuadrada NO es vertical");
    check(!reels::thumbnailIsVertical(0, 0),
          "sin medidas no se descarta nada");

    // --- the list without the shorts ---
    auto video = [](const QString &id, int w, int h) {
        VideoInfo v;
        v.id = id;
        v.thumbWidth = w;
        v.thumbHeight = h;
        return v;
    };
    QVector<VideoInfo> list;
    list.append(video(QStringLiteral("normal1"), 1280, 720));
    list.append(video(QStringLiteral("short1"), 1080, 1920));
    list.append(video(QStringLiteral("normal2"), 320, 180));
    list.append(video(QStringLiteral("short2"), 1080, 1920));
    list.append(video(QStringLiteral("sinMedidas"), 0, 0));

    const QVector<VideoInfo> filtered = reels::withoutShorts(list);
    check(filtered.size() == 3, "los 2 reels quedan fuera de los 5");
    check(filtered.size() == 3 && filtered.at(0).id == QLatin1String("normal1"),
          "el primero que queda es el que estaba primero");
    check(filtered.size() == 3 && filtered.at(1).id == QLatin1String("normal2"),
          "el orden de los que quedan no se cambia");
    check(filtered.size() == 3 && filtered.at(2).id == QLatin1String("sinMedidas"),
          "un video sin medidas NO se descarta (no se descarta nada por ignorancia)");
    check(reels::withoutShorts(QVector<VideoInfo>()).isEmpty(),
          "una lista vacia queda vacia");

    std::printf("REELS SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
