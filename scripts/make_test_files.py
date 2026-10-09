#!/usr/bin/env python3
"""Create a deterministic random file and an edited copy of it.

The base file is pseudo-random bytes from a seed, so the same size and seed
always produce the same file on any machine with Python 3.9+. The edited copy
differs from the base by one small edit at a chosen offset:

  overwrite  replace EDIT_SIZE bytes at OFFSET with new random bytes
  insert     insert EDIT_SIZE new random bytes at OFFSET
  delete     remove EDIT_SIZE bytes starting at OFFSET

Both files are written in pieces, so memory use stays small for large files.
The SHA-256 digest of each output is printed, one "<hex>  <path>" line each
(the same format as sha256sum).

Example:
  make_test_files.py --size 64M --seed 1 --edit-offset 32M --edit-size 4K \\
      --base build/bench/basis.bin --edited build/bench/target.bin
"""

import argparse
import hashlib
import os
import random
import sys

PIECE = 1 << 20  # generator granularity; part of the file definition


def parse_size(text):
    """Byte count with an optional K, M or G suffix (powers of 1024)."""
    units = {"": 1, "K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    text = text.strip().upper()
    if text.endswith("B"):
        text = text[:-1]
    suffix = text[-1:] if text[-1:] in ("K", "M", "G") else ""
    number = text[: len(text) - len(suffix)]
    if not number.isdigit():
        raise argparse.ArgumentTypeError("not a size: %r" % text)
    return int(number) * units[suffix]


def base_pieces(size, seed):
    """Yields the base file as pieces of at most PIECE bytes."""
    rng = random.Random(seed)
    remaining = size
    while remaining > 0:
        n = min(PIECE, remaining)
        yield rng.randbytes(n)
        remaining -= n


def edited_pieces(size, seed, offset, edit_size, kind):
    """Yields the edited copy: base[:offset] + new bytes + base[skip_end:]."""
    replacement = random.Random(seed ^ 0x5EED_ED17).randbytes(edit_size)
    skip_end = offset if kind == "insert" else offset + edit_size
    position = 0
    spliced = False
    for piece in base_pieces(size, seed):
        start = position
        position += len(piece)
        if start < offset:
            yield piece[: offset - start]
        if not spliced and position >= offset:
            if kind != "delete":
                yield replacement
            spliced = True
        if position > skip_end:
            yield piece[max(0, skip_end - start):]
    if not spliced and kind != "delete":  # empty base file
        yield replacement


def write_file(path, pieces):
    digest = hashlib.sha256()
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    with open(path, "wb") as f:
        for piece in pieces:
            f.write(piece)
            digest.update(piece)
    return digest.hexdigest()


def main(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--size", type=parse_size, required=True, help="base file size")
    parser.add_argument("--seed", type=int, default=1, help="generator seed (default 1)")
    parser.add_argument("--edit-offset", type=parse_size, default=None,
                        help="where the edit starts (default: middle of the file)")
    parser.add_argument("--edit-size", type=parse_size, default=4096,
                        help="bytes overwritten, inserted or deleted (default 4K)")
    parser.add_argument("--edit-kind", choices=("overwrite", "insert", "delete"),
                        default="overwrite")
    parser.add_argument("--base", required=True, help="path of the base file to write")
    parser.add_argument("--edited", help="path of the edited copy to write (optional)")
    args = parser.parse_args(argv)

    offset = args.size // 2 if args.edit_offset is None else args.edit_offset
    if args.edited:
        if offset > args.size:
            parser.error("--edit-offset is past the end of the file")
        if args.edit_kind != "insert" and offset + args.edit_size > args.size:
            parser.error("the edit runs past the end of the file")

    print("%s  %s" % (write_file(args.base, base_pieces(args.size, args.seed)), args.base))
    if args.edited:
        pieces = edited_pieces(args.size, args.seed, offset, args.edit_size, args.edit_kind)
        print("%s  %s" % (write_file(args.edited, pieces), args.edited))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
