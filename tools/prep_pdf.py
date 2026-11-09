#!/usr/bin/env python3
"""
prep_pdf.py - turn an image-based PDF (scans, manga, PNG/lossless/JPEG2000 pages...)
into a DSi-reader-friendly PDF: one baseline JPEG per page, sensible size, classic layout.

  python prep_pdf.py book.pdf                 -> book.dsi.pdf
  python prep_pdf.py book.pdf out.pdf --width 800 --quality 75
  python prep_pdf.py book.pdf --inspect       -> just list what the pages contain

Options
  --width N     longest allowed page width in pixels (default 1000; 800 is faster on the
                standard edition, 1100 suits the DSi Enhanced edition)
  --quality N   JPEG quality 1-95 (default 80)
  --gray        force grayscale      --color  never convert to grayscale
                (default: pages that are really black & white become grayscale, which
                 decodes faster and lets the Enhanced edition store twice the detail)

Needs:  pip install pikepdf pillow
Text-only (non-image) pages can't be converted by this tool; they are skipped with a warning.
"""
import argparse, io, os, sys, warnings

warnings.simplefilter("ignore", DeprecationWarning)

try:
    import pikepdf
    from pikepdf import PdfImage
    from PIL import Image
except ImportError:
    sys.exit("Please run:  pip install pikepdf pillow")


def filters_of(raw):
    f = raw.get("/Filter")
    if f is None:
        return []
    if isinstance(f, pikepdf.Array):
        return [str(x) for x in f]
    return [str(f)]


def biggest_image(page):
    best = None
    imgs = page.get_images() if hasattr(page, "get_images") else page.images   # pikepdf versions differ
    for _name, raw in imgs.items():
        area = int(raw.Width) * int(raw.Height)
        if best is None or area > best[0]:
            best = (area, raw)
    return best[1] if best else None


def to_pil(raw):
    im = PdfImage(raw).as_pil_image()
    if im.mode in ("1", "P", "LA", "RGBA", "CMYK", "I", "I;16", "F"):
        im = im.convert("L" if im.mode in ("1", "LA") else "RGB")
    return im


def looks_gray(im):
    if im.mode == "L":
        return True
    b = im.convert("RGB").resize((64, 64)).tobytes()
    return all(max(b[i:i + 3]) - min(b[i:i + 3]) < 14 for i in range(0, len(b), 3))


class PdfWriter:
    """Tiny streaming writer: classic xref table, nothing compressed."""

    def __init__(self, f):
        self.f, self.off, self.n = f, {}, 0
        f.write(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")

    def new(self):
        self.n += 1
        return self.n

    def obj(self, num, body):
        self.off[num] = self.f.tell()
        self.f.write(b"%d 0 obj\n" % num + body + b"\nendobj\n")

    def stream(self, num, d, data):
        self.off[num] = self.f.tell()
        self.f.write(b"%d 0 obj\n<< " % num + d + b" /Length %d >>\nstream\n" % len(data) + data + b"\nendstream\nendobj\n")

    def finish(self, root):
        x = self.f.tell()
        self.f.write(b"xref\n0 %d\n0000000000 65535 f \n" % (self.n + 1))
        for i in range(1, self.n + 1):
            self.f.write(b"%010d 00000 n \n" % self.off[i])
        self.f.write(b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (self.n + 1, root, x))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--width", type=int, default=1000)
    ap.add_argument("--quality", type=int, default=80)
    ap.add_argument("--gray", action="store_true")
    ap.add_argument("--color", action="store_true")
    ap.add_argument("--inspect", action="store_true")
    a = ap.parse_args()

    pdf = pikepdf.open(a.src)
    pages = list(pdf.pages)

    if a.inspect:
        kinds = {}
        for i, page in enumerate(pages, 1):
            raw = biggest_image(page)
            if raw is None:
                desc = "no image (text/vector page)"
            else:
                fl = "+".join(x.lstrip("/") for x in filters_of(raw)) or "raw"
                desc = "%dx%d  %s  %s" % (int(raw.Width), int(raw.Height), str(raw.get("/ColorSpace", "?"))[:20], fl)
                kinds[fl] = kinds.get(fl, 0) + 1
            if i <= 5 or i == len(pages):
                print("page %d: %s" % (i, desc))
        print("\n%d pages. Image encodings: %s" % (len(pages), kinds or "none"))
        print("The DSi reader can open the file directly only if every page is a single 'DCTDecode' (JPEG).")
        return

    dst = a.dst or os.path.splitext(a.src)[0] + ".dsi.pdf"
    with open(dst, "wb") as f:
        w = PdfWriter(f)
        root, pages_obj = w.new(), w.new()
        kids = []
        skipped = 0
        for i, page in enumerate(pages, 1):
            sys.stdout.write("\rpage %d/%d" % (i, len(pages)))
            sys.stdout.flush()
            raw = biggest_image(page)
            if raw is None:
                skipped += 1
                print("\nwarning: page %d has no image, skipped" % i)
                continue
            try:
                im = to_pil(raw)
            except Exception as e:                      # unsupported encoding etc.
                skipped += 1
                print("\nwarning: page %d: can't decode image (%s), skipped" % (i, e))
                continue
            if im.width > a.width:
                im = im.resize((a.width, max(1, round(im.height * a.width / im.width))), Image.LANCZOS)
            if a.gray or (not a.color and looks_gray(im)):
                im = im.convert("L")
            elif im.mode != "L":
                im = im.convert("RGB")
            buf = io.BytesIO()
            im.save(buf, "JPEG", quality=a.quality, progressive=False)
            iw, ih = im.size
            cs = b"/DeviceGray" if im.mode == "L" else b"/DeviceRGB"
            img, con, pg = w.new(), w.new(), w.new()
            w.stream(img, b"/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace %s "
                          b"/BitsPerComponent 8 /Filter /DCTDecode" % (iw, ih, cs), buf.getvalue())
            w.stream(con, b"", b"q %d 0 0 %d 0 0 cm /Im0 Do Q" % (iw, ih))
            w.obj(pg, b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %d %d] /Contents %d 0 R "
                      b"/Resources << /XObject << /Im0 %d 0 R >> >> >>" % (pages_obj, iw, ih, con, img))
            kids.append(pg)
        w.obj(pages_obj, b"<< /Type /Pages /Count %d /Kids [%s] >>" % (len(kids), b" ".join(b"%d 0 R" % k for k in kids)))
        w.obj(root, b"<< /Type /Catalog /Pages %d 0 R >>" % pages_obj)
        w.finish(root)
    print("\nwrote %s  (%d pages%s, %.1f MB)" % (dst, len(kids), ", %d skipped" % skipped if skipped else "", os.path.getsize(dst) / 1e6))


if __name__ == "__main__":
    main()
