#!/usr/bin/env python3
"""Self-tests for make_test_files.py, csv_to_markdown.py, plot_netem.py and
draw_readme_figures.py.

Run directly or through ctest (test name scripts.selftest).
"""

import csv
import hashlib
import io
import os
import random
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

SCRIPTS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPTS)

import csv_to_markdown  # noqa: E402
import draw_readme_figures  # noqa: E402
import make_test_files  # noqa: E402
import plot_netem  # noqa: E402


def generate(directory, size, seed, offset, edit_size, kind):
    base = os.path.join(directory, "base.bin")
    edited = os.path.join(directory, "edited.bin")
    out = subprocess.run(
        [sys.executable, os.path.join(SCRIPTS, "make_test_files.py"), "--size", str(size),
         "--seed", str(seed), "--edit-offset", str(offset), "--edit-size", str(edit_size),
         "--edit-kind", kind, "--base", base, "--edited", edited],
        check=True, capture_output=True, text=True).stdout
    with open(base, "rb") as f:
        base_bytes = f.read()
    with open(edited, "rb") as f:
        edited_bytes = f.read()
    return base_bytes, edited_bytes, out


class MakeTestFilesTest(unittest.TestCase):
    def test_parse_size(self):
        self.assertEqual(make_test_files.parse_size("0"), 0)
        self.assertEqual(make_test_files.parse_size("4K"), 4096)
        self.assertEqual(make_test_files.parse_size("3m"), 3 << 20)
        self.assertEqual(make_test_files.parse_size("1G"), 1 << 30)
        self.assertEqual(make_test_files.parse_size("2KB"), 2048)

    def test_edits_match_a_simple_model(self):
        piece = make_test_files.PIECE
        rng = random.Random(5)
        cases = [(0, 0, 0), (10, 0, 10), (10, 10, 0), (10, 3, 4),
                 (3 * piece, piece, 4096), (3 * piece, piece - 2, 4),
                 (3 * piece, 3 * piece - 4096, 4096), (3 * piece + 7, 3 * piece + 7, 0)]
        for _ in range(6):
            size = rng.randint(0, 3 * piece)
            offset = rng.randint(0, size)
            cases.append((size, offset, rng.randint(0, min(9000, size - offset))))
        with tempfile.TemporaryDirectory() as directory:
            for size, offset, edit_size in cases:
                for kind in ("overwrite", "insert", "delete"):
                    with self.subTest(size=size, offset=offset, edit=edit_size, kind=kind):
                        base, edited, out = generate(directory, size, 9, offset, edit_size, kind)
                        self.assertEqual(len(base), size)
                        new = random.Random(9 ^ 0x5EED_ED17).randbytes(edit_size)
                        if kind == "overwrite":
                            expected = base[:offset] + new + base[offset + edit_size:]
                        elif kind == "insert":
                            expected = base[:offset] + new + base[offset:]
                        else:
                            expected = base[:offset] + base[offset + edit_size:]
                        self.assertEqual(edited, expected)
                        digests = [line.split()[0] for line in out.splitlines()]
                        self.assertEqual(digests, [hashlib.sha256(base).hexdigest(),
                                                   hashlib.sha256(edited).hexdigest()])

    def test_same_seed_same_file_and_different_seed_differs(self):
        with tempfile.TemporaryDirectory() as directory:
            a, _, _ = generate(directory, 100000, 1, 0, 0, "overwrite")
            b, _, _ = generate(directory, 100000, 1, 0, 0, "overwrite")
            c, _, _ = generate(directory, 100000, 2, 0, 0, "overwrite")
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)

    def test_rejects_an_edit_past_the_end(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, os.path.join(SCRIPTS, "make_test_files.py"), "--size", "100",
                 "--edit-offset", "90", "--edit-size", "20", "--base",
                 os.path.join(directory, "b"), "--edited", os.path.join(directory, "e")],
                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)


def rows_csv(rows):
    header = ("delay_ms,loss_pct,scenario,run,exit_code,mode,file_size,bytes_sent,"
              "bytes_received,socket_bytes,signature_bytes,delta_bytes,literal_bytes,"
              "matched_bytes,resumed_from,attempts,elapsed_ms,wall_ms,verified,note").split(",")
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=header, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow({k: row.get(k, "") for k in header})
    return buffer.getvalue()


class CsvToMarkdownTest(unittest.TestCase):
    # Synthetic rows that only exercise the formatting; they are not
    # measurements.
    ROWS = [
        {"delay_ms": "20", "loss_pct": "0", "scenario": "delta", "run": "1", "exit_code": "0",
         "bytes_sent": "100", "bytes_received": "50", "socket_bytes": "150",
         "elapsed_ms": "10", "wall_ms": "12", "verified": "yes"},
        {"delay_ms": "20", "loss_pct": "0", "scenario": "delta", "run": "2", "exit_code": "0",
         "bytes_sent": "300", "bytes_received": "50", "socket_bytes": "350",
         "elapsed_ms": "30", "wall_ms": "32", "verified": "yes"},
        {"delay_ms": "20", "loss_pct": "0", "scenario": "delta", "run": "3", "exit_code": "1",
         "socket_bytes": "999999", "verified": "no"},
        {"delay_ms": "0", "loss_pct": "0.5", "scenario": "interrupted", "run": "1",
         "exit_code": "1", "socket_bytes": "5000", "verified": "no",
         "note": "proxy forwarded 5000 bytes"},
        {"delay_ms": "0", "loss_pct": "0.5", "scenario": "resumed", "run": "1", "exit_code": "0",
         "socket_bytes": "6000", "resumed_from": "4096", "verified": "yes",
         "note": "a | b"},
        {"delay_ms": "0", "loss_pct": "0.5", "scenario": "whole-file", "run": "1",
         "exit_code": "0", "socket_bytes": "1234567", "verified": "yes"},
    ]

    def table(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "netem.csv")
            with open(path, "w") as f:
                f.write(rows_csv(self.ROWS))
            output = os.path.join(directory, "table.md")
            self.assertEqual(csv_to_markdown.main([path, "--output", output]), 0)
            with open(output) as f:
                return f.read().splitlines()

    def test_rows_are_sorted_and_summarised(self):
        lines = self.table()
        self.assertTrue(lines[0].startswith("| delay ms | loss % | scenario | runs | ok |"))
        body = [line.split(" | ") for line in lines[2:]]
        self.assertEqual([(r[0].lstrip("| "), r[1], r[2]) for r in body],
                         [("0", "0.5", "whole-file"), ("0", "0.5", "interrupted"),
                          ("0", "0.5", "resumed"), ("20", "0", "delta")])
        delta = body[3]
        self.assertEqual(delta[3], "3")    # runs
        self.assertEqual(delta[4], "2")    # ok
        self.assertEqual(delta[5], "250")  # median of 150 and 350; the failed run is excluded
        self.assertEqual(body[0][5], "1,234,567")
        self.assertEqual(body[1][5], "5,000")  # interrupted runs count although they fail
        self.assertEqual(body[2][8], "4,096")  # resumed from
        self.assertIn("a / b", lines[4])        # pipes cannot break the table
        self.assertNotIn("proxy forwarded", "\n".join(lines))

    def test_every_line_has_the_same_number_of_cells(self):
        lines = self.table()
        counts = {line.count(" | ") for line in lines}
        self.assertEqual(len(counts), 1)


def plot_rows():
    """Synthetic rows that only exercise the drawing; they are not
    measurements. Two grid points, two runs each, plus one failed delta run."""
    rows = []
    for delay, loss, whole_ms, delta_ms in (("0", "0", "1000", "2000"),
                                            ("50", "1", "6000", "2000")):
        for run in ("1", "2"):
            rows += [
                {"delay_ms": delay, "loss_pct": loss, "scenario": "whole-file", "run": run,
                 "exit_code": "0", "file_size": "4194304", "bytes_sent": "4194500",
                 "bytes_received": "80", "wall_ms": whole_ms, "verified": "yes"},
                {"delay_ms": delay, "loss_pct": loss, "scenario": "delta", "run": run,
                 "exit_code": "0", "file_size": "4194304", "bytes_sent": "4500",
                 "bytes_received": "41000", "signature_bytes": "40973", "delta_bytes": "4310",
                 "literal_bytes": "4096", "wall_ms": delta_ms, "verified": "yes"},
                {"delay_ms": delay, "loss_pct": loss, "scenario": "interrupted", "run": run,
                 "exit_code": "1", "bytes_sent": "2097152", "bytes_received": "56",
                 "wall_ms": "500", "verified": "no", "note": "proxy forwarded 2097208 bytes"},
                {"delay_ms": delay, "loss_pct": loss, "scenario": "resumed", "run": run,
                 "exit_code": "0", "file_size": "4194304", "bytes_sent": "3146305",
                 "bytes_received": "77", "resumed_from": "1048576", "wall_ms": "700",
                 "verified": "yes"},
            ]
    rows.append({"delay_ms": "0", "loss_pct": "0", "scenario": "delta", "run": "3",
                 "exit_code": "1", "file_size": "4194304", "bytes_sent": "999999999",
                 "bytes_received": "1", "wall_ms": "999999", "verified": "no"})
    return rows


class PlotNetemTest(unittest.TestCase):
    ROWS = plot_rows()

    def test_figures_are_well_formed_and_show_the_medians(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "netem.csv")
            with open(path, "w") as f:
                f.write(rows_csv(self.ROWS))
            out = os.path.join(directory, "assets")
            self.assertEqual(plot_netem.main([path, "--headline", path, "--out-dir", out]), 0)
            names = sorted(os.listdir(out))
            self.assertEqual(names, ["netem-bytes-dark.svg", "netem-bytes-light.svg",
                                     "netem-resume-dark.svg", "netem-resume-light.svg",
                                     "netem-time-dark.svg", "netem-time-light.svg"])
            for name in names:
                with open(os.path.join(out, name)) as f:
                    content = f.read()
                root = ET.fromstring(content)  # parses
                self.assertEqual(root.get("width"), root.get("viewBox").split()[2])
                self.assertNotIn("<script", content)
                self.assertNotIn("href=", content)
                for size in set(re.findall(r'font-size="([0-9.]+)"', content)):
                    self.assertGreaterEqual(float(size), 12.0)
            with open(os.path.join(out, "netem-bytes-light.svg")) as f:
                bytes_svg = f.read()
            self.assertIn("4,194,500 B sent", bytes_svg)   # the failed delta run is left out
            self.assertIn("4,500 B sent", bytes_svg)
            self.assertIn("41,000 B back", bytes_svg)
            with open(os.path.join(out, "netem-time-light.svg")) as f:
                time_svg = f.read()
            self.assertIn("0.5x", time_svg)                # 1000 / 2000 at 0 ms / 0 %
            self.assertIn("3.0x", time_svg)                # 6000 / 2000 at 50 ms / 1 %
            self.assertIn("6,000 / 2,000 ms", time_svg)
            with open(os.path.join(out, "netem-resume-light.svg")) as f:
                resume_svg = f.read()
            self.assertIn("1,048,576 B kept", resume_svg)
            self.assertIn("3,146,305 B sent", resume_svg)
            self.assertIn("2,097,152 B crossed", resume_svg)

    def test_colours_keep_heatmap_text_readable(self):
        for theme in plot_netem.THEMES.values():
            for ratio in (0.5, 0.7, 1.0, 1.5, 3.0, 5.2):
                fill = plot_netem.ratio_colour(ratio, 5.2, theme)
                self.assertGreaterEqual(plot_netem.contrast(theme["text"], fill), 4.5,
                                        (theme["text"], fill, ratio))


class DrawReadmeFiguresTest(unittest.TestCase):
    def test_figures_are_well_formed(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(draw_readme_figures.main(["--out-dir", directory]), 0)
            names = sorted(os.listdir(directory))
            self.assertEqual(names, ["banner-dark.svg", "banner-light.svg", "hero-dark.svg",
                                     "hero-light.svg", "rolling-window-dark.svg",
                                     "rolling-window-light.svg"])
            for name in names:
                with open(os.path.join(directory, name)) as f:
                    content = f.read()
                ET.fromstring(content)
                self.assertNotIn("<script", content)
                self.assertNotIn("href=", content)
            with open(os.path.join(directory, "hero-light.svg")) as f:
                hero = f.read()
            # The dirty blocks of the LargeFileTest overwrite: 33 MiB + 6,000 B, 4 KiB long.
            self.assertEqual(draw_readme_figures.DIRTY_BLOCKS, [4224, 4225])
            self.assertIn("blocks 4,224 and 4,225", hero)
            self.assertIn("signature 163,853", hero)
            self.assertIn("delta 19,059", hero)
            self.assertIn('fill="url(#cells)"', hero)


if __name__ == "__main__":
    unittest.main()
