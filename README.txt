DSi PDF / manga reader  -  two editions
=======================================
STANDARD  (source/main.c)      works in plain DS mode, one page decoded at a time.
ENHANCED  (source/enhanced.c)  for DSi mode / 16 MB RAM:
  * page cache: books under 5 MB keep 5 pages decoded (current, 2 back, 2 ahead);
    larger books keep 3. Neighbours are cached as fast low-resolution previews;
    the incoming page's preview is filled before the current page's detail
    upgrade, so a page turn never waits for the whole JPEG decode. Previews never
    evict the page on screen. The library shows the heap, page budget and peak
    cache RAM.
  * zoom: X redraws immediately from the cached image, then decodes the current
    page once at the widest useful detail in the background (up to 4 MB, current
    page only; neighbours are evicted if RAM is tight). 2x/3x/4x reuse that
    decode instead of re-decoding, and full resolution is used when it fits
    (usual for grayscale manga). Decode targets follow the actual view width
    (256 portrait, 384+gap spread), not 384*f; X in the library selects
    normal/high for unzoomed pages.
  * views (SELECT): PORTRAIT page over both screens -> SPREAD (one wide page over both
    screens) -> DUAL (two pages, one per screen). SPREAD and DUAL are for holding the
    console sideways like a book; Y in the reader flips the reading direction.
  * page turns: the upcoming page's cheap preview is filled before the current page's
    multi-second detail decode, so the next turn does not wait for a JPEG decode. The
    short bar at the top of the top screen shows current-page detail progress.
    Detail upgrades are staged: a faster intermediate scale is shown before the final
    scale, and final quality is unchanged. Detail work pauses while the user is
    panning/turning so scrolling keeps priority, then resumes when input stops. Very
    large source images are not decoded beyond twice the pixels the display can use.
  * library: cover grid (first page), green progress bar per book, sorted by last opened,
    A resumes at the saved page, B starts at page 1. Saved in <folder>/dsimanga.lib,
    covers cached next to each PDF as <name>.pdf.thm
  * EPUB (DSi edition only): .epub files join the library next to PDFs and TXTs.
    Own ZIP/OPF/XHTML module (source/epub.c); the PDF, TXT and music paths are
    untouched. Supports EPUB 2 and basic EPUB 3, spine order, EPUB 3 nav or
    EPUB 2 NCX contents (SELECT), wrapped text pages, full-screen embedded
    JPEG/PNG illustrations (plus SVG <image> wrappers used by fixed-layout
    comics), chapter progress and cached covers. Text is ASCII: common Latin-1
    accents and smart punctuation are mapped, CJK glyphs are not available.
    CSS layout (floats/tables/columns), embedded fonts and DRM are not rendered.
    Baseline JPEG and 8-bit non-interlaced PNG work; progressive JPEG is not
    supported by TJpgDec. Opening or switching chapters costs a ZIP decompress,
    then page turns are immediate; the next illustration is prefetched.

BUILD
  1. Install devkitPro (devkitARM, libnds, libfat), plus maxmod and libmad for DSi
     music:
       dkp-pacman -S --needed nds-dev nds-libmad
     nds-libmad installs mad.h and libmad.a into portlibs/nds; maxmod provides
     libmm9.a. The official devkitpro/devkitarm Docker image already contains both.
  2. Download TJpgDec (elm-chan.org/fsw/tjpgd/00index.html), copy tjpgd.c, tjpgd.h,
     tjpgdcnf.h into source/.  In tjpgdcnf.h:  JD_USE_SCALE 1   JD_TBLCLIP 1
     JD_FASTDECODE 1  (the 32-bit ARM9 path; JD_FORMAT 0 or 1 both work).
     (enhanced.c reads jd.ncomp from the JDEC struct to detect grayscale JPEGs.)
  3. make        -> dsimanga.nds        make dsi  -> dsimanga_dsi.nds
  Put the .nds on the SD card, PDFs/EPUBs in /manga and MP3s in /mp3.
  DSi music is enabled by default (`-lmm9 -lmad`, `-DUSE_AUDIO`). Use
  `make dsi AUDIO=0` for a build without music. AUDIO must be 0 or 1.
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

MUSIC
  Put MP3 files in /mp3. The DSi edition plays them shuffled by default and shows
  the current file name in the library. The reader has a compact top-left overlay
  with NOW / NEXT (and PAUSED while paused); an empty /mp3 folder shows
  "No MP3s in /mp3". Every new track also shows a temporary NOW PLAYING banner
  near the top of the reader for about three seconds; the compact NOW / NEXT
  overlay remains, and the banner does not restart on page redraws. L+UP toggles
  the compact overlay on/off (default on); the temporary new-track banner is
  independent and always remains enabled.
  The music menu shows embedded ID3 APIC/PIC JPEG artwork when available, cached
  for the current track. Missing, unsupported, corrupt or oversized artwork shows
  a NO ART placeholder. Artwork decoding is limited to the menu and does not run
  during reader scrolling or page decoding.
  L+A+DOWN in the reader opens the music menu, where the list of detected tracks,
  the current track, the planned next track, manual selection, pause/resume and
  next-track controls are available. In the menu, A plays the selected track,
  Y pauses/resumes, R skips, D-pad up/down browses, left/right jumps ten tracks,
  and B or START closes it. Music keeps playing while the menu is open. Empty
  folders and unsupported files are skipped; if no track can be decoded the
  overlay shows that music is unavailable and reading continues.
  MP3s are decoded on ARM9 by libmad; Maxmod feeds the DS sound hardware.

SLEEP (DSi edition)
  Closing the lid uses libnds/Calico's pmMainLoop sleep handling. The reader,
  page/zoom/scroll position, display layout and an open music menu are preserved.
  Music is paused while asleep and resumes on wake only if it was playing; music
  that was already paused stays paused. Repeated lid cycles are supported.

CONTROLS
  Library: D-pad move, A resume, B from page 1, X quality, START quit
           SELECT next song, Y pause/resume music
  Reader:  A/R next, B/L prev, D-pad left/right turn (dual: two-page step),
           Y reading direction (starts right-to-left), X zoom 1x-4x, SELECT view,
           L+A+DOWN music menu, L+UP show/hide music overlay,
           zoomed: D-pad / touch-drag pan, START back to library
  EPUB (DSi edition only):
           A/R/L/B/D-pad turn pages, UP/DOWN previous/next chapter,
           SELECT table of contents, START back to library
           Green line along the bottom of the lower screen = reading progress
  Red square top-right = decoding. Blue strip at the top = current-page detail
  progress. Red screen = page can't be decoded (progressive JPEG etc).
  Music overlay top-left = NOW / NEXT, or PAUSED while paused. A temporary
  NOW PLAYING banner appears near the top when a new track starts.

HOST TESTS (no DSi needed)
  gcc -O1 -o test_pdf tools/test_pdf.c source/pdf.c && python3 tools/gen_test_pdfs.py ./test_pdf
  gcc -O1 -Isource -o id3art_test tools/id3art_test.c source/id3art.c && ./id3art_test
  gcc -O1 -Itools/stubs -Isource -o dir_test tools/dir_test.c source/pdf.c source/id3art.c source/epub.c tools/stubs/stubs.c && ./dir_test
  gcc -O1 -Itools/stubs -Isource -o render_test tools/render_test.c source/pdf.c source/id3art.c source/epub.c tools/stubs/stubs.c && ./render_test
  gcc -O1 -Itools/stubs -Isource -o text_test tools/text_test.c source/pdf.c source/id3art.c source/epub.c tools/stubs/stubs.c && ./text_test
  gcc -O1 -Isource -o epub_test tools/epub_test.c source/epub.c && python3 tools/gen_test_epubs.py /tmp/epub_fixtures && ./epub_test /tmp/epub_fixtures
  gcc -O1 -Isource -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -o epub_oom_test tools/epub_oom_test.c source/epub.c && ./epub_oom_test /tmp/epub_fixtures
