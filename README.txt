DSi PDF / manga reader  -  two editions
=======================================
STANDARD  (source/main.c)      works in plain DS mode, one page decoded at a time.
ENHANCED  (source/enhanced.c)  for DSi mode / 16 MB RAM:
  * page cache: books under 5 MB keep 5 pages decoded (current, 2 back, 2 ahead);
    larger books keep 3. Neighbours are cached as fast low-resolution previews
    while you read; the visible page is upgraded first, in small slices. A button
    press never waits for a whole JPEG decode, and previews never evict the page
    on screen. The library shows the heap, page budget and peak cache RAM.
  * zoom: X redraws immediately from the cached image, then decodes the current
    page once at the widest useful detail in the background (up to 4 MB, current
    page only; neighbours are evicted if RAM is tight). 2x/3x/4x reuse that
    decode instead of re-decoding, and full resolution is used when it fits
    (usual for grayscale manga). Decode targets follow the actual view width
    (256 portrait, 384+gap spread), not 384*f; X in the library selects
    normal/high for unzoomed pages.
  * views (SELECT): PORTRAIT page over both screens -> SPREAD (one wide page over both
    screens) -> DUAL (two pages, one per screen). SPREAD and DUAL are for holding the
    console sideways like a book; Y in the library picks which way you turn it.
  * library: cover grid (first page), green progress bar per book, sorted by last opened,
    A resumes at the saved page, B starts at page 1. Saved in <folder>/dsimanga.lib,
    covers cached next to each PDF as <name>.pdf.thm

BUILD
  1. Install devkitPro (devkitARM, libnds, libfat).
  2. Download TJpgDec (elm-chan.org/fsw/tjpgd/00index.html), copy tjpgd.c, tjpgd.h,
     tjpgdcnf.h into source/.  In tjpgdcnf.h:  JD_USE_SCALE 1   JD_TBLCLIP 1
     JD_FASTDECODE 1  (the 32-bit ARM9 path; JD_FORMAT 0 or 1 both work).
     (enhanced.c reads jd.ncomp from the JDEC struct to detect grayscale JPEGs.)
  3. make        -> dsimanga.nds        make dsi  -> dsimanga_dsi.nds
  Put the .nds on the SD card, PDFs in /manga.
  The DSi build uses high title ID 00030000 (DSi-enhanced gamecard). Do not
  change it to 00030004: that is a DSiWare title ID and cannot be launched as
  a .nds card; melonDS can crash at boot with it.

IS IT REALLY USING 16 MB?
  The library screen prints "DSi 12MB" (largest block it could allocate) plus the
  KB budget per cached page and the peak cache RAM of the last book. If it says
  "DS 3MB" the app is running in DS mode or libnds gave it the small heap. Check
  that melonDS has DSi BIOS/firmware configured and DSi mode enabled, or use
  TWiLight Menu++'s per-app "run in DSi mode" setting. You can also try a different
  linker spec (make dsi SPECS=...). The app still works either way - it just decodes
  at lower resolution when memory is short.

CONTROLS
  Library: D-pad move, A resume, B from page 1, X quality, Y console hold side, START quit
  Reader:  A/R next, B/L prev, D-pad left/right turn (dual: single page shift),
           Y reading direction (starts right-to-left), X zoom 1x-4x, SELECT view,
           zoomed: D-pad / touch-drag pan, START back to library
           Green line along the bottom of the lower screen = reading progress
  Red square top-right = decoding. Red screen = page can't be decoded (progressive JPEG etc).

HOST TESTS (no DSi needed)
  gcc -O1 -o test_pdf tools/test_pdf.c source/pdf.c && python3 tools/gen_test_pdfs.py ./test_pdf
  gcc -O1 -Itools/stubs -Isource -o render_test tools/render_test.c source/pdf.c tools/stubs/stubs.c
