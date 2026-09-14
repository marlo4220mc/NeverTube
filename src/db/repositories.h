#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "../models.h"

class AppDatabase;
class QSqlQuery;

class DiscoverRepo;

struct SubscriptionInfo
{
    QString id;
    QString title;
    QString avatar;
};

struct PlaylistItem
{
    QString id;
    QString title;
};

class VideoCacheRepo
{
public:
    explicit VideoCacheRepo(AppDatabase &db);

    bool upsert(const VideoInfo &v);
    bool get(const QString &id, VideoInfo &out) const;
    QVector<VideoInfo> getMany(const QStringList &ids) const;

private:
    VideoInfo fromRow(const QSqlQuery &q) const;

    AppDatabase &m_db;
};

class HistoryRepo
{
public:
    explicit HistoryRepo(AppDatabase &db, DiscoverRepo *discover = nullptr);

    bool add(const QString &videoId, qlonglong positionMs = 0);
    bool remove(const QString &videoId);
    bool clear();
    QStringList recentIds(int limit = 50) const;
    qlonglong positionOf(const QString &videoId) const;

    // Un vídeo se considera COMPLETO si su posición guardada indica ≥95% visto
    // o le quedan ≤15 s (final/créditos). En ese caso no debe ofrecerse en
    // "Continuar viendo" y, si se reproduce desde el historial, arranca desde
    // el principio.
    static bool isCompleted(const VideoInfo &v, qlonglong positionMs);
    // Posición inicial de reanudación para un vídeo del historial: 0 si está
    // completo (o no tiene progreso), su posición de otro modo.
    static qlonglong resumeStartMs(const VideoInfo &v, qlonglong positionMs);

private:
    AppDatabase &m_db;
    DiscoverRepo *m_discover = nullptr;
};

class FavoritesRepo
{
public:
    explicit FavoritesRepo(AppDatabase &db);

    bool toggle(const QString &videoId);
    bool isFavorite(const QString &videoId) const;
    QStringList ids() const;

private:
    AppDatabase &m_db;
};

// Me gusta (pulgar) y Favoritos (estrella) son repos separados a propósito.
// Esta es la estrella manual, distinta del toggle de "Me gusta".
class StarredRepo
{
public:
    explicit StarredRepo(AppDatabase &db);

    bool toggle(const QString &videoId);
    bool isStarred(const QString &videoId) const;
    QStringList ids() const;
    bool remove(const QString &videoId);

private:
    AppDatabase &m_db;
};

class WatchLaterRepo
{
public:
    explicit WatchLaterRepo(AppDatabase &db);

    bool toggle(const QString &videoId);
    bool isWatchLater(const QString &videoId) const;
    QStringList ids() const;
    bool remove(const QString &videoId);

private:
    AppDatabase &m_db;
};

class SubscriptionsRepo
{
public:
    explicit SubscriptionsRepo(AppDatabase &db);

    bool add(const QString &channelId, const QString &title,
             const QString &avatarUrl = QString());
    bool setAvatar(const QString &channelId, const QString &avatarUrl);
    bool remove(const QString &channelId);
    bool isSubscribed(const QString &channelId) const;
    QVector<SubscriptionInfo> all() const;

private:
    AppDatabase &m_db;
};

class PlaylistsRepo
{
public:
    explicit PlaylistsRepo(AppDatabase &db);

    QString create(const QString &title);
    bool deletePlaylist(const QString &id);
    bool rename(const QString &id, const QString &title);
    QVector<PlaylistItem> all() const;
    bool contains(const QString &playlistId, const QString &videoId) const;
    bool addVideo(const QString &playlistId, const QString &videoId);
    bool removeVideo(const QString &playlistId, const QString &videoId);
    QStringList videoIds(const QString &playlistId) const;

private:
    AppDatabase &m_db;
};

class SettingsRepo
{
public:
    explicit SettingsRepo(AppDatabase &db);

    bool get(const QString &key, QString &out) const;
    bool set(const QString &key, const QString &value);
    qlonglong getInt(const QString &key, qlonglong def = 0) const;
    QString getString(const QString &key, const QString &def = QString()) const;

private:
    AppDatabase &m_db;
};

// Caché persistente del Home por sección de red (Recomendado/Feed/Tendencias).
// Guarda solo ids ordenados + fetched_at; los metadatos viven en "videos"
// (VideoCacheRepo). saveSection REEMPLAZA la sección completa: cada respuesta
// de red es la lista vigente de esa sección, nunca se agrega contenido a una
// lista vieja (evita duplicados y contenido obsoleto).
class HomeCacheRepo
{
public:
    explicit HomeCacheRepo(AppDatabase &db);

    bool saveSection(const QString &section, const QStringList &videoIds,
                     qint64 fetchedAt);
    QStringList sectionVideoIds(const QString &section) const;
    qint64 sectionFetchedAt(const QString &section) const;
    bool clearSection(const QString &section);

private:
    AppDatabase &m_db;
};

struct DiscoverRadio
{
    qlonglong id = 0;
    QString seedVideoId;
    qlonglong createdAt = 0;
    qlonglong lastUsedAt = 0;
};

// Radios guardadas de Descubre (fuente persistente: no se generan al abrir
// Home, se construyen al reproducir vídeos y aquí se almacenan). Los
// metadatos de los vídeos reutilizan la tabla "videos" (VideoCacheRepo).
class DiscoverRepo
{
public:
    explicit DiscoverRepo(AppDatabase &db);

    // Guarda (o reemplaza) la radio de una semilla: una radio por semilla, no
    // se duplica si ya existe. Devuelve true si quedó guardada con contenido.
    bool saveRadio(const QString &seedVideoId, const QVector<VideoInfo> &videos);
    bool hasRadio(const QString &seedVideoId) const;
    // Radios ordenadas por last_used_at ASC (menos usadas primero, variedad).
    QVector<DiscoverRadio> radios() const;
    // Ids de vídeo de una radio en orden de posición. Los metadatos se resuelven
    // con VideoCacheRepo::getMany sobre el caché "videos".
    QStringList radioVideoIds(qlonglong radioId) const;
    // Ids de vídeo de la radio guardada para una semilla (orden de posición),
    // para reusarla en "A continuación" sin volver a pedirla a YouTube.
    QStringList radioVideoIdsForSeed(const QString &seedVideoId) const;
    void markUsed(qlonglong radioId);
    bool deleteRadio(qlonglong radioId);
    int count() const;
    AppDatabase &db() const { return m_db; }

    // Limpieza transaccional con HistoryRepo: al borrar un vídeo del
    // historial, elimina la radio cuyo seed sea ese vídeo.
    void clearForSeed(const QString &seedVideoId);
    // Al borrar todo el historial, elimina todas las radios.
    void clearAll();

private:
    AppDatabase &m_db;
};

// Selección de vídeos para Descubre usando SOLO las radios almacenadas (sin
// red). Coge hasta maxVideos de entre las radios guardadas menos usadas
// (barajadas): hasta perRadio vídeos de cada una, sin repetir ids y excluyendo
// excludeId. Las radios sin vídeo útil (borradas de "videos" o vacías) se
// eliminan. Devuelve lo conseguido: puede ser < maxVideos si no hay contenido
// válido, lista vacía si no hay radios.
QVector<VideoInfo> pickDiscoverVideos(DiscoverRepo &discover,
                                      VideoCacheRepo &cache,
                                      const QString &excludeId,
                                      int maxVideos = 5, int perRadio = 2);

bool databaseSelfTest(const QString &dbPath);
bool discoverSelfTest(const QString &dbPath);