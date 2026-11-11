// EPUB 2 / basic EPUB 3 reader core for the DSi manga reader.
//
// Deliberately standalone: only stdio/stdlib/string, no libnds or libfat, so the
// whole parser can be exercised by the host tests in tools/.  The reader UI in
// enhanced.c only asks this module for pages; nothing here touches PDF, TXT or
// audio state.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct Epub Epub;

#define EPUB_COLS 30          // characters per line (8 px glyphs at x = 8)
#define EPUB_ROWS 46          // text lines per page: 24 top + 22 bottom (footer row reserved)

enum { EPUB_LINE_TEXT, EPUB_LINE_BLANK };

typedef struct {
    uint32_t off;             // into epub_text()
    uint16_t len;
    uint8_t  kind;            // EPUB_LINE_TEXT / EPUB_LINE_BLANK
} EpubLine;

typedef struct {
    int first, n;             // line range for a text page
    int img;                  // >= 0: this page is one full-page image
} EpubPage;

Epub *epub_open(const char *path);
void  epub_close(Epub *e);

const char *epub_title(const Epub *e);
int   epub_chapters(const Epub *e);
const char *epub_chapter_title(const Epub *e, int ch);

// Parse chapter `ch` into pages.  Returns the page count, or -1 on failure.
// Invalidates any pointers/indices from a previously loaded chapter.
int   epub_load_chapter(Epub *e, int ch);
int   epub_pages(const Epub *e);
const EpubPage *epub_page(const Epub *e, int pg);
int   epub_line_count(const Epub *e);
const EpubLine *epub_line(const Epub *e, int i);
const char *epub_text(const Epub *e);

int   epub_toc_count(const Epub *e);
const char *epub_toc_title(const Epub *e, int i);
int   epub_toc_chapter(const Epub *e, int i);

int   epub_image_count(const Epub *e);
int   epub_cover(const Epub *e);                 // image id, or -1
int   epub_first_image(Epub *e, int ch);         // first image of a chapter, or -1
// type: 1 = baseline/progressive JPEG, 2 = PNG, 0 = unsupported.
int   epub_image_info(Epub *e, int id, int *w, int *h, int *gray, int *type);
// Same, but on bytes already extracted with epub_image_data() (avoids
// decompressing an entry twice).  Returns 0 if usable, 1 if unsupported.
int   epub_image_probe(const uint8_t *data, size_t len, int *w, int *h, int *gray, int *type);
// *buf is malloc'd; caller frees it.  Raw (still JPEG/PNG-encoded) bytes.
int   epub_image_data(Epub *e, int id, uint8_t **buf, size_t *len);

// Minimal PNG decoder: 8-bit, non-interlaced, colour types 0/2/3/4/6, alpha is
// ignored (transparent pixels become white).  Returns an RGB565 buffer scaled
// to at most `maxw` pixels wide; *out is malloc'd.
int   epub_png_decode(const uint8_t *data, size_t len, int maxw,
                      uint16_t **out, int *ow, int *oh);
