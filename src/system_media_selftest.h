#pragma once

// Selftest of the system media integration: the MPRIS conversions (playback
// status, loop status, metadata, volume, track id) are pure and run on every
// platform; on a Linux session with a D-Bus bus it additionally registers the
// real service and does a round trip (Get + PlayPause + PropertiesChanged).
// Offline: no network, no player, no GPU. It goes in the CI smoke.
bool systemMediaSelfTest();
