#!/usr/bin/env python3
"""Builds test PDFs (fake JPEG payloads) in several layouts and checks that
pdf.c recovers every page's bytes in the right order.
usage: python3 tools/gen_test_pdfs.py /path/to/test_pdf_binary"""
import os, random, subprocess, sys, tempfile

random.seed(1)
def blob(i):  # looks like a JPEG to the parser; includes tricky bytes
    return b"\xff\xd8" + bytes(random.randrange(256) for _ in range(300 + i * 37)) + b"stream endobj\r\n" + b"\xff\xd9"

def build(npages, *, xref=True, nested=False, indirect_len=False, indirect_res=False,
          crlf=False, shuffle=False, damage_xref=False, flate_decoy=False):
    blobs = [blob(i) for i in range(npages)]
    objs = {}                      # num -> bytes (body between obj/endobj, may include stream)
    nxt = [3 + npages * 3 + 10]
    def new(): nxt[0] += 1; return nxt[0]
    img, page = {}, {}
    order = list(range(npages))
    for i in range(npages):
        img[i] = 3 + i * 3; page[i] = 4 + i * 3
        res = 5 + i * 3
        lenref = new() if indirect_len else None
        L = b"%d 0 R" % lenref if indirect_len else b"%d" % len(blobs[i])
        eol = b"\r\n" if crlf else b"\n"
        objs[img[i]] = (b"<< /Type /XObject /Subtype /Image /Width 10 /Height 10 /ColorSpace /DeviceGray "
                        b"/BitsPerComponent 8 /Filter /DCTDecode /Length " + L + b" >>\nstream" + eol + blobs[i] + b"\nendstream")
        if indirect_len: objs[lenref] = b"%d" % len(blobs[i])
        xo = b"<< /Im0 %d 0 R >>" % img[i]
        resd = b"<< /ProcSet [/PDF] /XObject " + xo + b" >>"
        if indirect_res:
            objs[res] = resd; rr = b"%d 0 R" % res
        else:
            rr = resd
        objs[page[i]] = b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 10 10] /Resources " + rr + b" >>"
    if flate_decoy:  # an earlier image that must NOT be picked up as a page image
        d = new(); objs[d] = b"<< /Subtype /Image /Filter /FlateDecode /Length 4 >>\nstream\nabcd\nendstream"
    pages_order = order[:]
    if shuffle: random.shuffle(pages_order)           # tree order != object order
    if nested:
        h = len(pages_order) // 2
        a, b = new(), new()
        objs[a] = b"<< /Type /Pages /Parent 2 0 R /Count %d /Kids [" % h + b" ".join(b"%d 0 R" % page[i] for i in pages_order[:h]) + b"] >>"
        objs[b] = b"<< /Type /Pages /Parent 2 0 R /Count %d /Kids [" % (len(pages_order) - h) + b" ".join(b"%d 0 R" % page[i] for i in pages_order[h:]) + b"] >>"
        kids = b"%d 0 R %d 0 R" % (a, b)
    else:
        kids = b" ".join(b"%d 0 R" % page[i] for i in pages_order)
    objs[2] = b"<< /Type /Pages /Count %d /Kids [" % npages + kids + b"] >>"
    objs[1] = b"<< /Type /Catalog /Pages 2 0 R >>"

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offs = {}
    for n in sorted(objs):
        offs[n] = len(out)
        out += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
    top = max(objs) + 1
    xpos = len(out)
    if xref:
        out += b"xref\n0 %d\n0000000000 65535 f \n" % top
        for n in range(1, top):
            out += (b"%010d 00000 n \n" % offs[n]) if n in offs else b"0000000000 65535 f \n"
        out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (top, xpos)
    else:
        out += b"trailer\n<< /Size %d /Root 1 0 R >>\n%%%%EOF\n" % top
    if damage_xref:  # offsets all wrong -> must fall back to scanning
        out = out.replace(b"startxref\n%d" % xpos, b"startxref\n%d" % (xpos + 5))
    # expected: tree order, or object order if tree unusable
    exp = [blobs[i] for i in (pages_order if not damage_xref else order)]
    return bytes(out), exp

cases = {
 "classic flat":            dict(npages=7),
 "crlf after stream":       dict(npages=5, crlf=True),
 "indirect /Length":        dict(npages=5, indirect_len=True),
 "indirect /Resources":     dict(npages=5, indirect_res=True),
 "nested tree + shuffled":  dict(npages=9, nested=True, shuffle=True),
 "everything at once":      dict(npages=12, nested=True, shuffle=True, indirect_len=True, indirect_res=True, crlf=True),
 "flate decoy ignored":     dict(npages=4, flate_decoy=True),
 "no xref (recovery scan)": dict(npages=6, xref=False),
 "bad xref (recovery)":     dict(npages=6, damage_xref=True),
 "300 pages flat":          dict(npages=300),
}
bin_ = sys.argv[1]
bad = 0
for name, kw in cases.items():
    data, exp = build(**kw)
    with tempfile.NamedTemporaryFile(suffix=".pdf", delete=False) as t:
        t.write(data); path = t.name
    r = subprocess.run([bin_, path], capture_output=True, text=True)
    os.unlink(path)
    lines = r.stdout.split("\n")
    ok = r.returncode == 0 and r.stderr == "" and lines[0] == str(len(exp))
    if ok:
        for i, e in enumerate(exp):
            off, ln = map(int, lines[1 + i].split())
            if data[off:off + ln] != e: ok = False; break
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  -> got %r %s" % (lines[0], r.stderr[:200])))
    bad += not ok
sys.exit(bad)
