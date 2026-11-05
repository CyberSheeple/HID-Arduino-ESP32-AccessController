#!/usr/bin/env python3
"""Write the Nano ESP32 web page into a PROGMEM header.

The sketch serves this header. DFU upload replaces only the application,
so the page is not stored as a filesystem image.
"""

import sys
from pathlib import Path


def render(html):
    if ")rawliteral" in html:
        raise SystemExit("HTML contains the raw-string terminator )rawliteral")
    return (
        "#pragma once\n"
        "// Generated from data/index.html by scripts/embed_index_html.py.\n"
        "const char INDEX_HTML[] PROGMEM = R\"rawliteral(\n"
        + html
        + ")rawliteral\";\n"
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} index.html index_html.h")
    source = Path(sys.argv[1])
    dest = Path(sys.argv[2])
    content = render(source.read_text(encoding="utf-8"))
    if dest.is_file() and dest.read_text(encoding="utf-8") == content:
        return
    dest.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
