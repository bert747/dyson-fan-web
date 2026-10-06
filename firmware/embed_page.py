# Regenerates src/page.h from ../index.html so the board and the laptop app share one page.
from pathlib import Path

Import("env")  # noqa: F821  (provided by PlatformIO)

root = Path(env["PROJECT_DIR"])  # noqa: F821
html = (root.parent / "index.html").read_text(encoding="utf-8")
out = root / "src" / "page.h"
text = '#pragma once\n// Generated from ../index.html by embed_page.py; edit that file instead.\nstatic const char PAGE_HTML[] PROGMEM = R"PAGE(' + html + ')PAGE";\n'
if not out.exists() or out.read_text(encoding="utf-8") != text:
    out.write_text(text, encoding="utf-8")
