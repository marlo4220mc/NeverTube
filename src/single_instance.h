#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;

// ONE process per user.
//
// The app had no single instance: every `--download` (or every link opened in
// the browser) started a new process, with its own player and its own
// connection to the SAME database. With WAL that is allowed, but the app has no
// busy timeout, so the second writer gets SQLITE_BUSY at once and the download
// page shows an error that means nothing to the person. And two players at the
// same time make no sense.
//
// The second process does not need to exist: it hands its arguments over and
// exits before creating a window.
//
// The name is per USER (the database and the settings are per user) and LOCAL:
// a named pipe on Windows, a socket in the runtime directory on Unix.
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstance(const QString &name, QObject *parent = nullptr);
    ~SingleInstance() override;

    // true  = this process is the ONE: the server is listening for the next
    //         process.
    // false = another instance was already running. It received `args` and
    //         this process has to exit WITHOUT opening a window. The call does
    //         not return until that instance has the line (it closes the
    //         connection as the ack) or the wait expires.
    bool start(const QStringList &args);

    // The name of the instance of the current user.
    static QString name();

signals:
    // The arguments of another process, as they arrived (argv[0] included, the
    // same shape as QApplication::arguments()).
    void argumentsReceived(const QStringList &args);

private:
    void onNewConnection();
    void readFrom(QLocalSocket *socket);

    QString m_name;
    QLocalServer *m_server = nullptr;
    // The bytes received from each connection, until its line is complete.
    QHash<QLocalSocket *, QByteArray> m_buffers;
};
