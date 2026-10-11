#!/usr/bin/env python3
"""Pack the editable dashboard literal into a deterministic gzip flash asset."""

import argparse
import gzip
import hashlib
import io
from pathlib import Path
import re


def dashboard_html(source: Path) -> bytes:
    # Universal newline decoding makes checkout line endings irrelevant.
    text = source.read_text(encoding="utf-8")
    matches = re.findall(r'R"HTML\((.*?)\)HTML"', text, flags=re.DOTALL)
    if len(matches) != 1:
        raise ValueError(f"Expected one dashboard HTML literal in {source}")
    return matches[0].encode("utf-8")


def compress_html(html: bytes) -> bytes:
    output = io.BytesIO()
    # Empty filename and fixed time avoid path and timestamp differences.
    with gzip.GzipFile(fileobj=output, mode="wb", filename="", mtime=0,
                       compresslevel=9) as stream:
        stream.write(html)
    return output.getvalue()


def write_if_changed(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != content:
        path.write_bytes(content)


def pack_dashboard(source: Path, output_dir: Path) -> tuple[int, int]:
    html = dashboard_html(source)
    packed = compress_html(html)
    gzip_path = output_dir / "dashboard.html.gz"
    asm_path = output_dir / "dashboard_html.S"
    write_if_changed(gzip_path, packed)
    assembly = (
        f'/* Dashboard gzip SHA-256: {hashlib.sha256(packed).hexdigest()} */\n'
        '.section .rodata\n'
        '.balign 4\n'
        '.global dashboard_html_gz_start\n'
        'dashboard_html_gz_start:\n'
        f'.incbin "{gzip_path.resolve().as_posix()}"\n'
        '.global dashboard_html_gz_end\n'
        'dashboard_html_gz_end:\n'
    ).encode("utf-8")
    write_if_changed(asm_path, assembly)
    return len(html), len(packed)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    original, compressed = pack_dashboard(args.source, args.output_dir)
    print(f"Dashboard: {original} HTML bytes -> {compressed} gzip bytes "
          f"({original - compressed} bytes saved)")


if __name__ == "__main__":
    main()
