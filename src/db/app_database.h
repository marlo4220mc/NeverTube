#pragma once

#include <QSqlDatabase>
#include <QString>

class QSqlDatabase;

class AppDatabase
{
public:
    explicit AppDatabase(const QString &explicitPath = QString());
    ~AppDatabase();

    bool open();
    void close();
    bool isOpen() const;
    QString path() const;
    QString lastError() const;
    QSqlDatabase db() const;

private:
    void createSchema(QSqlDatabase &db);
    bool tableExists(QSqlDatabase &db, const QString &name) const;

    QSqlDatabase m_db;
    QString m_path;
    QString m_lastError;
};

// Moves the folder of a database that an OLD resolution left in another place
// (the English "Documents" that Qt returns when XDG_CONFIG_HOME is custom) to
// the documents folder of the SYSTEM, BEFORE anything opens it: the history
// belongs to the person and is not recreated from scratch.
//
// It refuses (false) when the origin or the destination is empty, when they are
// the same, when the origin is not a folder, when the destination ALREADY exists
// (two databases are never mixed) and when the move fails; in those cases the
// caller keeps the old folder, because a history in a folder that is not the one
// of the system is bad, but a LOST history is worse.
//
// It is public so --selftest-db can exercise the move in temp directories.
bool migrateLegacyDatabase(const QString &from, const QString &to);