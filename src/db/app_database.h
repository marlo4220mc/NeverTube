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