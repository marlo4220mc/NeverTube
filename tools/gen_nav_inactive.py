#!/usr/bin/env python3
"""Puts the INACTIVE sprites of the bottom bar on the app's muted token.

`inactive` and `disabled` are the only nav sprites that have no variant per
accent: `NavButton::updateIcon()` loads `<key>_inactive.png` with no suffix for
every accent color, so their gray is shared by the whole bar. The four old ones
(home, search, library, settings) came with #414248, a gray so dark on #101010
that the icon is almost invisible, while the `downloads` one -- drawn by
`gen_nav_downloads.py`, the newest of the set -- used #8F8F8F, which is
`nt::kMuted`, the token the app uses for everything that is read but not
emphasized. Seen together in the bar, downloads looked "lighter" than the rest.

This sets all of them to kMuted, with the same exact recolor that
`gen_nav_accent.cpp` uses for the accent variants: the RGB is forced and ONLY
the alpha is kept (the glyph shape and its antialiasing survive; the edge
pixels get the full color with partial alpha, which is what already happens
with the pre-painted accent sprites).

Idempotent: running it again leaves the files as they are.

Usage: gen_nav_inactive.py <dir-nav>
"""

import os
import sys

from PIL import Image

# nt::kMuted (src/ui/theme.h).
K_MUTED = (0x8F, 0x8F, 0x8F)

KEYS = ("home", "search", "library", "downloads", "settings")


def recolor(path):
    im = Image.open(path)
    if im.mode != "RGBA":
        im = im.convert("RGBA")
    px = im.load()
    changed = 0
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            if (r, g, b) != K_MUTED:
                changed += 1
            px[x, y] = (K_MUTED[0], K_MUTED[1], K_MUTED[2], a)
    im.save(path)
    return changed


def main():
    if len(sys.argv) < 2:
        print("usage: %s <dir-nav>" % sys.argv[0], file=sys.stderr)
        return 2
    out = sys.argv[1]
    total = 0
    for key in KEYS:
        path = os.path.join(out, "%s_inactive.png" % key)
        if not os.path.exists(path):
            print("MISSING %s" % path, file=sys.stderr)
            return 1
        total += recolor(path)
        print("%s_inactive.png" % key)
    if total == 0:
        print("already on kMuted (#8F8F8F)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
