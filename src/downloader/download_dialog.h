#pragma once

#include <QString>
#include <QVector>
#include <functional>

#include "../video_variant.h"

class QWidget;
class QPoint;
class QMenu;

namespace dl {

// What the user chose in the menu of the download.
struct DownloadChoice
{
    int height = 0;          // 0 = the best one
    QString codec;           // empty = the default one (the smallest)
    bool audioOnly = false;  // only the audio, as an mp3
};

// One line of the menu of the download: a resolution with the codecs it has.
//
// The menu is a QMenu like the one of the three dots of a row: the
// resolution opens a SUBMENU with the codecs of that resolution (AV1, VP9,
// H264), each with its real size, so the user can pick the one that suits
// the machine the file is going to be watched on.
//
// Clicking the resolution ITSELF downloads the default codec of it (the
// smallest one): nobody is forced to open the submenu.
struct ResolutionEntry
{
    int height = 0;
    // The codecs of this resolution, from the smallest file to the biggest
    // one, with their real size.
    QVector<VideoVariant> codecs;
};

// The lines of the menu: one per resolution that has variants, from the
// highest to the lowest.
//
// It is a PURE function (of the variants), so the shape of the menu can be
// checked without a screen.
QVector<ResolutionEntry> menuEntriesOf(const QVector<VideoVariant> &variants);

// BUILDS the menu of the download (without showing it): the resolutions,
// each with its codecs in a submenu, and the option of only the audio.
//
// It is separate from the function that shows it because the shape of the
// menu can be checked without a screen.
void buildDownloadMenu(QMenu *menu, const QVector<VideoVariant> &variants,
                        const QString &defaultCodec = QString());

// The menu of the download: it asks for the RESOLUTION and for WHAT to
// download (the video with its audio, or only the audio as an mp3).
//
// It is a QMenu, like the one of the three dots of a row: it appears where
// the button is, it is dismissed with Escape or with a click outside, and it
// does not steal the pointer the way a modal dialog does.
//
// It asks for NOTHING: the resolutions are the ones the player already
// knows (the same list of the quality menu, with the ones that are not
// available already hidden), so clicking the button does not have to ask
// YouTube for anything.
//
// `globalPos` is where the menu appears (the corner of the button).
//
// `defaultCodec` is the codec of the setting of Ajustes: it is the one that
// the click on the resolution downloads (empty = the smallest one of it).
bool askDownloadOptions(QWidget *parent, const QVector<VideoVariant> &variants,
                        const QPoint &globalPos, const QString &defaultCodec,
                        DownloadChoice *choice);

} // namespace dl
