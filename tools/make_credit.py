#!/usr/bin/env python3
"""make_credit.py -- the one-line credit shown on the start-up loading screen.

Usage: make_credit.py <BerlinSansFBDemi-Bold.ttf> <out.png> [text]

The text is drawn in the game's own font (Berlin Sans FB Demi Bold, from the
game's res/com/gfx/font) in the colour of the loading screen's percentage
counter: plain beige (150, 120, 100), no outline or shadow -- the game draws its
"45%" exactly like that (libcgame 0x8fbbe sets that colour before drawing it).
The PNG is made at the exact size it is shown on the Vita's 960x544 screen (no
scaling, so it stays sharp).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

font_path, out = sys.argv[1], sys.argv[2]
text = sys.argv[3] if len(sys.argv) > 3 else "PS Vita port by rodi · v1.0"

SIZE = 22                  # font size in screen pixels
COLOUR = (150, 120, 100)   # the loading percentage's colour
PAD = 2

font = ImageFont.truetype(font_path, SIZE)
l, t, r, b = font.getbbox(text)
w, h = r - l + 2 * PAD, b - t + 2 * PAD
img = Image.new("RGBA", (w, h), COLOUR + (0,))   # transparent, same colour (clean edges when scaled)
ImageDraw.Draw(img).text((PAD - l, PAD - t), text, font=font, fill=COLOUR + (255,))
img.save(out, optimize=True)
print(f"{out}: {w}x{h} \"{text}\"")
