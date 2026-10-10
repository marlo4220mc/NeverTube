#pragma once

#include <QString>

struct VideoInfo;

QString fmtTime(double seconds);
QString durationLabel(int seconds);
QString compactCount(qint64 count);
QString publishedLabel(const QString &publishedAt);
QString viewsLabel(qint64 count);

// Meta line of a video: "canal  •  N vistas  •  hace N d", with whatever is
// available (the three fields are not always there: a cached video may have no
// date, a search may bring no views). It is the SAME in every row of the app
// (Home grid, Search/Channel/Playlist/History/"A continuación" lists and the
// Library sections), so the style does not depend on the page: one function,
// not a copy per surface.
QString metaLine(const VideoInfo &v);

// Description as HTML for a RichText QLabel: escapes the text, turns line
// breaks into <br> and wraps each URL in an <a> link in the accent color (no
// underline). It is the SAME HTML in the Player description and in the Channel
// card — one function, not a copy per surface, so the link color and format
// never diverge.
QString descriptionHtml(const QString &plain);