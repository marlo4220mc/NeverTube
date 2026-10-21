#pragma once

// El selftest de los ajustes del downloader (el códec por defecto). Es puro:
// no toca la red ni la interfaz, así que corre en el smoke de los CI.
bool downloadSettingsSelfTest();
