#include "download_dialog.h"

#include "../i18n.h"

#include <QAction>
#include <QMenu>
#include <QPoint>

namespace dl {

namespace {

// The label of a resolution, like the quality menu of the player: "720p HD",
// "2160p 4K".
QString resolutionLabel(int height)
{
    if (height >= 2160)
        return QStringLiteral("%1p 4K").arg(height);
    if (height >= 720)
        return QStringLiteral("%1p HD").arg(height);
    return QStringLiteral("%1p").arg(height);
}

} // namespace

bool askDownloadOptions(QWidget *parent, const QVector<int> &heights,
                        const QPoint &globalPos, DownloadChoice *choice)
{
    QMenu menu(parent);

    // --- the resolutions ---
    // The SAME list the quality menu of the player offers, and only the ones
    // that are available: the player already hides the ones this video does
    // not have.
    QAction *best = nullptr;
    for (int h : heights) {
        QAction *action = menu.addAction(resolutionLabel(h));
        action->setData(h);
        if (!best)
            best = action;
    }
    if (!best)
        best = menu.addAction(resolutionLabel(0));

    menu.addSeparator();

    // --- only the audio ---
    QAction *audio = menu.addAction(nt::tr("downloads.kindAudio"));
    audio->setData(-1);

    QAction *chosen = menu.exec(globalPos);
    if (!chosen || !choice)
        return false;

    choice->audioOnly = (chosen == audio);
    choice->height = choice->audioOnly
        ? 0
        : chosen->data().toInt();
    return true;
}

} // namespace dl
