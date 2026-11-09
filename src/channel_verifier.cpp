#include "channel_verifier.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace
{
const char kBrowseUrl[] = "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false";
const char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like "
    "Gecko) Chrome/120.0.0.0 Safari/537.36";
}

ChannelVerifier::ChannelVerifier(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    connect(m_nam, &QNetworkAccessManager::finished, this,
            &ChannelVerifier::onReplyFinished);
}

ChannelVerifier::~ChannelVerifier()
{
    cancel();
}

void ChannelVerifier::start(const QVector<data_transfer::Subscription> &subs,
                            int concurrency)
{
    cancel();
    m_pending = subs;
    m_alive.clear();
    m_inFlight.clear();
    m_total = subs.size();
    m_done = 0;
    m_dead = 0;
    m_concurrency = qBound(1, concurrency, 16);
    m_running = true;
    m_cancelled = false;

    if (m_total == 0) {
        m_running = false;
        emit finished(m_alive, m_dead);
        return;
    }

    dispatchNext();
}

void ChannelVerifier::cancel()
{
    m_cancelled = true;
    m_running = false;
    for (auto it = m_inFlight.begin(); it != m_inFlight.end(); ++it) {
        if (it.key()) {
            it.key()->abort();
            it.key()->deleteLater();
        }
    }
    m_inFlight.clear();
    m_pending.clear();
}

void ChannelVerifier::dispatchNext()
{
    if (m_cancelled || !m_running)
        return;

    while (m_inFlight.size() < m_concurrency && !m_pending.isEmpty()) {
        const data_transfer::Subscription sub = m_pending.takeFirst();
        const int slash = sub.url.lastIndexOf(QLatin1Char('/'));
        const QString channelId =
            slash >= 0 ? sub.url.mid(slash + 1) : sub.url;
        if (channelId.isEmpty()) {
            ++m_done;
            ++m_dead;
            emit progress(m_done, m_total, m_alive.size(), m_dead);
            continue;
        }

        QJsonObject client;
        client.insert(QStringLiteral("clientName"), QStringLiteral("WEB"));
        client.insert(QStringLiteral("clientVersion"),
                      QStringLiteral("2.20260301.00.00"));
        client.insert(QStringLiteral("hl"), QStringLiteral("es"));

        QJsonObject body;
        body.insert(QStringLiteral("context"),
                    QJsonObject{{QStringLiteral("client"), client}});
        body.insert(QStringLiteral("browseId"), channelId);

        QNetworkRequest req((QUrl(QLatin1String(kBrowseUrl))));
        req.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QString::fromLatin1(kUserAgent));

        QNetworkReply *reply = m_nam->post(
            req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        m_inFlight.insert(reply, sub);
    }

    if (m_inFlight.isEmpty() && m_pending.isEmpty()) {
        m_running = false;
        emit finished(m_alive, m_dead);
    }
}

void ChannelVerifier::onReplyFinished(QNetworkReply *reply)
{
    if (!reply)
        return;
    reply->deleteLater();

    if (!m_inFlight.contains(reply))
        return;

    data_transfer::Subscription sub = m_inFlight.take(reply);
    if (m_cancelled)
        return;

    const int statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();

    bool isAlive = false;
    if (statusCode == 200) {
        const data_transfer::ChannelCheckResult check =
            data_transfer::parseChannelCheckResponse(responseBody, sub.name);
        if (check.isAlive) {
            isAlive = true;
            if (!check.title.isEmpty())
                sub.name = check.title;
            if (!check.avatarUrl.isEmpty())
                sub.avatarUrl = check.avatarUrl;
            m_alive.append(sub);
        }
    }

    if (!isAlive)
        ++m_dead;

    ++m_done;
    emit progress(m_done, m_total, m_alive.size(), m_dead);

    dispatchNext();
}