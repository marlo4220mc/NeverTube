#include "subtitle_overlay.h"

#include <QNetworkReply>
#include <QEvent>
#include "../debug.h"
#include <QPainter>
#include <QRegularExpression>
#include <QStringList>
#include <QFontMetrics>

// ─────────────────────────────────────────────────────────────────────────────
// WebVTT parser — pure, so --selftest-subtitles can cover it offline.
// Handles the subset YouTube delivers.
// ─────────────────────────────────────────────────────────────────────────────

static double parseVttTimeMs(const QStringRef &token)
{
    // Accepts "HH:MM:SS.mmm", "MM:SS.mmm" and "SS.mmm".
    const QStringList parts = token.toString().split(QLatin1Char('.'));
    if (parts.size() < 2)
        return -1.0;
    double ms = parts.at(1).leftJustified(3, QLatin1Char('0')).left(3).toDouble();
    const QStringList hms = parts.at(0).split(QLatin1Char(':'));
    double secs = 0.0;
    for (const QString &p : hms)
        secs = secs * 60.0 + p.trimmed().toDouble();
    return secs * 1000.0 + ms;
}

static QString stripVttTags(const QString &line)
{
    // Remove WebVTT inline tags: <HH:MM:SS.mmm>, <c>, </c>, <b>, </b>, etc.
    // Also remove word-timing marks like <00:00:00.599>.
    QString result;
    result.reserve(line.size());
    bool inTag = false;
    for (const QChar ch : line) {
        if (ch == QLatin1Char('<')) {
            inTag = true;
        } else if (ch == QLatin1Char('>')) {
            inTag = false;
        } else if (!inTag) {
            result += ch;
        }
    }
    return result;
}

namespace vtt {

QVector<Cue> parse(const QByteArray &data)
{
    const QString text = QString::fromUtf8(data);
    const QStringList lines = text.split(QLatin1Char('\n'));

    // Regex to detect a cue-timing line: "HH:MM:SS.mmm --> HH:MM:SS.mmm ..."
    // We accept optional settings after the arrow.
    static const QRegularExpression reTiming(
        QStringLiteral("^(\\S+)\\s+-->\\s+(\\S+)"));

    QVector<Cue> result;
    Cue cur;
    bool inCue = false;
    QStringList cueLines;

    auto flushCue = [&]() {
        if (!inCue)
            return;
        inCue = false;
        if (!cueLines.isEmpty()) {
            // Join all cue lines; strip inline tags; collapse blank lines.
            QStringList parts;
            for (const QString &l : cueLines) {
                const QString stripped = stripVttTags(l).trimmed();
                if (!stripped.isEmpty())
                    parts << stripped;
            }
            if (!parts.isEmpty()) {
                cur.text = parts.join(QLatin1Char(' '));
                result.append(cur);
            }
        }
        cueLines.clear();
    };

    for (const QString &raw : lines) {
        const QString line = raw.trimmed();

        if (inCue) {
            if (line.isEmpty()) {
                flushCue();
            } else {
                cueLines << line;
            }
            continue;
        }

        // Look for a timing line.
        const QRegularExpressionMatch m = reTiming.match(line);
        if (m.hasMatch()) {
            flushCue();
            cur.startMs = parseVttTimeMs(m.capturedRef(1));
            cur.endMs   = parseVttTimeMs(m.capturedRef(2));
            if (cur.startMs >= 0 && cur.endMs > cur.startMs) {
                inCue = true;
                cueLines.clear();
            }
        }
    }
    flushCue();
    return result;
}

int cueAt(const QVector<Cue> &cues, double posMs)
{
    for (int i = 0; i < cues.size(); ++i) {
        if (posMs >= cues.at(i).startMs && posMs < cues.at(i).endMs)
            return i;
    }
    return -1;
}

}  // namespace vtt

// ─────────────────────────────────────────────────────────────────────────────

SubtitleOverlay::SubtitleOverlay(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(QStringLiteral("background: transparent;"));
    if (parent)
        parent->installEventFilter(this);

    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &SubtitleOverlay::onTick);
}

void SubtitleOverlay::loadUrl(const QString &url, const QString &fallbackUrl)
{
    m_cues.clear();
    m_activeCue = -1;
    m_currentText.clear();
    m_fallbackUrl = fallbackUrl;
    m_timer.stop();
    update();

    QNetworkRequest req(url);
    req.setRawHeader("Accept-Language", "es-ES,es;q=0.9");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
        reply->deleteLater();
        bool ok = (reply->error() == QNetworkReply::NoError);
        if (ok) {
            m_cues = vtt::parse(reply->readAll());
        } else if (!m_fallbackUrl.isEmpty() && m_fallbackUrl != url) {
            // Translation was rate-limited (YouTube 429): fall back to the
            // original track so subtitles still show. Do not hammer the
            // translated endpoint again — that makes the block worse.
            const QString fallback = m_fallbackUrl;
            m_fallbackUrl.clear();
            NT_DEBUG("E2E SubtitleOverlay translated failed, falling back\n");
            loadUrl(fallback);
            return;
        }
        NT_DEBUG("E2E SubtitleOverlay download ok=%d cues=%d\n", int(ok), int(m_cues.size()));
        emit downloadFinished(ok);
        if (ok && m_show && !m_cues.isEmpty())
            m_timer.start();
    });
}

void SubtitleOverlay::clear()
{
    m_timer.stop();
    m_cues.clear();
    m_activeCue = -1;
    m_currentText.clear();
    m_show = false;
    setVisible(false);
}

void SubtitleOverlay::setVisible(bool v)
{
    m_show = v;
    if (!v) {
        m_timer.stop();
        m_activeCue = -1;
        m_currentText.clear();
        update();
        QWidget::setVisible(false);
        return;
    }
    QWidget::setVisible(true);
    if (!m_cues.isEmpty())
        m_timer.start();
}

void SubtitleOverlay::setPosition(double positionSecs)
{
    m_posMs = positionSecs * 1000.0;
    // Do not call onTick here: the timer drives updates so we don't do
    // extra work on every position signal from the player.
}

void SubtitleOverlay::onTick()
{
    const int idx = vtt::cueAt(m_cues, m_posMs);
    if (idx == m_activeCue)
        return;                     // cue unchanged — nothing to repaint
    m_activeCue = idx;
    m_currentText = (idx >= 0) ? m_cues.at(idx).text : QString();
    update();
}

void SubtitleOverlay::paintEvent(QPaintEvent *)
{
    if (m_currentText.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing);

    // Font: slightly larger than mpv's default (38 px at 1080p).
    QFont f = font();
    f.setPixelSize(qMax(16, height() / 22));
    f.setBold(false);
    p.setFont(f);

    const QFontMetrics fm(f);
    const int margin = width() / 20;   // horizontal margin (~5%)
    const int availW = width() - 2 * margin;

    // Build a single display line: join multi-line cue text with spaces.
    // We keep it as one line so mpv/libass wrap-style issues cannot occur.
    const QString line = m_currentText.simplified();

    // Elide if the text is wider than the available area.
    const QString display = fm.elidedText(line, Qt::ElideRight, availW);
    const int textW = fm.horizontalAdvance(display);

    // Position: bottom centre, 8% up from the bottom edge.
    const int bottomMargin = height() / 12;
    const int x = (width() - textW) / 2;
    const int y = height() - bottomMargin - fm.descent();

    // Shadow: thin black outline for readability on any background.
    p.setPen(QColor(0, 0, 0, 200));
    const int o = qMax(1, f.pixelSize() / 18);  // shadow offset
    for (int dx = -o; dx <= o; ++dx)
        for (int dy = -o; dy <= o; ++dy)
            if (dx || dy)
                p.drawText(x + dx, y + dy, display);

    // Main text: white.
    p.setPen(Qt::white);
    p.drawText(x, y, display);
}

// ─────────────────────────────────────────────────────────────────────────────

bool SubtitleOverlay::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == parent() && ev->type() == QEvent::Resize) {
        if (auto *pw = qobject_cast<QWidget *>(parentWidget()))
            setGeometry(pw->rect());
    }
    return QWidget::eventFilter(obj, ev);
}
