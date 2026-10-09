#!/usr/bin/env python3
"""Draw the benchmark figures in docs/assets/ from bench/results/netem.csv.

Three figures, each written twice (a light and a dark variant, for a
<picture> element in Markdown), with a transparent background:

  netem-bytes-{light,dark}.svg   bytes on the wire: whole-file push against
                                 delta push, drawn to scale with a magnified
                                 inset for the delta
  netem-time-{light,dark}.svg    heatmap over delay x loss of the median wall
                                 time ratio whole-file / delta
  netem-resume-{light,dark}.svg  bytes sent after an interruption: the
                                 resumed push against a full retry

Every number comes from the CSV: medians over the successful runs, grouped
the same way as csv_to_markdown.py. Nothing is typed in by hand, so the
figures can be rebuilt byte for byte from the same CSV. The SVGs use only
system font stacks and no scripts, links or external resources, so they
render inside an <img> on github.com.

Usage:
  plot_netem.py [CSV] [--headline CSV] [--out-dir DIR]

CSV defaults to bench/results/netem.csv and --out-dir to docs/assets/.
--headline adds a second panel to the resume figure from another run in the
same format (the 1 GiB run in bench/results/no-netem-1g.csv).

Colours mean the same thing in every figure: amber is file data sent by the
sender, slate is what the receiver sends back (signature and control frames)
and teal is data the receiver already had and did not need again. The
heatmap is the one place with a colour scale: amber below 1.0x (the delta
push was slower), teal above it.
"""

import argparse
import csv
import math
import os
import statistics
import sys
import xml.etree.ElementTree as ET

# --- theme -------------------------------------------------------------------

THEMES = {
    "light": {
        "surface": "#ffffff",   # only used to pick readable text; never painted
        "text": "#1f2328",
        "muted": "#57606a",
        "grid": "#d0d7de",
        "faint": "#eaeef2",
        "teal": "#0f9688",
        "teal_deep": "#0a4f49",
        "amber": "#b45309",
        "amber_deep": "#7a3806",
        "slate": "#6e7781",
        # heatmap ramp ends: chosen so the text colour stays >= 4.5:1 on every step
        "heat_teal": "#179c8e",
        "heat_amber": "#cf7a1f",
    },
    "dark": {
        "surface": "#0d1117",
        "text": "#e6edf3",
        "muted": "#8b949e",
        "grid": "#30363d",
        "faint": "#21262d",
        "teal": "#1a9d8f",
        "teal_deep": "#8fd4cb",
        "amber": "#d07d1f",
        "amber_deep": "#f0b56a",
        "slate": "#7f8ea3",
        "heat_teal": "#12756b",
        "heat_amber": "#915815",
    },
}

SANS = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "ui-monospace, SFMono-Regular, Menlo, Consolas, 'Liberation Mono', monospace"


def hex_to_rgb(color):
    color = color.lstrip("#")
    return tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))


def rgb_to_hex(rgb):
    return "#%02x%02x%02x" % tuple(max(0, min(255, int(round(c)))) for c in rgb)


def mix(a, b, t):
    """Linear interpolation between two hex colours, t in [0, 1]."""
    ra, rb = hex_to_rgb(a), hex_to_rgb(b)
    return rgb_to_hex(tuple(x + (y - x) * t for x, y in zip(ra, rb)))


def luminance(color):
    def channel(c):
        c /= 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (channel(c) for c in hex_to_rgb(color))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    """WCAG contrast ratio between two colours."""
    la, lb = luminance(a), luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


# --- SVG builder -------------------------------------------------------------

def esc(text):
    return (str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


class Svg:
    def __init__(self, width, height, theme, title):
        self.width = width
        self.height = height
        self.theme = theme
        self.title = title
        self.parts = []

    def rect(self, x, y, w, h, fill, rx=0, opacity=None, stroke=None, stroke_width=1):
        attrs = 'x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"' % (x, y, w, h, fill)
        if rx:
            attrs += ' rx="%g"' % rx
        if opacity is not None:
            attrs += ' fill-opacity="%g"' % opacity
        if stroke:
            attrs += ' stroke="%s" stroke-width="%g"' % (stroke, stroke_width)
        self.parts.append("<rect %s/>" % attrs)

    def line(self, x1, y1, x2, y2, stroke, width=1, dash=None, opacity=None):
        attrs = 'x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" stroke="%s" stroke-width="%g"' % (
            x1, y1, x2, y2, stroke, width)
        if dash:
            attrs += ' stroke-dasharray="%s"' % dash
        if opacity is not None:
            attrs += ' stroke-opacity="%g"' % opacity
        self.parts.append("<line %s/>" % attrs)

    def path(self, d, stroke, width=1, fill="none"):
        self.parts.append('<path d="%s" fill="%s" stroke="%s" stroke-width="%g"/>' % (
            d, fill, stroke, width))

    def text(self, x, y, s, size=13, fill=None, anchor="start", weight=None, mono=False,
             baseline=None):
        attrs = 'x="%.2f" y="%.2f" font-size="%g" fill="%s" font-family="%s"' % (
            x, y, size, fill or self.theme["text"], MONO if mono else SANS)
        if anchor != "start":
            attrs += ' text-anchor="%s"' % anchor
        if weight:
            attrs += ' font-weight="%s"' % weight
        if baseline:
            attrs += ' dominant-baseline="%s"' % baseline
        self.parts.append("<text %s>%s</text>" % (attrs, esc(s)))

    def render(self):
        head = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" '
                'height="%d" role="img" aria-labelledby="t">\n' % (
                    self.width, self.height, self.width, self.height))
        return head + "<title id=\"t\">%s</title>\n" % esc(self.title) + "\n".join(
            self.parts) + "\n</svg>\n"


def fmt(n):
    """Integer with thousands separators."""
    return "{:,}".format(int(round(n)))


def human(n):
    n = float(n)
    for unit, size in (("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if n >= size and n % size == 0:
            return "%d %s" % (n // size, unit)
        if n >= size:
            return "%.1f %s" % (n / size, unit)
    return "%d B" % n


# --- data --------------------------------------------------------------------

def number(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def median_of(rows, column):
    values = [number(r.get(column)) for r in rows]
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise SystemExit("plot_netem: %s has no data rows" % path)
    return rows


def ok_rows(rows, scenario):
    """Successful runs of a scenario; the interrupted scenario keeps its
    failed runs, since failing is the point there."""
    picked = [r for r in rows if r["scenario"] == scenario]
    if scenario != "interrupted":
        picked = [r for r in picked if r.get("exit_code") == "0"]
    return picked


def grid_points(rows):
    """Sorted distinct (delay, loss) pairs as numbers."""
    points = {(number(r["delay_ms"]), number(r["loss_pct"])) for r in rows}
    return sorted(points)


def axis_label(value):
    return ("%g" % value)


# --- figure 1: bytes on the wire --------------------------------------------

def bytes_figure(rows, theme_name):
    theme = THEMES[theme_name]
    whole = ok_rows(rows, "whole-file")
    delta = ok_rows(rows, "delta")
    if not whole or not delta:
        raise SystemExit("plot_netem: need successful whole-file and delta rows")
    w_sent = median_of(whole, "bytes_sent")
    w_recv = median_of(whole, "bytes_received")
    d_sent = median_of(delta, "bytes_sent")
    d_recv = median_of(delta, "bytes_received")
    d_sig = median_of(delta, "signature_bytes")
    d_delta = median_of(delta, "delta_bytes")
    d_literal = median_of(delta, "literal_bytes")
    file_size = median_of(delta, "file_size")

    W, H = 880, 350
    svg = Svg(W, H, theme, "Bytes on the wire: whole-file push against delta push, to scale")
    x0, bar_w = 180, 540
    scale = bar_w / (w_sent + w_recv)

    svg.text(24, 30, "Bytes on the wire after a small edit to a %s file" % human(file_size),
             size=16, weight="600")
    svg.text(24, 52, "Median of %d whole-file and %d delta pushes (every delay x loss point); "
             "TCP payload in both directions. The delta resends %s B of literal data."
             % (len(whole), len(delta), fmt(d_literal)), size=12, fill=theme["muted"])

    # Whole-file push: everything the sender has goes over the wire as literal data.
    y = 92
    svg.text(x0 - 14, y + 18, "Whole-file push", size=13, anchor="end", weight="600")
    svg.rect(x0, y, w_sent * scale, 26, theme["amber"], rx=3)
    svg.text(x0 + w_sent * scale - 10, y + 18, "%s B sent" % fmt(w_sent), size=13,
             fill=theme["surface"], anchor="end", mono=True)
    svg.text(x0 + bar_w + 10, y + 18, "+ %s B back" % fmt(w_recv), size=12,
             fill=theme["muted"], mono=True)

    # Delta push, at the same scale: a sliver.
    y2 = 142
    sliver_w = (d_sent + d_recv) * scale
    svg.text(x0 - 14, y2 + 18, "Delta push", size=13, anchor="end", weight="600")
    svg.rect(x0, y2, max(sliver_w, 0.5), 26, theme["slate"])
    svg.rect(x0, y2, max(d_sent * scale, 0.25), 26, theme["amber"])
    share = (d_sent + d_recv) / (w_sent + w_recv) * 100
    svg.text(x0 + sliver_w + 12, y2 + 18, "%s B in both directions: %.2f %% of the bar above"
             % (fmt(d_sent + d_recv), share), size=13, mono=True)

    # The sliver again, magnified, with leader lines from its two corners.
    zoom = 100
    y3 = 236
    recv_w = d_recv * scale * zoom
    sent_w = d_sent * scale * zoom
    inset_w = recv_w + 2 + sent_w
    svg.line(x0, y2 + 26, x0, y3, theme["muted"], 1, dash="3,3")
    svg.line(x0 + sliver_w, y2 + 26, x0 + inset_w, y3, theme["muted"], 1, dash="3,3")
    svg.text(x0 + inset_w + 12, y3 + 18, "the delta push, %d x wider" % zoom, size=12,
             fill=theme["muted"])
    svg.rect(x0, y3, recv_w, 26, theme["slate"], rx=3)
    svg.rect(x0 + recv_w + 2, y3, sent_w, 26, theme["amber"], rx=3)
    svg.text(x0 + recv_w / 2, y3 + 18, "%s B back" % fmt(d_recv), size=12,
             fill=theme["surface"], anchor="middle", mono=True)
    svg.text(x0, y3 + 46, "back from the receiver: signature %s B, plus HELLO, RESUME and OK"
             % fmt(d_sig), size=12, fill=theme["muted"])
    tick_x = x0 + recv_w + 2 + sent_w / 2
    svg.line(tick_x, y3 - 4, tick_x, y3 - 22, theme["muted"], 1)
    svg.text(tick_x + 6, y3 - 24, "%s B sent: delta %s B, of which %s B literal"
             % (fmt(d_sent), fmt(d_delta), fmt(d_literal)), size=12, mono=True)

    # Legend: the meanings every figure shares.
    ly = 322
    x = 24
    for colour, label in ((theme["amber"], "file data sent by the sender"),
                          (theme["slate"], "sent back by the receiver (signature and control frames)")):
        svg.rect(x, ly - 10, 14, 14, colour, rx=2)
        svg.text(x + 20, ly + 1, label, size=12, fill=theme["muted"])
        x += 20 + 6 * len(label) + 28
    return svg.render(), {
        "whole_sent": w_sent, "whole_recv": w_recv, "delta_sent": d_sent,
        "delta_recv": d_recv, "signature": d_sig, "delta_bytes": d_delta,
        "literal": d_literal, "share_pct": share,
    }


# --- figure 2: heatmap of the wall-time ratio --------------------------------

def ratio_colour(ratio, rmax, theme):
    """Diverging fill around 1.0: amber below (delta slower), teal above."""
    if ratio >= 1.0:
        t = math.log(ratio) / math.log(rmax) if rmax > 1 else 0.0
        return mix(theme["faint"], theme["heat_teal"], min(1.0, 0.15 + 0.85 * t))
    t = math.log(1.0 / ratio) / math.log(2.0)  # 0.5x would be the full amber
    return mix(theme["faint"], theme["heat_amber"], min(1.0, 0.15 + 0.85 * t))


def heatmap_figure(rows, theme_name):
    theme = THEMES[theme_name]
    points = grid_points(rows)
    delays = sorted({p[0] for p in points})
    losses = sorted({p[1] for p in points})
    cells = {}
    for delay in delays:
        for loss in losses:
            at = [r for r in rows if number(r["delay_ms"]) == delay and number(r["loss_pct"]) == loss]
            w = median_of(ok_rows(at, "whole-file"), "wall_ms")
            d = median_of(ok_rows(at, "delta"), "wall_ms")
            if w is None or d is None:
                continue
            cells[(delay, loss)] = (w, d, w / d)
    if not cells:
        raise SystemExit("plot_netem: no (delay, loss) point has both whole-file and delta runs")
    rmax = max(v[2] for v in cells.values())
    rmin = min(v[2] for v in cells.values())
    sample = ok_rows(rows, "delta")
    w_sent = median_of(ok_rows(rows, "whole-file"), "bytes_sent")
    d_sent = median_of(sample, "bytes_sent")
    d_recv = median_of(sample, "bytes_received")
    runs = max(len([r for r in rows if r["scenario"] == "delta"
                    and number(r["delay_ms"]) == p[0] and number(r["loss_pct"]) == p[1]])
               for p in points)

    cw, ch = 158, 74
    left, top = 150, 118
    W = left + cw * len(losses) + 24
    H = top + ch * len(delays) + 92
    svg = Svg(W, H, theme, "Wall time of a whole-file push divided by a delta push, "
              "over added delay and packet loss")
    svg.text(24, 30, "Wall time: whole-file push / delta push", size=16, weight="600")
    svg.text(24, 52, "Median of %d runs per cell; %s file; tc netem on the loopback "
             "(round trip = 2 x delay)." % (runs, human(median_of(sample, "file_size"))),
             size=12, fill=theme["muted"])
    svg.text(24, 70, "Bytes on the wire are the same in every cell: %s B sent for the "
             "whole file; %s B sent + %s B back for the delta." % (fmt(w_sent), fmt(d_sent),
                                                                   fmt(d_recv)),
             size=12, fill=theme["muted"])

    svg.text(left + cw * len(losses) / 2, top - 26, "packet loss, each direction (%)", size=12,
             fill=theme["muted"], anchor="middle")
    for j, loss in enumerate(losses):
        svg.text(left + cw * j + cw / 2, top - 8, axis_label(loss), size=13, anchor="middle",
                 mono=True)
    for i, delay in enumerate(delays):
        svg.text(left - 14, top + ch * i + ch / 2 + 5, "%s ms" % axis_label(delay), size=13,
                 anchor="end", mono=True)
    svg.text(24, top + ch * len(delays) / 2, "added delay", size=12, fill=theme["muted"])
    svg.text(24, top + ch * len(delays) / 2 + 16, "each direction", size=12, fill=theme["muted"])

    worst_contrast = 21.0
    for i, delay in enumerate(delays):
        for j, loss in enumerate(losses):
            if (delay, loss) not in cells:
                continue
            w, d, ratio = cells[(delay, loss)]
            fill = ratio_colour(ratio, rmax, theme)
            x, y = left + cw * j, top + ch * i
            svg.rect(x + 1, y + 1, cw - 2, ch - 2, fill, rx=4)
            ink = theme["text"]
            worst_contrast = min(worst_contrast, contrast(ink, fill))
            svg.text(x + cw / 2, y + 34, "%.1fx" % ratio, size=20, fill=ink, anchor="middle",
                     weight="600", mono=True)
            svg.text(x + cw / 2, y + 56, "%s / %s ms" % (fmt(w), fmt(d)), size=12, fill=ink,
                     anchor="middle", mono=True)

    # Colour key.
    ky = top + ch * len(delays) + 34
    svg.text(24, ky + 4, "cell: ratio, then whole-file / delta median wall time", size=12,
             fill=theme["muted"])
    kx = left + cw * len(losses) - 300
    steps = 12
    for k in range(steps):
        r = rmin * (rmax / rmin) ** ((k + 0.5) / steps) if rmin < rmax else 1.0
        svg.rect(kx + k * 16, ky - 8, 16, 16, ratio_colour(r, rmax, theme))
    svg.text(kx - 8, ky + 4, "%.1fx" % rmin, size=12, anchor="end", mono=True)
    svg.text(kx + steps * 16 + 8, ky + 4, "%.1fx" % rmax, size=12, mono=True)
    svg.text(kx + steps * 16 / 2, ky + 28, "amber: the delta push was slower; teal: faster",
             size=12, fill=theme["muted"], anchor="middle")
    return svg.render(), {"cells": cells, "worst_text_contrast": worst_contrast,
                          "ratio_min": rmin, "ratio_max": rmax}


# --- figure 3: resumed push against a full retry ------------------------------

def resume_panel(svg, rows, theme, y, x0, scale, caption):
    """Three bars to one scale: the cut attempt, the resumed push and a full
    retry. Returns the numbers drawn and the height used."""
    cut = ok_rows(rows, "interrupted")
    resumed = ok_rows(rows, "resumed")
    whole = ok_rows(rows, "whole-file")
    if not (cut and resumed and whole):
        raise SystemExit("plot_netem: need interrupted, resumed and whole-file rows")
    cut_up = median_of(cut, "bytes_sent")           # the proxy's sender-to-receiver count
    offset = median_of(resumed, "resumed_from")     # where the resumed push started
    re_sent = median_of(resumed, "bytes_sent")
    full = median_of(whole, "bytes_sent")
    file_size = median_of(whole, "file_size")
    offsets = sorted({int(number(r["resumed_from"])) for r in resumed})
    restarted = sum(1 for r in resumed if number(r["resumed_from"]) == 0)

    svg.text(24, y, caption, size=14, weight="600")
    bar_h = 24
    labels_x = x0 - 14
    rows_y = y + 40

    # 1. the attempt the proxy cut
    yy = rows_y
    svg.text(labels_x, yy + 17, "Cut attempt", size=13, anchor="end", weight="600")
    svg.rect(x0, yy, cut_up * scale, bar_h, theme["amber"], rx=3)
    svg.line(x0 + offset * scale, yy - 6, x0 + offset * scale, yy + bar_h + 6, theme["text"], 1.5)
    svg.text(x0 + cut_up * scale + 10, yy + 17, "%s B crossed before the cut" % fmt(cut_up),
             size=12, mono=True)
    svg.text(x0 + offset * scale - 6, yy - 10, "verified and journaled: %s B" % fmt(offset),
             size=12, anchor="end", fill=theme["muted"], mono=True)

    # 2. the resumed push
    yy = rows_y + 46
    svg.text(labels_x, yy + 17, "Resumed push", size=13, anchor="end", weight="600")
    svg.rect(x0, yy, offset * scale, bar_h, theme["teal"], rx=3)
    svg.rect(x0 + offset * scale + 2, yy, re_sent * scale, bar_h, theme["amber"], rx=3)
    label = "%s B kept" % fmt(offset)
    svg.text(x0 + offset * scale / 2, yy + 17, label, size=12, fill=theme["surface"],
             anchor="middle", mono=True)
    svg.text(x0 + (offset + re_sent / 2) * scale, yy + 17, "%s B sent" % fmt(re_sent), size=12,
             fill=theme["surface"], anchor="middle", mono=True)

    # 3. a full retry
    yy = rows_y + 92
    svg.text(labels_x, yy + 17, "Full retry", size=13, anchor="end", weight="600")
    svg.rect(x0, yy, full * scale, bar_h, theme["amber"], rx=3)
    svg.text(x0 + full * scale / 2, yy + 17, "%s B sent" % fmt(full), size=12,
             fill=theme["surface"], anchor="middle", mono=True)

    # Summary line under the bars.
    yy = rows_y + 140
    both = cut_up + re_sent
    extra = (both / full - 1) * 100
    svg.text(x0, yy, "Resumed push: %.1f %% of a full retry. Both attempts together: %s B, "
             "%.1f %% more than one uninterrupted push." % (re_sent / full * 100, fmt(both), extra),
             size=12, fill=theme["muted"])
    note = "%d runs" % len(resumed)
    if len(offsets) > 1:
        note += "; resume offsets seen: " + ", ".join(fmt(o) for o in offsets)
    if restarted:
        note += "; %d restarted from 0" % restarted
    svg.text(x0, yy + 18, "%s file, cut after %.0f %% of it. %s."
             % (human(file_size), cut_up / file_size * 100, note), size=12, fill=theme["muted"])
    return {"cut": cut_up, "offset": offset, "re_sent": re_sent, "full": full,
            "offsets": offsets, "both": both}, 230


def resume_figure(rows, theme_name, headline_rows=None):
    theme = THEMES[theme_name]
    panels = [(rows, "64 MiB file, whole-file push cut by the proxy, then pushed again")]
    if headline_rows:
        panels.append((headline_rows, "1 GiB file, no network emulation"))
    W = 880
    H = 96 + 236 * len(panels) + 30
    svg = Svg(W, H, theme, "Bytes sent after an interruption: resumed push against a full retry")
    svg.text(24, 30, "After an interruption: resume, or send it all again?", size=16,
             weight="600")
    svg.text(24, 52, "Sender-to-receiver bytes, medians over the runs of each scenario. The cut "
             "attempt is what the proxy forwarded before closing the connection.", size=12,
             fill=theme["muted"])
    x0, bar_w = 160, 560
    results = []
    y = 96
    for panel_rows, caption in panels:
        full = median_of(ok_rows(panel_rows, "whole-file"), "bytes_sent")
        drawn, used = resume_panel(svg, panel_rows, theme, y, x0, bar_w / full, caption)
        results.append(drawn)
        y += used + 6
    ly = H - 16
    x = 24
    for colour, label in ((theme["teal"], "already verified at the receiver, skipped"),
                          (theme["amber"], "file data sent by the sender")):
        svg.rect(x, ly - 10, 14, 14, colour, rx=2)
        svg.text(x + 20, ly + 1, label, size=12, fill=theme["muted"])
        x += 20 + 7 * len(label) + 28
    return svg.render(), results


# --- main ---------------------------------------------------------------------

def write_svg(path, content):
    ET.fromstring(content)  # refuse to write anything that does not parse
    with open(path, "w") as f:
        f.write(content)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("csv", nargs="?",
                        default=os.path.join(here, "..", "bench", "results", "netem.csv"))
    parser.add_argument("--headline", help="second CSV for the resume figure (same format)")
    parser.add_argument("--out-dir", default=os.path.join(here, "..", "docs", "assets"))
    args = parser.parse_args(argv)

    rows = load(args.csv)
    headline = load(args.headline) if args.headline else None
    os.makedirs(args.out_dir, exist_ok=True)
    for theme in ("light", "dark"):
        content, bytes_info = bytes_figure(rows, theme)
        write_svg(os.path.join(args.out_dir, "netem-bytes-%s.svg" % theme), content)
        content, heat_info = heatmap_figure(rows, theme)
        write_svg(os.path.join(args.out_dir, "netem-time-%s.svg" % theme), content)
        content, resume_info = resume_figure(rows, theme, headline)
        write_svg(os.path.join(args.out_dir, "netem-resume-%s.svg" % theme), content)
        print("%s: bytes whole %s / delta %s+%s (%.2f %%); ratio %.2fx..%.2fx, worst cell text "
              "contrast %.2f; resume %s"
              % (theme, fmt(bytes_info["whole_sent"]), fmt(bytes_info["delta_sent"]),
                 fmt(bytes_info["delta_recv"]), bytes_info["share_pct"],
                 heat_info["ratio_min"], heat_info["ratio_max"],
                 heat_info["worst_text_contrast"],
                 "; ".join("offset %s, sent %s of %s" % (fmt(r["offset"]), fmt(r["re_sent"]),
                                                         fmt(r["full"])) for r in resume_info)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
