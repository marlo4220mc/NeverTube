#pragma once

#include <QHash>
#include <QObject>
#include <QVector>

#include "data_transfer.h"

class QNetworkAccessManager;
class QNetworkReply;

// Verifies a list of subscriptions against YouTube in parallel before importing,
// discarding terminated, deleted, or banned channels (the same behavior as NewPipe).
// Also extracts the live title and avatar URL so channels have their metadata
// ready from the first second.
class ChannelVerifier : public QObject
{
    Q_OBJECT

public:
    explicit ChannelVerifier(QObject *parent = nullptr);
    ~ChannelVerifier() override;

    void start(const QVector<data_transfer::Subscription> &subs, int concurrency = 8);
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    void progress(int done, int total, int alive, int dead);
    void finished(const QVector<data_transfer::Subscription> &alive, int deadCount);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    void dispatchNext();

    QNetworkAccessManager *m_nam = nullptr;
    QVector<data_transfer::Subscription> m_pending;
    QVector<data_transfer::Subscription> m_alive;
    QHash<QNetworkReply *, data_transfer::Subscription> m_inFlight;
    int m_total = 0;
    int m_done = 0;
    int m_dead = 0;
    int m_concurrency = 8;
    bool m_running = false;
    bool m_cancelled = false;
};