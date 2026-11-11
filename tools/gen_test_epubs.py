#!/usr/bin/env python3
"""Build small EPUB fixtures for tools/epub_test.c.

Usage: gen_test_epubs.py [outdir]
Creates good.epub (EPUB 3 nav), epub2.epub (EPUB 2 NCX), bad.epub and the raw
JPEG/PNG payloads the C test compares against.
"""
import binascii
import os
import struct
import sys
import zipfile
import zlib

outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/epub_fixtures"
os.makedirs(outdir, exist_ok=True)

# Minimal JPEG header: SOI, APP0, SOF0 80x100 with 3 components, EOI.
JPG = (b"\xff\xd8"
       b"\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
       b"\xff\xc0\x00\x11\x08\x00\x64\x00\x50\x03"
       b"\x01\x22\x00\x02\x11\x01\x03\x11\x01"
       b"\xff\xd9")


def png_rgb(w, h):
    raw = b""
    for y in range(h):
        raw += b"\x00"
        for x in range(w):
            raw += bytes(((x * 32) % 256, (y * 40) % 256, 128))

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d +
                struct.pack(">I", binascii.crc32(t + d) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) +
            chunk(b"IEND", b""))


PNG = png_rgb(8, 6)

CONTAINER = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
 <rootfiles>
  <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
 </rootfiles>
</container>
"""

LONG = " ".join(["This is a long paragraph that has to wrap over many lines so the reader "
                 "builds more than one page from it."] * 24)

CH1 = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Ignored</title>
<style>p { color: red }</style></head><body>
<h1>Chapter One</h1>
<p>Hello &amp; welcome to the EPUB test. &ldquo;Quotes&rdquo; and &#8212; dashes.</p>
<p>%s</p>
<img src="img1.jpg" alt="one"/>
<p>After the image. Nested <b>bold</b> and <i>italic</i> text.</p>
</body></html>
""" % LONG

CH2 = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="8" height="6">
<image xlink:href="img2.png" width="8" height="6"/></svg>
</body></html>
"""

NAV = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body>
<nav epub:type="toc"><ol>
<li><a href="ch1.xhtml">Chapter One</a></li>
<li><a href="ch2.xhtml#top">Chapter Two</a></li>
</ol></nav>
</body></html>
"""

OPF3 = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:title>Fixture &amp; Test</dc:title>
 </metadata>
 <manifest>
  <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
  <item id="ch2" href="ch2.xhtml" media-type="application/xhtml+xml"/>
  <item id="jpg" href="img1.jpg" media-type="image/jpeg"/>
  <item id="png" href="img2.png" media-type="image/png"/>
  <item id="cover" href="cover.jpg" media-type="image/jpeg" properties="cover-image"/>
 </manifest>
 <spine>
  <itemref idref="ch1"/><itemref idref="ch2"/>
 </spine>
</package>
"""

OPF2 = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:title>EPUB Two</dc:title>
 </metadata>
 <manifest>
  <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  <item id="c1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
 </manifest>
 <spine toc="ncx"><itemref idref="c1"/></spine>
</package>
"""

NCX = """<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
 <navMap>
  <navPoint id="np1" playOrder="1">
   <navLabel><text>EPUB Two Chapter</text></navLabel>
   <content src="ch1.xhtml"/>
  </navPoint>
 </navMap>
</ncx>
"""


def build(path, files, order):
    with zipfile.ZipFile(path, "w") as z:
        # EPUB requires an uncompressed mimetype first.
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip",
                   compress_type=zipfile.ZIP_STORED)
        for name in order:
            data = files[name]
            comp = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            z.writestr(name, data, compress_type=comp)


files3 = {
    "META-INF/container.xml": CONTAINER,
    "OEBPS/content.opf": OPF3,
    "OEBPS/nav.xhtml": NAV,
    "OEBPS/ch1.xhtml": CH1,
    "OEBPS/ch2.xhtml": CH2,
    "OEBPS/img1.jpg": JPG,
    "OEBPS/img2.png": PNG,
    "OEBPS/cover.jpg": JPG,
}
build(os.path.join(outdir, "good.epub"), files3, list(files3))

files2 = {
    "META-INF/container.xml": CONTAINER,
    "OEBPS/content.opf": OPF2,
    "OEBPS/toc.ncx": NCX,
    "OEBPS/ch1.xhtml": CH1,
    "OEBPS/img1.jpg": JPG,
}
build(os.path.join(outdir, "epub2.epub"), files2, list(files2))

with open(os.path.join(outdir, "img1.jpg"), "wb") as f:
    f.write(JPG)
with open(os.path.join(outdir, "bad.epub"), "wb") as f:
    f.write(b"not a zip file at all" * 8)

# Stored entries (method 0) and a deflate stream made of stored blocks
# (compresslevel 0) exercise the two ZIP/inflate paths the on-device reader uses.
with zipfile.ZipFile(os.path.join(outdir, "stored.epub"), "w") as z:
    z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
    for name, data in files2.items():
        z.writestr(name, data, compress_type=zipfile.ZIP_STORED)
with zipfile.ZipFile(os.path.join(outdir, "level0.epub"), "w", compresslevel=0) as z:
    z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
    for name, data in files2.items():
        z.writestr(name, data, compress_type=zipfile.ZIP_DEFLATED)

print("fixtures in", outdir)
