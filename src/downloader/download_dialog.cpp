#include "download_dialog.h"

#include "../i18n.h"
#include "../ui/format.h"

#include <QAction>
#include <QMenu>
#include <QPoint>

#include <algorithm>

namespace dl {

namespace {

// The name of a codec, as it is written: "AV1", "VP9", "H264".
QString codecLabel(const QString &codec)
{
    if (codec == QLatin1String("av1"))
        return QStringLiteral("AV1");
    if (codec == QLatin1String("vp9"))
        return QStringLiteral("VP9");
    if (codec == QLatin1String("h264"))
        return QStringLiteral("H264");
    return codec.toUpper();
}

} // namespace

QVector<ResolutionEntry> menuEntriesOf(const QVector<VideoVariant> &variants)
{
    QVector<ResolutionEntry> out;
    for (const VideoVariant &v : variants) {
        if (v.height <= 0)
            continue;
        ResolutionEntry *entry = nullptr;
        for (ResolutionEntry &e : out) {
            if (e.height == v.height) {
                entry = &e;
                break;
            }
        }
        if (!entry) {
            ResolutionEntry e;
            e.height = v.height;
            out.append(e);
            entry = &out.last();
        }
        entry->codecs.append(v);
    }
    // Inside a resolution, the SMALLEST file comes first: it is the default
    // one (the click on the resolution downloads it) and the one a person
    // wants by default.
    for (ResolutionEntry &e : out) {
        std::sort(e.codecs.begin(), e.codecs.end(),
                  [](const VideoVariant &a, const VideoVariant &b) {
                      return a.sizeBytes < b.sizeBytes;
                  });
    }
    // From the highest resolution to the lowest.
    std::sort(out.begin(), out.end(),
              [](const ResolutionEntry &a, const ResolutionEntry &b) {
                  return a.height > b.height;
              });
    return out;
}

void buildDownloadMenu(QMenu *menu, const QVector<VideoVariant> &variants,
                        const QString &defaultCodec)
{
    if (!menu)
        return;

    // The data of an item: the height and the codec. It travels in ONE
    // QVariant (a map), because setData() overwrites what was there.
    auto setChoice = [](QAction *a, int height, const QString &codec) {
        QVariantMap m;
        m.insert(QStringLiteral("h"), height);
        m.insert(QStringLiteral("c"), codec);
        a->setData(m);
    };

    // --- the resolutions, each with its codecs in a submenu ---
    const QVector<ResolutionEntry> entries = menuEntriesOf(variants);
    for (const ResolutionEntry &entry : entries) {
        if (entry.codecs.size() <= 1) {
            // A single codec: no submenu, the line IS the choice.
            QAction *action = menu->addAction(resolutionLabel(entry.height));
            setChoice(action, entry.height,
                      entry.codecs.isEmpty() ? QString() : entry.codecs.first().codec);
            continue;
        }
        // The RECOMMENDED codec is the one that this PC can reproduce (the
        // hardware profile says which one). It is the one that the click on
        // the resolution downloads. When the resolution does not have it,
        // the smallest one of the resolution wins.
        QString recommendedHere = defaultCodec;
        bool hasRecommended = false;
        for (const VideoVariant &v : entry.codecs) {
            if (v.codec == recommendedHere) {
                hasRecommended = true;
                break;
            }
        }
        if (!hasRecommended)
            recommendedHere = entry.codecs.first().codec;
        // The submenu: the codecs of this resolution, with their real size.
        QMenu *sub = menu->addMenu(resolutionLabel(entry.height));
        sub->setToolTipsVisible(true);
        for (const VideoVariant &v : entry.codecs) {
            QString label = codecLabel(v.codec);
            const QString size = humanBytes(v.sizeBytes);
            if (!size.isEmpty())
                label += QStringLiteral("  ·  ") + size;
            QAction *codecAction = sub->addAction(label);
            setChoice(codecAction, entry.height, v.codec);
            if (!size.isEmpty())
                codecAction->setToolTip(size);
            // The recommended one is marked, and it says WHY: the person
            // sees which one this PC reproduces without opening anything.
            const bool isRecommended = (v.codec == recommendedHere);
            codecAction->setCheckable(true);
            codecAction->setChecked(isRecommended);
            if (isRecommended) {
                const QString note = nt::tr("downloads.recommendedForPC");
                codecAction->setText(label + QStringLiteral("  ·  ") + note);
                codecAction->setToolTip(note);
            }
        }
    }

    menu->addSeparator();

    // --- only the audio ---
    QAction *audio = menu->addAction(nt::tr("downloads.kindAudio"));
    setChoice(audio, -1, QString());
}

bool askDownloadOptions(QWidget *parent, const QVector<VideoVariant> &variants,
                        const QPoint &globalPos, const QString &defaultCodec,
                        DownloadChoice *choice)
{
    QMenu menu(parent);
    buildDownloadMenu(&menu, variants, defaultCodec);

    QAction *chosen = menu.exec(globalPos);
    if (!chosen || !choice)
        return false;

    const QVariantMap m = chosen->data().toMap();
    const int height = m.value(QStringLiteral("h")).toInt();
    choice->audioOnly = height < 0;
    choice->height = choice->audioOnly ? 0 : height;
    choice->codec = choice->audioOnly ? QString() : m.value(QStringLiteral("c")).toString();
    return true;
}

} // namespace dl
