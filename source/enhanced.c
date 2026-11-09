// DSi Enhanced PDF/manga reader: uses the DSi's 16 MB RAM.
//
// Library (top = cover grid, bottom = details)
//   D-pad move   A resume   B open from page 1   X quality   Y book hold   START quit
// Reader
//   A next   B prev   D-pad left/right = turn (reading direction)
//   R / L = rotate view forward / back   Y flip reading direction   X zoom (1x..4x)
//   SELECT = next song   START library        (.txt files: A/R/L/B/D-pad turn pages)
//   Views: PORTRAIT (one page over both screens) -> SPREAD (one wide page over both
//   screens, console held sideways) -> DUAL (two pages, one per screen, held sideways)
//   Zoomed: D-pad or touch-drag pans.
#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "pdf.h"
#include "audio.h"
#include "tjpgd.h"          // TJpgDec R0.03: JD_USE_SCALE 1, JD_FORMAT 0 or 1
#if !JD_USE_SCALE
#error "tjpgdcnf.h: set JD_USE_SCALE 1"
#endif
#if JD_FORMAT > 1
#error "tjpgdcnf.h: set JD_FORMAT to 1 (or 0), not 2"
#endif

#define TW 60               // cover thumbnail size
#define TH 88
#define MAXBOOKS 128
#define NSLOT 4             // pages kept decoded: prev + current + next (+1 in dual view)
enum { V_PORTRAIT, V_SPREAD_L, V_DUAL_L, V_SPREAD_R, V_DUAL_R, V_COUNT };   // _L: top screen on the left, _R: on the right

typedef struct { void *px; int w, h, gray; } Img;      // gray: 1 byte (0..63) per pixel, else RGB565
typedef struct { FILE *f; uint32_t left; Img *img; int abortable; } Dec;
typedef struct { int page; Img im; } Slot;
typedef struct { char *name; long last; int page, npages, kind; u16 *thumb; } Book;   // kind 0 = PDF, 1 = text

static struct { int quality, gap, view, rtl; } cfg = { 1, 40, 0, 1 };   // gap = pixels hidden by the hinge
static u16 *scr[2];         // top, bottom framebuffers (256 px stride)
static u16 *g_buf; static int g_bufw;   // draw into a memory buffer instead (thumbnails)
static int g_rot;           // 0 portrait, 1 book (top screen on left), 2 book (top screen on right)
static u16 g_sink;          // pixels that fall under the hinge are written here
static size_t g_slot;       // RAM budget per cached page
static int g_target;        // preferred decoded width
static int g_ramkb;
static u32 g_pend;          // key presses seen while a background decode was running
static Slot cache[NSLOT];
static Book bk[MAXBOOKS];   // the library
static int nbook;

// ---- video -----------------------------------------------------------------

static void gfx_reader(void) {
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetPrimaryBanks(VRAM_A_MAIN_BG, VRAM_B_LCD, VRAM_C_SUB_BG, VRAM_D_LCD);
    scr[0] = bgGetGfxPtr(bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0));
    scr[1] = bgGetGfxPtr(bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0));
}

static void gfx_library(void) {          // top = bitmap grid, bottom = text console
    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    scr[0] = bgGetGfxPtr(bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0));
    consoleDemoInit();
}

static void wait_key(void) { do { swiWaitForVBlank(); scanKeys(); audio_pump(4); } while (!keysDown()); }

// viewer coordinates (u right, v down) -> pixel. Portrait: 256 x (384+gap). Book: (384+gap) x 256.
// The `gap` rows/columns between the two screens are skipped, so the picture stays in
// proportion across the hinge (what the hinge hides is simply not drawn).
static inline u16 *scrpix(int u, int v) {
    if (g_buf) return g_buf + v * g_bufw + u;
    int t = g_rot == 0 ? v : u;                  // position along the screen stack
    int c = g_rot == 0 ? u : v;                  // position across it
    if (t >= 192) { if (t < 192 + cfg.gap) return &g_sink; t -= cfg.gap; }
    int sx, dy;
    if (g_rot == 0)      { sx = c;       dy = t; }
    else if (g_rot == 1) { sx = 255 - c; dy = t; }           // console turned counter-clockwise
    else                 { sx = c;       dy = 383 - t; }     // console turned clockwise
    return scr[dy >= 192] + (dy % 192) * 256 + sx;
}

// ---- JPEG decode -----------------------------------------------------------

static size_t jin(JDEC *jd, uint8_t *buf, size_t n) {
    Dec *d = jd->device;
    if (n > d->left) n = d->left;
    if (buf) n = fread(buf, 1, n, d->f); else fseek(d->f, n, SEEK_CUR);
    d->left -= n;
    return n;
}

static int jout(JDEC *jd, void *bm, JRECT *r) {
    Dec *d = jd->device;
    audio_pump(1);                           // keep the music going during long decodes
    if (d->abortable) {                      // background decode: give way to the buttons
        scanKeys();
        g_pend |= keysDown();
        if (g_pend || keysHeld()) return 0;
    }
    Img *im = d->img;
    int w = r->right - r->left + 1;
    int n = im->w - r->left; if (n > w) n = w;
    if (n <= 0) return 1;
    for (int y = r->top; y <= r->bottom; y++) {
        if (y >= im->h) continue;
        int row = y - r->top;
#if JD_FORMAT == 0                           // RGB888 from the decoder
        const uint8_t *s = (const uint8_t *)bm + row * w * 3;
        if (im->gray) {
            u8 *o = (u8 *)im->px + y * im->w + r->left;
            for (int x = 0; x < n; x++) o[x] = s[x * 3 + 1] >> 2;
        } else {
            u16 *o = (u16 *)im->px + y * im->w + r->left;
            for (int x = 0; x < n; x++, s += 3) o[x] = ((s[0] & 0xF8) << 8) | ((s[1] & 0xFC) << 3) | (s[2] >> 3);
        }
#else                                        // RGB565 from the decoder
        const u16 *s = (const u16 *)bm + row * w;
        if (im->gray) {
            u8 *o = (u8 *)im->px + y * im->w + r->left;
            for (int x = 0; x < n; x++) o[x] = (s[x] >> 5) & 63;        // green channel = 6-bit gray
        } else memcpy((u16 *)im->px + y * im->w + r->left, s, n * 2);
#endif
    }
    return 1;
}

// 0 ok, -1 failed, -2 interrupted. Gray JPEGs use 1 byte/pixel, so they decode
// at twice the resolution for the same RAM.
static int decode(Pdf *pdf, int pg, Img *im, int abortable, int target) {
    memset(im, 0, sizeof *im);
    Dec d = { pdf->f, pdf->pages[pg].len, im, abortable };
    fseek(pdf->f, pdf->pages[pg].off, SEEK_SET);
    JDEC jd;
    void *pool = malloc(8192);
    int rc = -1;
    if (pool && jd_prepare(&jd, jin, pool, 8192, &d) == JDR_OK) {
        im->gray = jd.ncomp == 1;
        size_t bpp = im->gray ? 1 : 2;
        int s = 0;
        while (s < 3 && (jd.width >> (s + 1)) >= target) s++;
        for (;; s++) {                       // shrink further until it fits the slot budget
            im->w = (jd.width + (1 << s) - 1) >> s;
            im->h = (jd.height + (1 << s) - 1) >> s;
            size_t need = (size_t)im->w * im->h * bpp;
            im->px = (need <= g_slot || s >= 3) ? malloc(need) : NULL;
            if (im->px || s >= 3) break;
        }
        if (im->px) {
            JRESULT r = jd_decomp(&jd, jout, s);
            if (r == JDR_OK) rc = 0;
            else { free(im->px); im->px = NULL; if (r == JDR_INTR) rc = -2; }
        }
    }
    free(pool);
    return rc;
}

// ---- page cache ------------------------------------------------------------

static Img *cached(int pg) {
    for (int i = 0; i < NSLOT; i++) if (cache[i].page == pg && cache[i].im.px) return &cache[i].im;
    return NULL;
}
static void trim(int lo, int hi) {           // drop everything outside [lo,hi]
    for (int i = 0; i < NSLOT; i++)
        if (cache[i].page < lo || cache[i].page > hi) { free(cache[i].im.px); cache[i].im.px = NULL; cache[i].page = -1; }
}
static Img *load(Pdf *pdf, int pg, u8 *bad, int abortable) {
    Img *c = cached(pg);
    if (c) return c;
    Slot *s = NULL;
    for (int i = 0; i < NSLOT && !s; i++) if (!cache[i].im.px) s = &cache[i];
    if (!s) return NULL;
    int rc = decode(pdf, pg, &s->im, abortable, g_target);
    if (rc) { s->page = -1; if (rc == -1) bad[pg] = 1; return NULL; }
    s->page = pg;
    return &s->im;
}

// ---- drawing ---------------------------------------------------------------
// A page is "virtually" vw wide (fit-width * zoom); a W x H window at (ox,oy)
// of it is drawn at viewer position (RU,RV). Each pixel is a box average.

static u16 sample(const Img *im, int x0, int x1, int y0, int y1) {
    int n = (x1 - x0) * (y1 - y0), sum = 0;
    if (im->gray) {
        for (int y = y0; y < y1; y++) {
            const u8 *p = (const u8 *)im->px + y * im->w;
            for (int x = x0; x < x1; x++) sum += p[x];
        }
        int v = (sum / n) >> 1;
        return RGB15(v, v, v) | BIT(15);
    }
    int r = 0, g = 0, b = 0;
    for (int y = y0; y < y1; y++) {
        const u16 *p = (const u16 *)im->px + y * im->w;
        for (int x = x0; x < x1; x++) { u16 c = p[x]; r += c >> 11; g += (c >> 5) & 63; b += c & 31; }
    }
    return RGB15(r / n, (g / n) >> 1, b / n) | BIT(15);
}

static void geom(const Img *im, int W, int H, int f, int *vw, int *vh) {
    int fw = (im->w * H >= W * im->h) ? W : im->w * H / im->h;
    *vw = fw * f;
    *vh = (int)((long long)im->h * *vw / im->w);
}

static void clamp_pan(const Img *im, int W, int H, int f, int *ox, int *oy) {
    int vw, vh;
    geom(im, W, H, f, &vw, &vh);
    *ox = vw > W ? (*ox < 0 ? 0 : *ox > vw - W ? vw - W : *ox) : -(W - vw) / 2;
    *oy = vh > H ? (*oy < 0 ? 0 : *oy > vh - H ? vh - H : *oy) : -(H - vh) / 2;
}

static void blank(int RU, int RV, int W, int H, u16 c) {
    for (int v = 0; v < H; v++) for (int u = 0; u < W; u++) *scrpix(RU + u, RV + v) = c;
}

static void blit(const Img *im, int RU, int RV, int W, int H, int f, int ox, int oy) {
    int vw, vh, xa[512], xb[512];
    geom(im, W, H, f, &vw, &vh);
    for (int u = 0; u < W; u++) {
        int vx = ox + u;
        if (vx < 0 || vx >= vw) { xa[u] = -1; continue; }
        xa[u] = vx * im->w / vw;
        xb[u] = (vx + 1) * im->w / vw;
        if (xb[u] <= xa[u]) xb[u] = xa[u] + 1;
    }
    for (int v = 0; v < H; v++) {
        audio_pump(1);
        int vy = oy + v;
        if (vy < 0 || vy >= vh) { for (int u = 0; u < W; u++) *scrpix(RU + u, RV + v) = BIT(15); continue; }
        int y0 = (int)((long long)vy * im->h / vh), y1 = (int)((long long)(vy + 1) * im->h / vh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int u = 0; u < W; u++)
            *scrpix(RU + u, RV + v) = xa[u] < 0 ? BIT(15) : sample(im, xa[u], xb[u], y0, y1);
    }
}

static void show(int pg, int RU, int RV, int W, int H, int f, int *ox, int *oy, const u8 *bad) {
    Img *im = pg >= 0 ? cached(pg) : NULL;
    if (!im) { blank(RU, RV, W, H, (pg >= 0 && bad[pg]) ? (RGB15(20, 0, 0) | BIT(15)) : BIT(15)); return; }
    clamp_pan(im, W, H, f, ox, oy);
    blit(im, RU, RV, W, H, f, *ox, *oy);
}

static void busy(void) {                      // little red square = decoding
    for (int y = 0; y < 6; y++) for (int x = 250; x < 256; x++) scr[0][y * 256 + x] = RGB15(31, 0, 0) | BIT(15);
}

// d-pad keys as the viewer sees them for the current orientation
static void vkeys(u32 *l, u32 *r, u32 *u, u32 *d) {
    if (g_rot == 0)      { *l = KEY_LEFT; *r = KEY_RIGHT; *u = KEY_UP;   *d = KEY_DOWN; }
    else if (g_rot == 1) { *l = KEY_UP;   *r = KEY_DOWN;  *u = KEY_RIGHT; *d = KEY_LEFT; }
    else                 { *l = KEY_DOWN; *r = KEY_UP;    *u = KEY_LEFT;  *d = KEY_RIGHT; }
}

static int rot_of(int view) { return view == V_PORTRAIT ? 0 : view <= V_DUAL_L ? 1 : 2; }
static int is_dual(int view) { return view == V_DUAL_L || view == V_DUAL_R; }

// Draw whatever the current view needs. Sets the rotation itself, so it can never be stale.
static void draw_view(int view, int page, int n, int rtl, int f, int *ox, int *oy, const u8 *bad) {
    g_rot = rot_of(view);
    if (is_dual(view)) {                                  // one page per screen
        int a = page, c = page + 1 < n ? page + 1 : -1, d0 = 0, d1 = 0;
        show(rtl ? c : a, 0, 0, 192, 256, 1, &d0, &d1, bad);
        d0 = d1 = 0;
        show(rtl ? a : c, 192 + cfg.gap, 0, 192, 256, 1, &d0, &d1, bad);
    } else if (g_rot) show(page, 0, 0, 384 + cfg.gap, 256, f, ox, oy, bad);   // spread across both screens
    else show(page, 0, 0, 256, 384 + cfg.gap, f, ox, oy, bad);                // portrait
}

// ---- library file / covers -------------------------------------------------

static char libpath[320];

static void lib_load(void) {
    FILE *f = fopen(libpath, "r");
    char line[400];
    while (f && fgets(line, sizeof line, f)) {
        if (!strncmp(line, "cfg2 ", 5)) {
            sscanf(line + 5, "%d %d %d %d", &cfg.quality, &cfg.gap, &cfg.view, &cfg.rtl);
            if (cfg.gap < 0 || cfg.gap > 96) cfg.gap = 40;
            if (cfg.view < 0 || cfg.view >= V_COUNT) cfg.view = 0;
            continue;
        }
        long last; int page, np, off = 0;
        if (sscanf(line, "%ld %d %d %n", &last, &page, &np, &off) >= 3 && off) {
            char *nm = line + off;
            nm[strcspn(nm, "\r\n")] = 0;
            for (int i = 0; i < nbook; i++)
                if (!strcmp(bk[i].name, nm)) { bk[i].last = last; bk[i].page = page; bk[i].npages = np; }
        }
    }
    if (f) fclose(f);
}

static void lib_save(void) {
    FILE *f = fopen(libpath, "w");
    if (!f) return;
    fprintf(f, "cfg2 %d %d %d %d\n", cfg.quality, cfg.gap, cfg.view, cfg.rtl);
    for (int i = 0; i < nbook; i++)
        if (bk[i].last > 0) fprintf(f, "%ld %d %d %s\n", bk[i].last, bk[i].page, bk[i].npages, bk[i].name);
    fclose(f);
}

static int thumb_load(Book *b, const char *path) {
    char p[340];
    snprintf(p, sizeof p, "%s.thm", path);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    int np = 0;
    u16 *t = malloc(TW * TH * 2);
    int ok = t && fread(&np, 4, 1, f) == 1 && fread(t, 2, TW * TH, f) == TW * TH;
    fclose(f);
    if (!ok) { free(t); return 0; }
    b->thumb = t;
    if (np > 0) b->npages = np;
    return 1;
}

static void thumb_make(Book *b, const char *path) {
    Pdf pdf;
    u16 *t = malloc(TW * TH * 2);
    if (!t) return;
    for (int i = 0; i < TW * TH; i++) t[i] = BIT(15);
    b->thumb = t;
    if (pdf_open(&pdf, path) || !pdf.npages) { pdf_close(&pdf); return; }
    b->npages = pdf.npages;
    Img im;
    if (decode(&pdf, 0, &im, 0, 100) == 0) {
        int ox = 0, oy = 0;
        clamp_pan(&im, TW, TH, 1, &ox, &oy);
        g_buf = t; g_bufw = TW;
        blit(&im, 0, 0, TW, TH, 1, ox, oy);
        g_buf = NULL;
        free(im.px);
        char p[340];
        snprintf(p, sizeof p, "%s.thm", path);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(&pdf.npages, 4, 1, f); fwrite(t, 2, TW * TH, f); fclose(f); }
    }
    pdf_close(&pdf);
}

static void rect0(int x, int y, int w, int h, u16 c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) scr[0][(y + j) * 256 + x + i] = c;
}

static void draw_library(int sel, const char *dir) {
    int n = nbook;
    rect0(0, 0, 256, 192, BIT(15));
    int first = sel / 8 * 8;
    for (int k = 0; k < 8 && first + k < n; k++) {
        Book *b = &bk[first + k];
        int x0 = (k % 4) * 64, y0 = (k / 4) * 96;
        if (first + k == sel) rect0(x0, y0, 64, 96, RGB15(31, 27, 0) | BIT(15));
        if (b->thumb) for (int y = 0; y < TH; y++) memcpy(scr[0] + (y0 + 2 + y) * 256 + x0 + 2, b->thumb + y * TW, TW * 2);
        rect0(x0 + 2, y0 + 91, TW, 3, RGB15(6, 6, 6) | BIT(15));
        if (b->last && b->npages > 0) rect0(x0 + 2, y0 + 91, TW * (b->page + 1) / b->npages, 3, RGB15(4, 28, 8) | BIT(15));
    }
    consoleClear();
    iprintf("DSi manga reader  %s %dMB\n", isDSiMode() ? "DSi" : "DS", g_ramkb / 1024);
    if (!n) { iprintf("\nNo PDFs in %s\n", dir); return; }
    Book *b = &bk[sel];
    iprintf("\n%.31s\n", b->name);
    if (b->last > 0 && b->npages > 0) {
        long days = (long)(time(NULL) - b->last) / 86400;
        iprintf("Page %d/%d  (%d%%)\n", b->page + 1, b->npages, 100 * (b->page + 1) / b->npages);
        iprintf("Last read: %s\n", days <= 0 ? "today" : days == 1 ? "yesterday" : "a while ago");
        if (days > 1) iprintf("           %ld days ago\n", days);
    } else iprintf(b->kind ? "text file - not opened yet\n" : "%d pages - not opened yet\n", b->npages);
    iprintf("\nA resume   B from page 1\nX quality: %s\nHinge gap: %d px (change it in\nthe reader: D-pad up/down)\nSTART quit\n",
            cfg.quality ? "high" : "normal", cfg.gap);
    const char *song = audio_title();
    if (song) iprintf("\nMusic%s: %.21s\nSELECT next song   Y pause\n", audio_paused() ? " (paused)" : "", song);
}

// ---- reader ----------------------------------------------------------------

static void reader(Book *b, const char *path, int start) {
    Pdf pdf;
    int rc = pdf_open(&pdf, path);
    if (rc || !pdf.npages) {
        char d[200] = "";
        if (!rc) pdf_diag(&pdf, d, sizeof d);
        iprintf("\n%s\n%s\nReader needs JPEG-image pages.\nConvert the PDF with\ntools/prep_pdf.py\n\nPress any key.\n",
                rc ? "Can't open the file." : "No usable pages.", d);
        pdf_close(&pdf); wait_key();
        return;
    }
    int n = pdf.npages;
    u8 *bad = calloc(n, 1);
    int page = start < 0 ? 0 : start >= n ? n - 1 : start;
    int view = cfg.view, rtl = cfg.rtl, f = 1, ox = 0, oy = 0, dirty = 1, turns = 0, tx = -1, ty = -1;
    b->npages = n;
    for (int i = 0; i < NSLOT; i++) { cache[i].page = -1; cache[i].im.px = NULL; }
    gfx_reader();
    touchPosition tp;

    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        u32 dn = keysDown() | g_pend, hd = keysHeld(), kl, kr, ku, kd;
        g_pend = 0;
        if (dn & KEY_START) break;
        int vd = ((dn & KEY_R) ? 1 : 0) - ((dn & KEY_L) ? 1 : 0);          // R rotate forward, L back
        if (vd) { view = (view + V_COUNT + vd) % V_COUNT; f = 1; oy = 0; ox = rtl ? 1 << 20 : 0; dirty = 1; }
        if (dn & KEY_SELECT) audio_next();
        audio_pump(4);
        g_rot = rot_of(view);                                  // after the view may have changed
        vkeys(&kl, &kr, &ku, &kd);
        int dual = is_dual(view), step = 0, pan0 = rtl ? 1 << 20 : 0;

        if (dn & KEY_A) step = dual ? 2 : 1;
        if (dn & KEY_B) step = dual ? -2 : -1;
        if (dual || f == 1) {
            if (dn & kl) step = rtl ? 1 : -1;
            if (dn & kr) step = rtl ? -1 : 1;
            if (dn & (ku | kd)) {                              // hinge gap: up = more, down = less
                cfg.gap += (dn & ku) ? 4 : -4;
                if (cfg.gap < 0) cfg.gap = 0;
                if (cfg.gap > 96) cfg.gap = 96;
                dirty = 1;
            }
        }
        if (dn & KEY_Y) { rtl ^= 1; ox = rtl ? 1 << 20 : 0; dirty = 1; }
        if ((dn & KEY_X) && !dual) { f = f % 4 + 1; ox = pan0; oy = 0; dirty = 1; }
        if (step) {
            int np = page + step;
            if (np < 0) np = 0;
            if (np > n - 1) np = n - 1;
            if (np != page) {
                page = np; ox = pan0; oy = 0; dirty = 1;
                b->page = page;
                if (++turns % 10 == 0) { b->last = time(NULL); lib_save(); }   // survive a dead battery
            }
        }

        if (!dual && f > 1) {                                  // panning
            int dx = 0, dy = 0;
            if (hd & kl) dx -= 8;
            if (hd & kr) dx += 8;
            if (hd & ku) dy -= 8;
            if (hd & kd) dy += 8;
            if (hd & KEY_TOUCH) {
                touchRead(&tp);
                if (tx >= 0) {
                    int px = tp.px - tx, py = tp.py - ty;      // finger movement in device coords
                    int du = g_rot == 0 ? px : g_rot == 1 ? py : -py;
                    int dv = g_rot == 0 ? py : g_rot == 1 ? -px : px;
                    dx -= du; dy -= dv;
                }
                tx = tp.px; ty = tp.py;
            } else tx = -1;
            if (dx || dy) { ox += dx; oy += dy; dirty = 1; }
        } else tx = -1;

        if (dirty) {
            dirty = 0;
            trim(page - 1, page + (dual ? 2 : 1));
            if (!cached(page)) busy();
            load(&pdf, page, bad, 0);
            if (dual && page + 1 < n) load(&pdf, page + 1, bad, 0);
            draw_view(view, page, n, rtl, f, &ox, &oy, bad);
        } else if (!hd && !dn) {                               // idle: decode the next page ahead
            int cand[2] = { page + (dual ? 2 : 1), page - 1 };
            for (int i = 0; i < 2; i++)
                if (cand[i] >= 0 && cand[i] < n && !bad[cand[i]] && !cached(cand[i])) { load(&pdf, cand[i], bad, 1); break; }
        }
    }
    cfg.view = view; cfg.rtl = rtl;
    b->page = page;
    trim(1 << 30, -1);
    free(bad);
    pdf_close(&pdf);
}

// ---- text files ------------------------------------------------------------
#define COLS 31                         // 31, not 32: writing the last column of the last row would scroll
#define ROWS_TOP 24
#define ROWS_BOT 23                     // the bottom screen keeps its last row for the page counter
#define LINES_PAGE (ROWS_TOP + ROWS_BOT)

static PrintConsole con_top, con_bot;

static void gfx_text(void) {
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&con_top, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&con_bot, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
}

// UTF-8 / junk -> plain ASCII the console font can show (in place, never grows)
static size_t sanitize(char *t, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = t[i];
        if (c == '\r') continue;
        if (c == '\t') { t[o++] = ' '; continue; }
        if (c < 128) { t[o++] = (c >= 32 || c == '\n') ? c : ' '; continue; }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        unsigned cp = len == 1 ? 0 : c & (0xFFu >> (len + 1));
        int ok = len > 1 && i + len <= n;
        for (int k = 1; ok && k < len; k++) {
            unsigned char d = t[i + k];
            if ((d & 0xC0) != 0x80) ok = 0; else cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { t[o++] = '?'; continue; }
        i += len - 1;
        if (cp == 0xFEFF) continue;                                          // byte-order mark
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032) t[o++] = '\'';
        else if (cp == 0x201C || cp == 0x201D) t[o++] = '"';
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) t[o++] = '-';
        else if (cp == 0x2026) { t[o++] = '.'; t[o++] = '.'; t[o++] = '.'; }
        else if (cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200B)) t[o++] = ' ';
        else t[o++] = '?';
    }
    return o;
}

// one wrapped line starting at p; returns where the next one starts (out may be NULL)
static size_t wrap_line(const char *t, size_t n, size_t p, char *out) {
    char tmp[COLS + 1];
    int len = 0, sp = -1;
    size_t i = p, next;
    while (i < n && t[i] != '\n' && len < COLS) {
        tmp[len] = t[i];
        if (t[i] == ' ') sp = len;
        len++; i++;
    }
    if (i >= n) next = n;
    else if (t[i] == '\n' || t[i] == ' ') next = i + 1;            // ended exactly at a break
    else if (sp > 0) { len = sp; next = p + sp + 1; }              // wrap at the last space
    else next = i;                                                  // one huge word: cut it
    while (len > 0 && tmp[len - 1] == ' ') len--;
    if (out) { memcpy(out, tmp, len); out[len] = 0; }
    return next;
}

static void txt_draw(const char *t, size_t n, size_t pos, int page, int np) {
    char line[COLS + 1];
    for (int s = 0; s < 2; s++) {
        consoleSelect(s ? &con_bot : &con_top);
        consoleClear();
        int rows = s ? ROWS_BOT : ROWS_TOP;
        for (int r = 0; r < rows && pos < n; r++) { pos = wrap_line(t, n, pos, line); iprintf("\x1b[%d;0H%s", r, line); }
    }
    iprintf("\x1b[23;0HPage %d/%d  %d%%", page + 1, np, 100 * (page + 1) / np);
}

static void txt_reader(Book *b, const char *path, int start) {
    FILE *f = fopen(path, "rb");
    if (!f) { iprintf("\nCan't open the file.\nPress any key.\n"); wait_key(); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > (4L << 20)) sz = 4L << 20;                              // first 4 MB is plenty
    char *t = sz > 0 ? malloc(sz) : NULL;
    size_t n = t ? fread(t, 1, sz, f) : 0;
    fclose(f);
    if (!n) { free(t); iprintf("\nEmpty file (or no RAM).\nPress any key.\n"); wait_key(); return; }
    n = sanitize(t, n);

    size_t *st = NULL, pos = 0;                                      // start offset of every page
    int np = 0, cap = 0;
    while (pos < n) {
        if (np == cap) {
            size_t *tt = realloc(st, (cap = cap ? cap * 2 : 256) * sizeof *st);
            if (!tt) break;
            st = tt;
        }
        st[np++] = pos;
        for (int l = 0; l < LINES_PAGE && pos < n; l++) pos = wrap_line(t, n, pos, NULL);
    }
    if (!np) { free(t); free(st); return; }
    b->npages = np;
    int page = start < 0 ? 0 : start >= np ? np - 1 : start, dirty = 1, turns = 0;
    gfx_text();

    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        audio_pump(4);
        u32 dn = keysDown();
        if (dn & KEY_START) break;
        if (dn & KEY_SELECT) audio_next();
        int step = 0;
        if (dn & (KEY_A | KEY_R | KEY_RIGHT)) step = 1;
        if (dn & (KEY_B | KEY_L | KEY_LEFT)) step = -1;
        if (dn & KEY_DOWN) step = 10;
        if (dn & KEY_UP) step = -10;
        if (step) {
            int p2 = page + step;
            if (p2 < 0) p2 = 0;
            if (p2 > np - 1) p2 = np - 1;
            if (p2 != page) {
                page = p2; dirty = 1; b->page = page;
                if (++turns % 10 == 0) { b->last = time(NULL); lib_save(); }
            }
        }
        if (dirty) { dirty = 0; txt_draw(t, n, st[page], page, np); }
    }
    b->page = page;
    free(t); free(st);
}

// cover for text files: a little page of text lines (no font needed)
static void thumb_txt(Book *b) {
    u16 *t = malloc(TW * TH * 2);
    if (!t) return;
    for (int i = 0; i < TW * TH; i++) t[i] = RGB15(5, 8, 18) | BIT(15);
    for (int row = 0; row < 9; row++) {
        int y = 14 + row * 7, w = 18 + (row * 37) % 34;
        for (int j = 0; j < 2; j++) for (int x = 8; x < 8 + w && x < TW - 4; x++) t[(y + j) * TW + x] = RGB15(26, 26, 28) | BIT(15);
    }
    b->thumb = t;
}

// ---- main / library ----------------------------------------------------------

static int book_cmp(const void *a, const void *b) {
    const Book *x = a, *y = b;
    if (x->last != y->last) return x->last < y->last ? 1 : -1;        // most recent first
    return strcasecmp(x->name, y->name);
}

static int scan_dir(const char *dir, Book *bk) {
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < MAXBOOKS) {
        size_t l = strlen(e->d_name);
        int pdf = l > 4 && !strcasecmp(e->d_name + l - 4, ".pdf"), txt = l > 4 && !strcasecmp(e->d_name + l - 4, ".txt");
        if (pdf || txt) { memset(&bk[n], 0, sizeof bk[n]); bk[n].kind = txt; bk[n++].name = strdup(e->d_name); }
    }
    if (d) closedir(d);
    return n;
}

static size_t probe_ram(void) {               // how big a block can we really get?
    for (int mb = 14; mb > 0; mb--) {
        void *p = malloc((size_t)mb << 20);
        if (p) { free(p); return (size_t)mb << 20; }
    }
    return 1 << 19;
}

int main(void) {
    consoleDemoInit();
    if (!fatInitDefault()) { iprintf("SD card / FAT init failed.\n"); for (;;) swiWaitForVBlank(); }
    size_t ram = probe_ram();
    g_ramkb = (int)(ram >> 10);
    audio_init();
    g_slot = ram > (2304u << 10) ? (ram - (1536u << 10)) / NSLOT : (192u << 10);

    for (;;) {
        const char *dir = "/manga/";
        int n = nbook = scan_dir(dir, bk);
        if (!n) n = nbook = scan_dir(dir = "/", bk);
        snprintf(libpath, sizeof libpath, "%sdsimanga.lib", dir);
        lib_load();
        qsort(bk, n, sizeof *bk, book_cmp);
        gfx_library();

        int sel = 0, shown = -1;
        for (;;) {
            if (sel / 8 != shown) {                               // make sure this page's covers exist
                shown = sel / 8;
                for (int k = shown * 8; k < n && k < shown * 8 + 8; k++) {
                    if (bk[k].thumb) continue;
                    char p[320];
                    snprintf(p, sizeof p, "%s%s", dir, bk[k].name);
                    consoleClear();
                    iprintf("Building cover %d/%d\n%.31s\n", k + 1, n, bk[k].name);
                    if (bk[k].kind) thumb_txt(&bk[k]);
                    else if (!thumb_load(&bk[k], p)) thumb_make(&bk[k], p);
                }
            }
            draw_library(sel, dir);
            do { swiWaitForVBlank(); scanKeys(); audio_pump(4); } while (!keysDown());
            u32 k = keysDown();
            if (k & KEY_SELECT) audio_next();
            if (k & KEY_Y) audio_toggle();
            if (k & KEY_START) { lib_save(); return 0; }
            if ((k & KEY_RIGHT) && sel < n - 1) sel++;
            if ((k & KEY_LEFT) && sel > 0) sel--;
            if ((k & KEY_DOWN) && sel + 4 < n) sel += 4;
            if ((k & KEY_UP) && sel >= 4) sel -= 4;
            if (k & KEY_X) cfg.quality ^= 1;
            if ((k & (KEY_A | KEY_B)) && n) {
                char p[320];
                Book *b = &bk[sel];
                snprintf(p, sizeof p, "%s%s", dir, b->name);
                g_target = cfg.quality ? 1100 : 640;
                int startpage = (k & KEY_B) ? 0 : b->page;
                b->last = time(NULL);
                if (b->kind) txt_reader(b, p, startpage); else reader(b, p, startpage);
                b->last = time(NULL);
                lib_save();
                break;
            }
        }
        for (int i = 0; i < n; i++) { free(bk[i].name); free(bk[i].thumb); }
    }
}
