#ifndef NT_PLAYER_SELFTEST_H
#define NT_PLAYER_SELFTEST_H

// Self-test of the protected playback core (PlayerWidget):
//   - effective hwdec == "auto"
//   - 33 ms redraw QTimer active
//   - valid mpv_render_context after initializeGL
// It does not play any file; it only checks core invariants.
// Returns true if everything is fine.
bool playerSelfTest();

#endif