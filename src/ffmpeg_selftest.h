#pragma once

// Selftest del FfmpegBackend (Fase 2). Decodifica en MODO BANCO los clips
// embebidos (av1/vp9/h264, los mismos de hwprobe) SIN red, SIN sink de audio
// y SIN render GL: valida la cadena crítica demux -> decode -> swscale y que
// el backend emite EOF natural con el códec correcto. Solo disponible cuando
// el build compiló el backend (NEVERTUBE_HAS_FFMPEG_BACKEND); sin FFmpeg es
// un skip (no un fallo), para que --selftest-ffmpeg no rompa builds x64 sin
// FFmpeg.
bool ffmpegSelfTest();