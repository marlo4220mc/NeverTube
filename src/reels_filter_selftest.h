#pragma once

// El selftest del filtro de reels (los Shorts, que son los videos
// verticales). Es puro: no toca la red ni la interfaz, así que corre en el
// smoke de los CI.
bool reelFilterSelfTest();
