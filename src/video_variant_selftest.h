#pragma once

// El selftest de las variantes de video (la altura, el códec y el tamaño
// real que un video ofrece). Es puro: no toca la red ni la interfaz, así que
// corre en el smoke de los CI.
bool videoVariantSelfTest();
