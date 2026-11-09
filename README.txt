DSi PDF / manga reader  -  two editions
=======================================
STANDARD  (source/main.c)      works in plain DS mode, one page decoded at a time.
ENHANCED  (source/enhanced.c)  for DSi mode / 16 MB RAM:
  * page cache: previous + current + next page kept decoded (dual view keeps 4),
    next page is decoded in the background while you read; a button press aborts it
  * higher quality: decodes at up to ~1100 px wide (X in library = normal/high),
    grayscale pages are stored at 1 byte/pixel so they get twice the detail per MB,
    zoom 1x-4x
  * views (SELECT): PORTRAIT page over both screens -> SPREAD (one wide page over both
    screens) -> DUAL (two pages, one per screen). SPREAD and DUAL are for holding the
    console sideways like a book; Y in the library picks which way you turn it.
  * library: cover grid (first page), progress bar per book, sorted by last opened,
    A resumes at the saved page, B starts at page 1. Saved in <folder>/dsimanga.lib,
    covers cached next to each PDF as <name>.pdf.thm

BUILD
  1. Install devkitPro (devkitARM, libnds, libfat).
  2. Download TJpgDec (elm-chan.org/fsw/tjpgd/00index.html), copy tjpgd.c, tjpgd.h,
     tjpgdcnf.h into source/.  In tjpgdcnf.h:  JD_FORMAT 1   JD_USE_SCALE 1   JD_TBLCLIP 1
     (enhanced.c reads jd.ncomp from the JDEC struct to detect grayscale JPEGs.)
  3. make        -> dsimanga.nds        make dsi  -> dsimanga_dsi.nds
  Put the .nds on the SD card, PDFs in /manga.

IS IT REALLY USING 16 MB?
  The library screen prints "DSi 12MB" (largest block it could allocate). If it says
  "DS 3MB" the app is running in DS mode or libnds gave it the small heap. Things to try:
  the DSIFLAGS in the Makefile, TWiLight Menu++'s per-app "run in DSi mode" setting,
  or a different linker spec (make dsi SPECS=...). The app still works either way - it
  just decodes at lower resolution when memory is short.

CONTROLS
  Library: D-pad move, A resume, B from page 1, X quality, Y console hold side, START quit
  Reader:  A/R next, B/L prev, D-pad left/right turn (dual: single page shift),
           Y reading direction (starts right-to-left), X zoom 1x-4x, SELECT view,
           zoomed: D-pad / touch-drag pan, START back to library
  Red square top-right = decoding. Red screen = page can't be decoded (progressive JPEG etc).

HOST TESTS (no DSi needed)
  gcc -O1 -o test_pdf tools/test_pdf.c source/pdf.c && python3 tools/gen_test_pdfs.py ./test_pdf
  gcc -O1 -Itools/stubs -Isource -o render_test tools/render_test.c source/pdf.c tools/stubs/stubs.c
