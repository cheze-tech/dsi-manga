// Host test: EPUB container, OPF metadata/spine, EPUB 3 nav + EPUB 2 NCX TOC,
// chapter text extraction, page building, embedded JPEG bytes and PNG decode.
#include "epub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) exit(2);
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int line_has(Epub *e, const char *needle) {
    const char *txt = epub_text(e);
    for (int i = 0; i < epub_line_count(e); i++) {
        const EpubLine *l = epub_line(e, i);
        if (l->kind != EPUB_LINE_TEXT) continue;
        char b[EPUB_COLS + 1];
        int n = l->len > EPUB_COLS ? EPUB_COLS : l->len;
        memcpy(b, txt + l->off, (size_t)n);
        b[n] = 0;
        if (strstr(b, needle)) return 1;
    }
    return 0;
}

static int has_text(Epub *e, int ch, const char *needle) {
    assert(epub_load_chapter(e, ch) > 0);
    return line_has(e, needle);
}

static int first_img_page(Epub *e, int ch) {
    assert(epub_load_chapter(e, ch) > 0);
    for (int p = 0; p < epub_pages(e); p++) {
        const EpubPage *pg = epub_page(e, p);
        if (pg->img >= 0) return pg->img;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/epub_fixtures";
    char p[512];

    snprintf(p, sizeof p, "%s/good.epub", dir);
    Epub *e = epub_open(p);
    if (!e) { fprintf(stderr, "could not open good.epub\n"); return 1; }
    assert(!strcmp(epub_title(e), "Fixture & Test"));
    assert(epub_chapters(e) == 2);
    assert(epub_toc_count(e) == 2);
    assert(!strcmp(epub_toc_title(e, 0), "Chapter One"));
    assert(epub_toc_chapter(e, 1) == 1);
    assert(epub_cover(e) >= 0);

    int pages = epub_load_chapter(e, 0);
    printf("ch1: %d pages, %d lines\n", pages, epub_line_count(e));
    assert(pages > 1);
    assert(line_has(e, "Hello & welcome"));
    assert(line_has(e, "Quotes"));
    assert(line_has(e, "After the image."));

    int id = first_img_page(e, 0);
    assert(id >= 0);
    int w = 0, h = 0, gray = 0, type = 0;
    assert(epub_image_info(e, id, &w, &h, &gray, &type) == 0);
    assert(type == 1 && w == 80 && h == 100 && gray == 0);
    uint8_t *data = NULL;
    size_t len = 0;
    assert(epub_image_data(e, id, &data, &len) == 0);
    size_t jn = 0;
    char jp[512];
    snprintf(jp, sizeof jp, "%s/img1.jpg", dir);
    char *jpg = slurp(jp, &jn);
    assert(len == jn && !memcmp(data, jpg, jn));
    free(data);
    free(jpg);

    int pid = first_img_page(e, 1);
    assert(pid >= 0);
    assert(epub_image_info(e, pid, &w, &h, &gray, &type) == 0);
    assert(type == 2 && w == 8 && h == 6);
    assert(epub_image_data(e, pid, &data, &len) == 0);
    uint16_t *px = NULL;
    int ow = 0, oh = 0;
    assert(epub_png_decode(data, len, 8, &px, &ow, &oh) == 0);
    assert(ow == 8 && oh == 6);
    uint16_t want = (uint16_t)(((64 & 0xF8) << 8) | ((40 & 0xFC) << 3) | (128 >> 3));
    assert(px[1 * ow + 2] == want);
    free(px);
    px = NULL;
    assert(epub_png_decode(data, len, 4, &px, &ow, &oh) == 0);
    assert(ow == 4 && oh == 3);
    assert(px[0] == (uint16_t)(128 >> 3));
    free(px);
    free(data);
    epub_close(e);

    snprintf(p, sizeof p, "%s/epub2.epub", dir);
    e = epub_open(p);
    assert(e);
    assert(!strcmp(epub_title(e), "EPUB Two"));
    assert(epub_chapters(e) == 1);
    assert(epub_toc_count(e) == 1);
    assert(!strcmp(epub_toc_title(e, 0), "EPUB Two Chapter"));
    assert(has_text(e, 0, "Hello & welcome"));
    epub_close(e);

    // ZIP method 0, and deflate with stored blocks (compresslevel 0).
    const char *other[] = { "stored.epub", "level0.epub" };
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, other[i]);
        e = epub_open(p);
        assert(e);
        assert(epub_chapters(e) == 1);
        assert(has_text(e, 0, "Hello & welcome"));
        epub_close(e);
    }

    snprintf(p, sizeof p, "%s/bad.epub", dir);
    assert(epub_open(p) == NULL);
    assert(epub_open("/tmp/definitely-missing.epub") == NULL);
    puts("epub test: OK");
    return 0;
}
