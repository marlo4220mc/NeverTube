#pragma once

#include <QString>

// The settings of the downloader: the folder, the connections and the
// recommended codec.
//
// They are CODES and not texts: the visible text is the translation of the
// code, so the setting survives a change of language and the identity of an
// option is stable.
namespace downloadsettings {

// The key of the setting of the folder (in the settings table).
inline constexpr const char *kFolderKey = "download_folder";
// The key of the setting of the number of connections.
inline constexpr const char *kConnectionsKey = "download_connections";

// The limits of the number of connections. 8 is the measured optimum (4 ->
// 15,23 MB/s, 8 -> 17-24, 12 -> 20,3 but not reliable, 16 -> 3,69 MB/s: the
// connections start to fight each other).
inline constexpr int kMinConnections = 1;
inline constexpr int kMaxConnections = 16;
inline constexpr int kDefaultConnections = 8;

// The number of connections, clamped to the valid range. An empty or
// invalid value is the default one.
int connectionsOf(const QString &value);

// The value that is saved for a number of connections (the same one that
// comes back).
QString valueForConnections(int connections);

} // namespace downloadsettings
