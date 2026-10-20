#pragma once

// Offline selftest of the downloader's segment plan. It touches NO network
// and NO GPU: the plan is a pure function of the master playlist, so the
// cases below are deterministic and run in CI.
//
// What it pins:
//  - the master is parsed into a video variant (the best by rank, capped at
//    maxHeight) and an audio rendition, and the URI of the CHOSEN audio is
//    kept (the player discards it because mpv resolves the audio itself);
//  - a media playlist becomes an ordered list of segments with durations;
//  - the size estimate from BANDWIDTH matches the real size to within a few
//    per cent (measured: 354.6 MB estimated against 343 MB real).
bool downloadPlanSelfTest();
