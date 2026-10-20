#pragma once

#include <QString>
#include <QVector>
#include <functional>

class QWidget;
class QPoint;

namespace dl {

// What the user chose in the menu of the download.
struct DownloadChoice
{
    int height = 0;          // 0 = the best one
    bool audioOnly = false;  // only the audio, as an mp3
};

// The menu of the download: it asks for the RESOLUTION and for WHAT to
// download (the video with its audio, or only the audio as an mp3).
//
// It is a QMenu, like the one of the three dots of a row: it appears where
// the button is, it is dismissed with Escape or with a click outside, and it
// does not steal the pointer the way a modal dialog does.
//
// It asks for NOTHING: the resolutions are the ones the player already
// offers (the same list of the quality menu, with the ones that are not
// available already hidden), so clicking the button does not have to ask
// YouTube for anything.
//
// `globalPos` is where the menu appears (the corner of the button).
bool askDownloadOptions(QWidget *parent, const QVector<int> &heights,
                        const QPoint &globalPos, DownloadChoice *choice);

} // namespace dl
