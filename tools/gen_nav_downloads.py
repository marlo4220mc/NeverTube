#!/usr/bin/env python3
"""Genera los sprites base de la nueva clave del nav "downloads".

El generador de acentos (tools/gen_nav_accent.cpp) solo RE-TIÑE sprites que ya
existen: no dibuja. Asi que el PNG base hay que hacerlo aqui, en el mismo
lenguaje visual que los otros: 512x512, RGBA, el rojo nativo del nav
(#FF0033) y trazo grueso con extremos redondeados.

Los seis estados que NavButton::updateIcon() busca: active, inactive,
disabled, pressed, hover y circular. El "circular" es el que se usa como
favicon de la pagina.

Uso: gen_nav_downloads.py <dir-nav>
"""
import sys
import os

from PIL import Image, ImageDraw

SIZE = 512
NATIVE_RED = (255, 0, 51, 255)
MUTED = (143, 143, 143, 255)
DISABLED = (90, 90, 90, 255)

# El glifo: una flecha que baja hacia una bandeja (el "download" de Material
# Icons, dibujado a mano para que el trazo sea el mismo que el de los demas).
STROKE = 44


def _download(draw, color, offset=0, scale=1.0):
    cx = SIZE / 2
    top = 120 + offset
    # la flecha
    draw.line([(cx, top), (cx, top + 150 * scale)], fill=color, width=STROKE)
    draw.line([(cx - 55 * scale, top + 100 * scale),
               (cx, top + 160 * scale),
               (cx + 55 * scale, top + 100 * scale)],
              fill=color, width=STROKE, joint="curve")
    # la bandeja
    y = top + 215 * scale
    half = 105 * scale
    draw.line([(cx - half, y), (cx - half, y + 45 * scale),
               (cx + half, y + 45 * scale), (cx + half, y)],
              fill=color, width=STROKE, joint="curve")


def _circular(draw, color):
    """La version redonda: la que se usa como favicon."""
    m = 46
    draw.rounded_rectangle([m, m, SIZE - m, SIZE - m],
                           radius=int(SIZE * 0.16), outline=color,
                           width=STROKE)
    _download(draw, color, offset=-6, scale=0.78)


def sprite(kind, color):
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if kind == "circular":
        _circular(d, color)
    else:
        _download(d, color)
    return im


def main():
    if len(sys.argv) < 2:
        print("uso: %s <dir-nav>" % sys.argv[0], file=sys.stderr)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    written = 0
    for kind, color in (("active", NATIVE_RED),
                        ("hover", NATIVE_RED),
                        ("pressed", NATIVE_RED),
                        ("inactive", MUTED),
                        ("disabled", DISABLED),
                        ("circular", NATIVE_RED)):
        path = os.path.join(out, "downloads_%s.png" % kind)
        sprite(kind, color).save(path)
        written += 1
    print("escritos %d sprites base en %s" % (written, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
