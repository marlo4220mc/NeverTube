#ifndef NT_PLAYER_SELFTEST_H
#define NT_PLAYER_SELFTEST_H

// Selftest del núcleo de reproducción protegido (PlayerWidget):
//   - hwdec efectivo == "auto"
//   - QTimer de redibujado a 33 ms activo
//   - mpv_render_context válido tras initializeGL
// No reproduce ningún archivo; solo verifica invariantes del núcleo.
// Devuelve true si todo está bien.
bool playerSelfTest();

#endif