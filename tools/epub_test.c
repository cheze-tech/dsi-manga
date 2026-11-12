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

    // Repeated open/close and book switching: leaks/stale state show up here.
    for (int i = 0; i < 120; i++) {
        snprintf(p, sizeof p, "%s/good.epub", dir);
        e = epub_open(p);
        assert(e);
        assert(epub_chapters(e) == 2);
        assert(epub_load_chapter(e, 0) > 0);
        assert(epub_cover(e) >= 0);
        epub_close(e);
    }
    for (int i = 0; i < 40; i++) {
        const char *names[] = { "good.epub", "epub2.epub" };
        for (int k = 0; k < 2; k++) {
            snprintf(p, sizeof p, "%s/%s", dir, names[k]);
            e = epub_open(p);
            assert(e);
            assert(epub_load_chapter(e, 0) > 0);
            epub_close(e);
        }
    }
    // Malformed containers/metadata must fail cleanly.
    const char *badfiles[] = { "bad_opf.epub", "no_spine.epub", "truncated.epub", "random.epub",
                               "bigopf.epub" };
    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, badfiles[i]);
        assert(epub_open(p) == NULL);
    }
    // Missing/oversized/corrupt entries must yield a message page, not a crash.
    const char *broken[] = { "missing.epub", "huge.epub", "hugec.epub", "corrupt.epub", "bigchap.epub" };
    for (int i = 0; i < 5; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, broken[i]);
        e = epub_open(p);
        assert(e);
        assert(epub_load_chapter(e, 0) > 0);
        assert(epub_page(e, 0) != NULL);
        epub_close(e);
    }
    // Header probes on every truncated prefix must stay in bounds.
    size_t bn = 0;
    snprintf(jp, sizeof jp, "%s/img1.jpg", dir);
    char *jb = slurp(jp, &bn);
    for (size_t i = 0; i <= bn; i++) {
        int w2 = 0, h2 = 0, g2 = 0, t2 = 0;
        epub_image_probe((const uint8_t *)jb, i, &w2, &h2, &g2, &t2);
    }
    free(jb);
    snprintf(jp, sizeof jp, "%s/img2.png", dir);
    char *pb = slurp(jp, &bn);
    for (size_t i = 0; i <= bn; i += 2) {
        uint16_t *pp = NULL;
        int pw = 0, ph = 0;
        epub_png_decode((const uint8_t *)pb, i, 4, &pp, &pw, &ph);
        free(pp);
        int w2 = 0, h2 = 0, g2 = 0, t2 = 0;
        epub_image_probe((const uint8_t *)pb, i, &w2, &h2, &g2, &t2);
    }
    free(pb);
    // Deterministic mutations of a valid EPUB; every open either fails or yields
    // a structure we can walk without OOB reads or leaks.
    size_t gn = 0;
    snprintf(jp, sizeof jp, "%s/good.epub", dir);
    char *gb = slurp(jp, &gn);
    unsigned rng = 0x12345678u;
    for (int it = 0; it < 120; it++) {
        char *m = malloc(gn);
        assert(m);
        memcpy(m, gb, gn);
        rng = rng * 1103515245u + 12345u;
        int flips = 1 + (int)(rng % 8);
        for (int f = 0; f < flips; f++) {
            rng = rng * 1103515245u + 12345u;
            size_t off = (size_t)(rng % gn);
            m[off] ^= (uint8_t)(1u << (rng % 8));
        }
        snprintf(p, sizeof p, "/tmp/epub_mut.epub");
        FILE *mf = fopen(p, "wb");
        assert(mf);
        assert(fwrite(m, 1, gn, mf) == gn);
        fclose(mf);
        free(m);
        e = epub_open(p);
        if (e) {
            int nch2 = epub_chapters(e);
            for (int c = 0; c < nch2 && c < 2; c++) {
                if (epub_load_chapter(e, c) > 0) {
                    (void)epub_page(e, 0);
                    (void)epub_text(e);
                    (void)epub_line(e, 0);
                }
            }
            epub_close(e);
        }
    }
    free(gb);

    snprintf(p, sizeof p, "%s/bad.epub", dir);
    assert(epub_open(p) == NULL);
    assert(epub_open("/tmp/definitely-missing.epub") == NULL);
    puts("epub test: OK");
    return 0;
}
