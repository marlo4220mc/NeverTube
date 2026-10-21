#include "reels_filter.h"

#include "models.h"

namespace reels {

bool thumbnailIsVertical(int width, int height)
{
    // Without the measures nothing is known: nothing is discarded.
    if (width <= 0 || height <= 0)
        return false;
    // Vertical means clearly taller than wide. The 4:3 of an old video
    // (640x480) is NOT a Short, so the margin has to be wide: the Shorts
    // are 9:16 (1080x1920) and the normal ones are 16:9 (1280x720).
    return height > width * 3 / 2;
}

bool isVertical(const VideoInfo &video)
{
    return thumbnailIsVertical(video.thumbWidth, video.thumbHeight);
}

QVector<VideoInfo> withoutShorts(const QVector<VideoInfo> &videos)
{
    QVector<VideoInfo> out;
    out.reserve(videos.size());
    for (const VideoInfo &v : videos) {
        if (isVertical(v))
            continue;
        out.append(v);
    }
    return out;
}

} // namespace reels
