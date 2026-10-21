#pragma once

// El selftest del transporte del downloader: el descarte de los fragmentos
// que no avanzan (los que el CDN sirve a crawl y retienen una conexion).
// Es puro: no toca la red, asi que corre en el smoke de los CI.
bool downloadTransportSelfTest();
