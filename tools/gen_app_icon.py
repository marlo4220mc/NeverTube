#!/usr/bin/env python3
"""Genera el juego de iconos de la app a partir del PNG maestro.

    resources/icon/nt_icon.png   (1024, alfa real)  -> maestro, se commitea
    resources/icon/nt_16.png ... nt_256.png         -> tamanos sueltos
    resources/icon/nt.ico                            -> multirresolucion

Por que hace falta un .ico DE VERDAD y no un PNG renombrado: el icono de un
.exe lo le el shell de Windows, que solo acepta .ico; y Windows elige la
entrada por la altura en pixels que pide la superficie (16 en la barra de
tareas al 100 %, 32 en el Explorador de iconos pequenos, 256 en la barra
grande y en la vista de iconos grandes). Un .ico con una sola entrada se ve
borroso en casi todas.

Uso:
    python3 tools/gen_app_icon.py                 # regenera desde el maestro
    python3 tools/gen_app_icon.py --cuadrar       # ademas cuadra el maestro

El motivo se RECORTA de su margen transparente y se reescala para llenar el
lienzo (normalizar()): el maestro traia la baldosa al ~90 % x 85 % y descentrada,
y por eso el icono se veia mas pequeno que los demas en la barra de tareas.

Nota sobre el maestro: el PNG que dio el modelo de imagen venia con un damero
de "transparencia" PINTADO en los pixeles (modo RGB, sin canal alfa). Se
retiro por relleno por inundacion antes de commitear el maestro; aqui ya no
hay nada que quitar, solo se comprueba.
"""
import argparse
import os
import sys

from PIL import Image

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(RAIZ, "resources", "icon")
MAESTRO = os.path.join(DIR, "nt_icon.png")

# The heights Windows actually asks for. 16 is the taskbar at 100 % and
# the most demanding: there the monitor must still be readable.
TAMANOS = [16, 24, 32, 48, 64, 128, 256]
LADO_MAESTRO = 1024


def comprobar_alfa(im, donde):
    """El maestro TIENE que llevar alfa real: sin esto el icono sale con el
    fondo del damero o con un cuadro blanco en las barras oscuras."""
    if im.mode != "RGBA":
        sys.exit("%s: modo %s, se necesita RGBA (alfa real)" % (donde, im.mode))
    lo, hi = im.getchannel("A").getextrema()
    if lo == 255:
        sys.exit("%s: no hay transparencia (alfa 255..255). El fondo esta "
                 "pintado, hay que quitarlo antes." % donde)
    if hi != 255:
        sys.exit("%s: alfa maximo %d, hay pixeles semitransparentes sin "
                 "resolver" % (donde, hi))
    return lo, hi


def cuadrar(im):
    """Deja el motivo centrado en un lienzo cuadrado SIN deformarlo: recorta un
    cuadrado del lado mayor y rellena el eje corto con transparencia."""
    w, h = im.size
    if w == h:
        return im
    lado = max(w, h)
    out = Image.new("RGBA", (lado, lado), (0, 0, 0, 0))
    out.paste(im, ((lado - w) // 2, (lado - h) // 2))
    return out


# Fraction of the canvas the MOTIF must fill (its larger dimension). The tile is
# trimmed of its transparent margin and rescaled until it reaches the edges, like
# the rest of the system icons (Chrome, Drawio, Basecamp... measure 100 %). The
# original master had the motif at ~90 % x 85 % and shifted up, which is what made
# the app icon look "smaller" than the others in the taskbar and menus.
RELLENO = 1.0


def normalizar(im, relleno=RELLENO):
    """Recorta el margen transparente y escala el motivo para que ocupe `relleno`
    del lienzo, centrado. Devuelve (imagen, (ancho %, alto %))."""
    caja = im.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()
    if not caja:
        return im, (0.0, 0.0)
    motivo = im.crop(caja)
    lado = max(im.size)
    escala = (lado * relleno) / max(motivo.size)
    nuevo = motivo.resize(
        (max(1, round(motivo.width * escala)),
         max(1, round(motivo.height * escala))), Image.LANCZOS)
    out = Image.new("RGBA", (lado, lado), (0, 0, 0, 0))
    out.paste(nuevo, ((lado - nuevo.width) // 2, (lado - nuevo.height) // 2))
    return out, (100.0 * nuevo.width / lado, 100.0 * nuevo.height / lado)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cuadrar", action="store_true",
                    help="cuadra el maestro antes de generar (lo reescribe)")
    args = ap.parse_args()

    if not os.path.exists(MAESTRO):
        sys.exit("no existe el maestro: %s" % MAESTRO)
    im = Image.open(MAESTRO)
    comprobar_alfa(im, MAESTRO)

    im = cuadrar(im)
    if im.size != (LADO_MAESTRO, LADO_MAESTRO):
        im = im.resize((LADO_MAESTRO, LADO_MAESTRO), Image.LANCZOS)
    # Normaliza el encuadre: recorta el margen transparente y reescala el motivo
    # para que llene el lienzo. Sin esto la baldosa ocupaba ~90 % x 85 % y se veia
    # mas pequena que los demas iconos del sistema.
    im, ocup = normalizar(im)
    if args.cuadrar:
        im.save(MAESTRO)
        print("maestro cuadrado y normalizado, reescrito: %dx%d" % im.size)
    comprobar_alfa(im, "maestro")

    print("maestro %dx%d, la baldosa ocupa el %.1f%% x %.1f%% del lienzo"
          % (im.width, im.height, ocup[0], ocup[1]))

    for t in TAMANOS:
        p = os.path.join(DIR, "nt_%d.png" % t)
        im.resize((t, t), Image.LANCZOS).save(p)
        print("  nt_%-3d.png  %5.1f KB" % (t, os.path.getsize(p) / 1024))

    ico = os.path.join(DIR, "nt.ico")
    im.save(ico, sizes=[(t, t) for t in TAMANOS])
    print("  nt.ico      %5.1f KB" % (os.path.getsize(ico) / 1024))

    # Check: the .ico must return one entry per size.
    with Image.open(ico) as vi:
        dentro = sorted(vi.info.get("sizes", []))
    fuera = sorted(set(TAMANOS) - {t for t, _ in dentro})
    print("entradas en el .ico: %s" % ", ".join("%dx%d" % s for s in dentro))
    if fuera:
        sys.exit("el .ico NO tiene: %s" % fuera)


if __name__ == "__main__":
    main()
