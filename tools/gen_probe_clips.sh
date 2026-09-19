#!/bin/sh
# Genera los micro-clips de sondeo de hardware (resources/clips/).
#
# Objetivo: que mpv active el decodificador de hardware real de cada códec
# (si existe) para poder leer hwdec-current. Receta obligatoria:
#   - 8-bit 4:2:0 (yuv420p): VDPAU solo 8-bit 4:2:0; VA-API AV1 solo 8/10-bit.
#   - Perfiles universales: H.264 Main, VP9 Profile 0, AV1 Main.
#   - 512x288 (16:9, múltiplo de 16 en ambos ejes): por encima del mínimo real
#     de cualquier decoder (176x144) y en rejilla segura de macrobloques.
#   - Sin B-frames / refs=1 / keyframe al inicio: el decoder arranca al frame 0.
#   - Sin audio: el sondeo solo mide vídeo.
# Uso: tools/gen_probe_clips.sh   (requiere ffmpeg con libx264, libvpx-vp9,
#                                  libsvtav1)
set -eu
dir="$(cd "$(dirname "$0")/.." && pwd)/resources/clips"
mkdir -p "$dir"
src="-f lavfi -i testsrc2=s=512x288:r=10 -frames:v 10 -an"

ffmpeg -hide_banner -loglevel error -y $src \
  -c:v libx264 -profile:v main -level 3.1 -bf 0 -refs 1 -g 10 \
  -pix_fmt yuv420p -movflags +faststart "$dir/h264.mp4"

ffmpeg -hide_banner -loglevel error -y $src \
  -c:v libvpx-vp9 -profile:v 0 -pix_fmt yuv420p \
  -lag-in-frames 0 -auto-alt-ref 0 -g 10 -b:v 0 -crf 42 "$dir/vp9.webm"

ffmpeg -hide_banner -loglevel error -y $src \
  -c:v libsvtav1 -profile:v 0 -pix_fmt yuv420p -g 10 -crf 42 "$dir/av1.mkv" \
  2>/dev/null

for f in h264.mp4 vp9.webm av1.mkv; do
  printf '%-9s %6s bytes  ' "$f" "$(stat -c%s "$dir/$f")"
  ffprobe -v error -select_streams v:0 \
    -show_entries stream=profile,width,height,pix_fmt -of csv=p=0 "$dir/$f"
done
