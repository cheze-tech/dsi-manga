// Host test: runs the real draw_view()/jout() code on synthetic pages.
//   gcc -fsanitize=address,undefined -Itools/stubs -Isource -o render_test tools/render_test.c source/pdf.c tools/stubs/stubs.c
//   (add -DJD_FORMAT=0 to test the RGB888 decoder path)   ./render_test  -> /tmp/r_view*.raw
#define main enhanced_main
#include "../source/enhanced.c"
#undef main
static void dump(const char *name) { FILE *f = fopen(name, "wb"); fwrite(scr[0], 2, 256 * 256, f); fwrite(scr[1], 2, 256 * 256, f); fclose(f); }
static Img mk(int w, int h, int gray, int tag) {   // light page, border, an "F", and a tag bar whose length = page number
    Img im = { malloc((size_t)w * h * (gray ? 1 : 2)), w, h, gray };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int v = 50, fx = x * 100 / w, fy = y * 100 / h;
        if (fx < 6 || fx > 94 || fy < 3 || fy > 97) v = 5;
        if (fx > 15 && fx < 30 && fy > 15 && fy < 85) v = 5;
        if (fy > 15 && fy < 25 && fx > 15 && fx < 70) v = 5;
        if (fy > 45 && fy < 55 && fx > 15 && fx < 55) v = 5;
        if (fy > 88 && fy < 94 && fx < 12 + tag * 20) v = 5;
        if (gray) ((u8 *)im.px)[y * w + x] = v; else ((u16 *)im.px)[y * w + x] = ((v >> 1) << 11) | (v << 5) | (v >> 1);
    }
    return im;
}
static int check_jout(void) {   // decoder output -> cache pixel must agree for gray and color
    Dec d; Img im = { calloc(16 * 16, 2), 16, 16, 0 }; JDEC jd; JRECT r = { 0, 15, 0, 15 };
    d.img = &im; d.abortable = 0; jd.device = &d;
    int bad = 0;
#if JD_FORMAT == 0
    uint8_t buf[16 * 16 * 3]; for (int i = 0; i < 256; i++) { buf[i*3] = 200; buf[i*3+1] = 100; buf[i*3+2] = 40; }
#else
    u16 buf[16 * 16]; for (int i = 0; i < 256; i++) buf[i] = ((200 >> 3) << 11) | ((100 >> 2) << 5) | (40 >> 3);
#endif
    jout(&jd, buf, &r);
    u16 want = ((200 >> 3) << 11) | ((100 >> 2) << 5) | (40 >> 3);
    for (int i = 0; i < 256; i++) bad += ((u16 *)im.px)[i] != want;
    im.gray = 1; free(im.px); im.px = calloc(256, 1); jout(&jd, buf, &r);
    for (int i = 0; i < 256; i++) bad += ((u8 *)im.px)[i] != 25;      // green 100 -> 6-bit 25
    free(im.px);
    return bad;
}
int main(void) {
    tables_init();
    scr[0] = calloc(256 * 256, 2); scr[1] = calloc(256 * 256, 2);
    printf("jout (JD_FORMAT=%d) mismatches: %d\n", JD_FORMAT, check_jout());
    for (int i = 0; i < NSLOT; i++) cache[i].page = -1;
    cache[0].page = 0; cache[0].im = mk(800, 1200, 1, 1);          // gray page 1
    cache[1].page = 1; cache[1].im = mk(800, 1200, 0, 2);          // colour page 2
    cache[2].page = 2; cache[2].im = mk(1600, 1100, 1, 3);         // wide page (spread)
    u8 bad[4] = { 0 };
    for (int gap = 0; gap <= 40; gap += 40) for (int view = 0; view < V_COUNT; view++) {
        cfg.gap = gap; int ox = 0, oy = 0, pg = (view == V_SPREAD_L || view == V_SPREAD_R) ? 2 : 0;
        draw_view(view, pg, 3, 1, 1, &ox, &oy, bad);
        char n[64]; snprintf(n, sizeof n, "/tmp/r_view%d_gap%d.raw", view, gap); dump(n);
    }
    cfg.gap = 40; { int ox = 1 << 20, oy = 0; draw_view(V_PORTRAIT, 0, 3, 1, 3, &ox, &oy, bad); dump("/tmp/r_zoom.raw"); }   // 3x zoom, top-right start
    puts("done");
    return 0;
}
