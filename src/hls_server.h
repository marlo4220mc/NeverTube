#pragma once

#include <QObject>

class QTcpServer;

class HlsServer : public QObject
{
    Q_OBJECT

public:
    explicit HlsServer(QObject *parent = nullptr);
    ~HlsServer() override;

    QString start();
    bool isRunning() const { return m_server != nullptr; }
    QString origin() const { return m_origin; }
    static QString masterDirPath();

private:
    void onNewConnection();

    QTcpServer *m_server = nullptr;
    QString m_origin;
};