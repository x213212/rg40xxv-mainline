#!/usr/bin/env python3
"""Build the tiny runtime atlas from UIOS Material Symbols Rounded."""

import argparse

from PIL import Image, ImageDraw, ImageFont


ICONS = [
    ("history", 0xE8B3),
    ("library_books", 0xE02F),
    ("favorite", 0xE87E),
    ("cast", 0xE307),
    ("apps", 0xE5C3),
    ("settings", 0xE8B8),
    ("wifi", 0xE63E),
    ("battery_full", 0xE1A5),
    ("volume_up", 0xE050),
    ("search", 0xEF7A),
    ("play_arrow", 0xE037),
    ("tune", 0xE429),
    ("power_settings_new", 0xF8C7),
]
CELL = 32


def font_variant(path, fill):
    font = ImageFont.truetype(path, 26)
    if hasattr(font, "set_variation_by_axes"):
        font.set_variation_by_axes([float(fill), 0.0, 24.0, 400.0])
    return font


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_font")
    parser.add_argument("output")
    arguments = parser.parse_args()
    image = Image.new("RGBA", (len(ICONS) * CELL, CELL * 2), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    for row, fill in enumerate((0, 1)):
        font = font_variant(arguments.source_font, fill)
        for column, (_, codepoint) in enumerate(ICONS):
            draw.text(
                (column * CELL + CELL / 2, row * CELL + CELL / 2),
                chr(codepoint),
                font=font,
                fill=(255, 255, 255, 255),
                anchor="mm",
                stroke_width=0,
            )
    image.save(arguments.output, optimize=True)
    print(
        "ICON_ATLAS_RESULT PASS icons=%d cells=%dx%d output=%s"
        % (len(ICONS), CELL, CELL, arguments.output)
    )


if __name__ == "__main__":
    main()
