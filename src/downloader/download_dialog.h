#pragma once

#include <QString>
#include <QVector>
#include <functional>

class QWidget;

#include "segment_plan.h"

namespace dl {


// What the user chose in the dialog of the download.
struct DownloadChoice
{
    int height = 0;          // 0 = the best one
    bool audioOnly = false;  // only the audio, as an mp3
};

// The dialog of the download: it asks for the RESOLUTION and for WHAT to
// download (the video with its audio, or only the audio as an mp3).
//
// It is NOT a QDialog: it is an overlay child of the window, like the
// confirmation of the app (nt::confirm), because a window of its own steals
// the pointer from the user.
//
// The options come from the master of the video (the variants it has), and
// each one carries its estimated size, so the user sees what it costs
// BEFORE starting. The estimate is BANDWIDTH x duration: measured, 354,6 MB
// estimated against 343 MB real (1.7 %).
//
// Returns true when the user accepted; `choice` then has what was chosen.
bool askDownloadOptions(QWidget *parent, const QVector<VideoOption> &options,
                        const QString &title, const QString &durationLabel,
                        DownloadChoice *choice);

} // namespace dl
