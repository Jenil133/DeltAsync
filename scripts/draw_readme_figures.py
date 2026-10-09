#!/usr/bin/env python3
"""Draw the hand-designed README figures in docs/assets/ as SVG.

Each figure is written twice, as a light and a dark variant for a <picture>
element, with a transparent background, system font stacks and no scripts,
links or external resources, so it renders inside an <img> on github.com.

  banner-{light,dark}.svg          1280 x 320 title banner: two versions of a
                                   file as rows of blocks, matching blocks
                                   tied together, the one changed block
                                   crossing the wire
  hero-{light,dark}.svg            the 64 MiB file as a 128 x 64 grid of 8 KiB
                                   blocks with the two blocks a 4 KiB overwrite
                                   touches, and two bars to one scale: the file
                                   itself against signature + delta
  rolling-window-{light,dark}.svg  how the encoder slides a window over the
                                   new file, looks each position up by its
                                   weak checksum and confirms a hit with
                                   SHA-256 before emitting COPY

The hero's numbers are the output of the LargeFileTest suite
(`ctest --preset release -R LargeFileTest -V`), which encodes a seeded
64 MiB file with a 4 KiB overwrite that starts 6,000 bytes into block 4,224
(tests/large_file_test.cpp). They are deterministic, and they are repeated
here as constants with that source rather than typed in from memory:

  file 67,108,864 B, block size 8,192 B (8,192 blocks), signature 163,853 B,
  delta 19,059 B of which 16,384 B literal (the two damaged blocks).

The banner and the rolling-window figure are schematic: their block counts
are not measurements.

Usage: draw_readme_figures.py [--out-dir DIR]   (default docs/assets/)

Colours mean the same thing as in plot_netem.py: teal is data the receiver
already has (COPY), amber is data that crosses the wire (LITERAL), slate is
the signature and control traffic.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_netem import MONO, SANS, THEMES, Svg, esc, fmt, human, write_svg  # noqa: E402

# LargeFileTest.FourKibOverwriteGivesATinyDelta (tests/large_file_test.cpp)
FILE_BYTES = 64 << 20
BLOCK_BYTES = 8192
BLOCKS = FILE_BYTES // BLOCK_BYTES          # 8,192
EDIT_OFFSET = (33 << 20) + 6000
EDIT_BYTES = 4096
SIGNATURE_BYTES = 163853
DELTA_BYTES = 19059
LITERAL_BYTES = 16384
DIRTY_BLOCKS = list(range(EDIT_OFFSET // BLOCK_BYTES,
                          (EDIT_OFFSET + EDIT_BYTES - 1) // BLOCK_BYTES + 1))  # [4224, 4225]
GRID_COLUMNS = 128
GRID_ROWS = BLOCKS // GRID_COLUMNS          # 64


# --- banner ------------------------------------------------------------------

def banner(theme_name):
    theme = THEMES[theme_name]
    W, H = 1280, 320
    svg = Svg(W, H, theme, "Deltasync: rolling-hash file sync over TCP, in C++17")

    svg.text(64, 150, "Deltasync", size=64, weight="700")
    svg.text(64, 196, "Rolling-hash file sync over TCP, in C++17", size=22, fill=theme["muted"])
    svg.text(64, 236, "Only the changed block crosses the wire.", size=15, fill=theme["muted"])

    # Two versions of one file, as rows of blocks.
    count, pitch, bw, bh = 22, 27, 22, 30
    x_start = 620
    y_old, y_new = 72, 218
    changed = 13
    svg.text(x_start, y_old - 14, "old version, at the receiver", size=13, fill=theme["muted"])
    svg.text(x_start, y_new + bh + 20, "new version, at the sender", size=13,
             fill=theme["muted"])
    wire_y = (y_old + bh + y_new) / 2
    svg.line(x_start - 2, wire_y, x_start + count * pitch - 3, wire_y, theme["slate"], 1,
             opacity=0.6)
    svg.text(x_start - 10, wire_y + 4, "TCP", size=12, fill=theme["slate"], anchor="end",
             mono=True)
    for i in range(count):
        x = x_start + i * pitch
        cx = x + bw / 2
        if i == changed:
            # The stale block at the receiver, and the new one that replaces it.
            svg.rect(x, y_old, bw, bh, theme["faint"], rx=3, stroke=theme["amber"],
                     stroke_width=1.5)
            svg.rect(x, y_new, bw, bh, theme["amber"], rx=3)
            # The one block in flight, with an arrow up to the receiver.
            svg.line(cx, y_new, cx, y_old + bh + 10, theme["amber"], 2)
            svg.path("M %.1f %.1f l -5 8 l 10 0 z" % (cx, y_old + bh + 2), theme["amber"], 1,
                     fill=theme["amber"])
            svg.rect(cx - 7, wire_y - 7, 14, 14, theme["amber"], rx=2,
                     stroke=theme["surface"], stroke_width=2)
        else:
            svg.rect(x, y_old, bw, bh, theme["teal"], rx=3)
            svg.rect(x, y_new, bw, bh, theme["teal"], rx=3)
            svg.line(cx, y_old + bh, cx, y_new, theme["teal"], 1.5, dash="2,4", opacity=0.7)
    return svg.render()


# --- hero: the file as a grid of blocks, and two bars to one scale -----------

def hero(theme_name):
    theme = THEMES[theme_name]
    W, H = 880, 640
    svg = Svg(W, H, theme, "A 4 KiB overwrite in a 64 MiB file: 2 of 8,192 blocks are resent; "
              "signature plus delta is 182,912 bytes against 67,108,864 for a full copy")
    pitch, cell = 6, 5
    gx, gy = 56, 74
    gw, gh = GRID_COLUMNS * pitch, GRID_ROWS * pitch

    svg.text(24, 30, "You changed %s of a %s file. This is what crosses the wire."
             % (human(EDIT_BYTES), human(FILE_BYTES)), size=16, weight="600")
    svg.text(24, 52, "One cell per %s block, %s blocks in all. %d are resent as literal data; "
             "the other %s are reused from the receiver's copy."
             % (human(BLOCK_BYTES), fmt(BLOCKS), len(DIRTY_BLOCKS), fmt(BLOCKS - len(DIRTY_BLOCKS))),
             size=12, fill=theme["muted"])
    svg.text(gx, gy - 7, "block 0", size=12, fill=theme["muted"], mono=True)
    svg.text(gx + gw, gy - 7, "block %s" % fmt(BLOCKS - 1), size=12, fill=theme["muted"],
             anchor="end", mono=True)

    # The reused blocks as a pattern, the damaged ones drawn on top.
    svg.parts.append(
        '<defs><pattern id="cells" width="%d" height="%d" patternUnits="userSpaceOnUse">'
        '<rect width="%d" height="%d" fill="%s" rx="0.5"/></pattern></defs>'
        % (pitch, pitch, cell, cell, theme["teal"]))
    svg.parts.append('<rect x="%d" y="%d" width="%d" height="%d" fill="url(#cells)"/>'
                     % (gx, gy, gw, gh))
    for block in DIRTY_BLOCKS:
        row, col = divmod(block, GRID_COLUMNS)
        svg.rect(gx + col * pitch, gy + row * pitch, cell, cell, theme["amber"])
    first_row, first_col = divmod(DIRTY_BLOCKS[0], GRID_COLUMNS)
    last_row, last_col = divmod(DIRTY_BLOCKS[-1], GRID_COLUMNS)
    ring_x, ring_y = gx + first_col * pitch - 4, gy + first_row * pitch - 4
    ring_w = (last_col - first_col + 1) * pitch + 7
    ring_h = (last_row - first_row + 1) * pitch + 7
    svg.rect(ring_x, ring_y, ring_w, ring_h, "none", rx=3, stroke=theme["amber"],
             stroke_width=1.5)
    # Callout on a small opaque panel, since text on the grid would be unreadable.
    note = ("blocks %s and %s: the %s overwrite starts %s B into block %s, so two blocks "
            "are resent" % (fmt(DIRTY_BLOCKS[0]), fmt(DIRTY_BLOCKS[-1]), human(EDIT_BYTES),
                            fmt(EDIT_OFFSET % BLOCK_BYTES), fmt(DIRTY_BLOCKS[0])))
    box_x, box_y, box_w = ring_x + 28, ring_y - 34, 6.6 * len(note) + 16
    svg.line(ring_x + ring_w, ring_y, box_x + 4, box_y + 24, theme["amber"], 1)
    svg.rect(box_x, box_y, box_w, 24, theme["faint"], rx=4, stroke=theme["amber"],
             stroke_width=1)
    svg.text(box_x + 8, box_y + 16, note, size=12)
    svg.text(gx - 8, gy + first_row * pitch + 5, "%s" % human(DIRTY_BLOCKS[0] * BLOCK_BYTES),
             size=12, fill=theme["muted"], anchor="end", mono=True)

    # Two bars, one scale.
    bx, bar_w = 190, 634
    scale = bar_w / FILE_BYTES
    total = SIGNATURE_BYTES + DELTA_BYTES
    y = gy + gh + 28
    svg.text(bx - 14, y + 17, "Full copy", size=13, anchor="end", weight="600")
    svg.rect(bx, y, FILE_BYTES * scale, 24, theme["amber"], rx=3)
    svg.text(bx + bar_w - 10, y + 17, "%s B, the whole file" % fmt(FILE_BYTES), size=13,
             fill=theme["surface"], anchor="end", mono=True)
    y2 = y + 40
    sliver = total * scale
    svg.text(bx - 14, y2 + 17, "Deltasync", size=13, anchor="end", weight="600")
    svg.rect(bx, y2, max(sliver, 0.5), 24, theme["slate"])
    svg.rect(bx, y2, max(DELTA_BYTES * scale, 0.25), 24, theme["amber"])
    svg.text(bx + sliver + 12, y2 + 17, "%s B: signature %s + delta %s (%.2f %% of the file)"
             % (fmt(total), fmt(SIGNATURE_BYTES), fmt(DELTA_BYTES), total / FILE_BYTES * 100),
             size=13, mono=True)
    zoom = 100
    y3 = y2 + 50
    sig_w = SIGNATURE_BYTES * scale * zoom
    delta_w = DELTA_BYTES * scale * zoom
    svg.line(bx, y2 + 24, bx, y3, theme["muted"], 1, dash="3,3")
    svg.line(bx + sliver, y2 + 24, bx + sig_w + 2 + delta_w, y3, theme["muted"], 1, dash="3,3")
    svg.text(bx - 14, y3 + 17, "x %d" % zoom, size=13, anchor="end", fill=theme["muted"],
             mono=True)
    svg.rect(bx, y3, sig_w, 24, theme["slate"], rx=3)
    svg.rect(bx + sig_w + 2, y3, delta_w, 24, theme["amber"], rx=3)
    svg.text(bx + sig_w / 2, y3 + 17, "signature %s B" % fmt(SIGNATURE_BYTES), size=12,
             fill=theme["surface"], anchor="middle", mono=True)
    svg.text(bx + sig_w + 2 + delta_w + 10, y3 + 17, "delta %s B, of which %s B literal"
             % (fmt(DELTA_BYTES), fmt(LITERAL_BYTES)), size=12, mono=True)

    ly = H - 18
    x = 24
    for colour, label in ((theme["teal"], "reused: COPY"), (theme["amber"], "sent: LITERAL"),
                          (theme["slate"], "signature and control")):
        svg.rect(x, ly - 10, 14, 14, colour, rx=2)
        svg.text(x + 20, ly + 1, label, size=12, fill=theme["muted"])
        x += 20 + 6.2 * len(label) + 28
    svg.text(W - 24, ly + 1, "measured: ctest --preset release -R LargeFileTest", size=12,
             fill=theme["muted"], anchor="end", mono=True)
    return svg.render()


# --- rolling window --------------------------------------------------------

def rolling_window(theme_name):
    theme = THEMES[theme_name]
    W, H = 560, 330
    svg = Svg(W, H, theme, "A window slides one byte at a time over the new file; each "
              "position is looked up by its weak checksum and a hit is confirmed with SHA-256")
    svg.text(16, 24, "Rolling a window over the new file", size=14, weight="600")

    # The signature: one (weak, strong) pair per basis block.
    svg.text(16, 50, "signature of the old file: (weak u32, SHA-256[16]) per block", size=12,
             fill=theme["muted"])
    bw, bh, pitch = 40, 22, 45
    sy = 58
    for i in range(10):
        x = 16 + i * pitch
        svg.rect(x, sy, bw, bh, theme["slate"], rx=3)
        svg.text(x + bw / 2, sy + 15, "b%d" % i, size=12, fill=theme["surface"],
                 anchor="middle", mono=True)

    # The new file as a strip of bytes, with the window over it.
    ty = 136
    svg.text(16, ty - 10, "new file, byte by byte", size=12, fill=theme["muted"])
    cw = 11
    n = 36
    window_start, window_len = 22, 8
    for i in range(n):
        x = 16 + i * cw
        if i < 16:
            colour = theme["teal"]        # matched blocks, already emitted as COPY
        elif i < window_start:
            colour = theme["amber"]       # pending literal: bytes the window left behind
        else:
            colour = theme["faint"]       # not scanned yet
        svg.rect(x, ty, cw - 1, 22, colour, rx=1)
    wx = 16 + window_start * cw
    ww = window_len * cw - 1
    svg.rect(wx - 1.5, ty - 1.5, ww + 3, 25, "none", rx=2, stroke=theme["text"],
             stroke_width=1.5)
    svg.text(wx + ww / 2, ty + 15, "window", size=12, anchor="middle", weight="600")
    # Slide direction, to the right of the strip.
    ax = 16 + n * cw + 8
    svg.line(ax, ty + 11, ax + 26, ty + 11, theme["text"], 1.5)
    svg.path("M %.1f %.1f l -6 -4 l 0 8 z" % (ax + 28, ty + 11), theme["text"], 1,
             fill=theme["text"])
    svg.text(ax + 34, ty + 15, "1 byte", size=12, mono=True)
    # Weak-sum lookup from the window into the signature.
    svg.line(wx + ww / 2, ty - 2, 16 + 5 * pitch + bw / 2, sy + bh + 2, theme["slate"], 1,
             dash="3,3")
    svg.text(wx + ww / 2 - 10, ty - 30, "weak-sum lookup", size=12, fill=theme["muted"],
             anchor="end")
    svg.text(16, ty + 40, "COPY: matched, reused", size=12, fill=theme["teal"])
    svg.text(16 + 16 * cw, ty + 40, "pending LITERAL", size=12, fill=theme["amber"])

    # The two outcomes.
    oy = ty + 74
    svg.rect(16, oy - 12, 14, 14, theme["amber"], rx=2)
    svg.text(36, oy, "weak miss (almost every byte)", size=12, weight="600")
    svg.text(36, oy + 18, "slide one byte; the byte that leaves the window joins the pending "
             "LITERAL", size=12, fill=theme["muted"])
    oy += 46
    svg.rect(16, oy - 12, 14, 14, theme["teal"], rx=2)
    svg.text(36, oy, "weak hit", size=12, weight="600")
    svg.text(36, oy + 18, "SHA-256 the window; if it matches the block's strong sum, emit "
             "COPY", size=12, fill=theme["muted"])
    svg.text(36, oy + 34, "and jump a whole block ahead", size=12, fill=theme["muted"])
    svg.text(16, H - 14, "weak = a | b << 16, updated in O(1) per byte; a weak hit is never "
             "trusted without SHA-256", size=12, fill=theme["muted"])
    return svg.render()


# --- main ---------------------------------------------------------------------

def main(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--out-dir", default=os.path.join(here, "..", "docs", "assets"))
    args = parser.parse_args(argv)
    os.makedirs(args.out_dir, exist_ok=True)
    assert BLOCKS == GRID_COLUMNS * GRID_ROWS
    for theme in ("light", "dark"):
        for name, draw in (("banner", banner), ("hero", hero), ("rolling-window", rolling_window)):
            path = os.path.join(args.out_dir, "%s-%s.svg" % (name, theme))
            write_svg(path, draw(theme))
            print(path)
    print("hero: file %s B, %s blocks of %s B, dirty blocks %s, signature %s B, delta %s B "
          "(literal %s B)" % (fmt(FILE_BYTES), fmt(BLOCKS), fmt(BLOCK_BYTES), DIRTY_BLOCKS,
                              fmt(SIGNATURE_BYTES), fmt(DELTA_BYTES), fmt(LITERAL_BYTES)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
