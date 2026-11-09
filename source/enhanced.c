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
//   Zoomed: D-pad or touch-drag pans.  The green line at the bottom is progress.
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
#define NSLOT 5             // max pages kept decoded: prev2, prev, current, next, next2
#define INVMAX 64           // reciprocal table for fast box averaging
enum { V_PORTRAIT, V_SPREAD_L, V_DUAL_L, V_SPREAD_R, V_DUAL_R, V_COUNT };   // _L: top screen on the left, _R: on the right

typedef struct { void *px; int w, h, gray; } Img;      // gray: 1 byte (0..63) per pixel, else RGB565
typedef struct { FILE *f; uint32_t left; Img *img; int abortable; int tick; } Dec;
typedef struct { int page, want; Img im; size_t bytes; } Slot;
typedef struct { char *name; long last; int page, npages, kind; u16 *thumb; } Book;   // kind 0 = PDF, 1 = text

static struct { int quality, gap, view, rtl; } cfg = { 1, 40, 0, 1 };   // gap = pixels hidden by the hinge
static u16 *scr[2];         // top, bottom framebuffers (256 px stride)
static u16 *g_buf; static int g_bufw;   // draw into a memory buffer instead (thumbnails)
static int g_rot;           // 0 portrait, 1 book (top screen on left), 2 book (top screen on right)
static u16 g_sink;          // pixels that fall under the hinge are written here
static size_t g_slot;       // RAM budget per cached page
static size_t g_ram;        // biggest heap block we could get at startup
static int g_nslot = 3;     // cache slots used by the current book (3 or 5)
static int g_maxslot = 3;
static int g_ramkb;
static int g_want;          // decoded width we want for the current zoom
static size_t g_cachebytes, g_cachepeak, g_lastpeak;
static int g_lastpages;
static u16 invtab[INVMAX + 1];   // 16.16 reciprocal for 1..INVMAX
static u8 g_pool[8192];          // TJpgDec work pool (one decode runs at a time)
static u32 g_pend;          // key presses seen while a background decode was running
static Slot cache[NSLOT];
static Book bk[MAXBOOKS];   // the library
static int nbook;

// Split the heap between page slots, keeping a reserve for the library, PDF tables,
// audio and the stack. A single cached page never gets more than 1.5 MB.
static size_t slot_budget(size_t ram, int nslot) {
    size_t reserve = ram < (6u << 20) ? (2u << 20) : (1536u << 10);
    if (ram <= reserve + (128u << 10)) return 128u << 10;
    size_t b = (ram - reserve) / nslot;
    if (b > (1536u << 10)) b = 1536u << 10;
    return b;
}

static void tables_init(void) {
    invtab[0] = 0;
    for (int i = 1; i <= INVMAX; i++) invtab[i] = (u16)((65536 + i - 1) / i);
}

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
    if ((++d->tick & 63) == 0) {             // not on every block: keeps long decodes cheap
        audio_pump(1);
        if (d->abortable) {                  // background decode: give way to the buttons
            scanKeys();
            g_pend |= keysDown();
            if (g_pend || keysHeld()) return 0;
        }
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
    Dec d = { pdf->f, pdf->pages[pg].len, im, abortable, 0 };
    fseek(pdf->f, pdf->pages[pg].off, SEEK_SET);
    JDEC jd;
    int rc = -1;
    if (jd_prepare(&jd, jin, g_pool, sizeof g_pool, &d) == JDR_OK) {
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
    return rc;
}

// ---- page cache ------------------------------------------------------------

static Slot *find_slot(int pg) {
    for (int i = 0; i < g_nslot; i++) if (cache[i].page == pg) return &cache[i];
    return NULL;
}
static Img *cached(int pg) {
    Slot *s = pg >= 0 ? find_slot(pg) : NULL;
    return s && s->im.px ? &s->im : NULL;
}
static void slot_drop(Slot *s) {
    if (s->im.px) {
        free(s->im.px);
        g_cachebytes -= s->bytes;
        memset(&s->im, 0, sizeof s->im);
    }
    s->page = -1; s->want = 0; s->bytes = 0;
}
static void trim(int lo, int hi) {           // drop everything outside [lo,hi]
    for (int i = 0; i < g_nslot; i++)
        if (cache[i].page < lo || cache[i].page > hi) slot_drop(&cache[i]);
}
// slot for pg: reuse its own old entry, else a free one, else the page farthest away
static Slot *slot_for(int pg) {
    Slot *freeS = NULL, *far = NULL;
    int fd = -1;
    for (int i = 0; i < g_nslot; i++) {
        Slot *s = &cache[i];
        if (s->page == pg) return s;
        if (!s->im.px) { if (!freeS) freeS = s; continue; }
        int d = s->page > pg ? s->page - pg : pg - s->page;
        if (d > fd) { fd = d; far = s; }
    }
    Slot *s = freeS ? freeS : far;
    if (s) slot_drop(s);
    return s;
}
static Img *load(Pdf *pdf, int pg, u8 *bad, int abortable) {
    int want = g_want / (cfg.quality ? 1 : 2);   // normal quality: one JPEG scale step down
    Slot *s = find_slot(pg);
    if (s && s->im.px && s->want >= want) return &s->im;
    s = slot_for(pg);
    if (!s) return NULL;
    slot_drop(s);                                // same page at too low a detail level: replace it
    int rc = decode(pdf, pg, &s->im, abortable, want);
    if (rc) { slot_drop(s); if (rc == -1) bad[pg] = 1; return NULL; }
    s->page = pg;
    s->want = want;
    s->bytes = (size_t)s->im.w * s->im.h * (s->im.gray ? 1 : 2);
    g_cachebytes += s->bytes;
    if (g_cachebytes > g_cachepeak) g_cachepeak = g_cachebytes;
    return &s->im;
}

// ---- drawing ---------------------------------------------------------------
// A page is "virtually" vw wide (fit-width * zoom); a W x H window at (ox,oy)
// of it is drawn at viewer position (RU,RV). Each pixel is a box average.

// RGB565 (decoder) -> RGB555 (screen)
static inline u16 c565(u16 s) {
    return (u16)((s >> 11) | (((s >> 6) & 31) << 5) | ((s & 31) << 10) | BIT(15));
}

static inline int recip(int n) {
    return n <= INVMAX ? invtab[n] : 65536 / n;
}

// Box average of [x0,x1) x [y0,y1); n = (x1-x0)*(y1-y0) is passed in because
// the caller already knows it. The 1:1 path and fixed-point reciprocal replace
// the per-pixel divisions, which were the slowest part of drawing.
static inline u16 box_px(const Img *im, int x0, int x1, int y0, int y1, int n) {
    if (n == 1) {
        if (im->gray) {
            int v = ((const u8 *)im->px)[y0 * im->w + x0] >> 1;
            return RGB15(v, v, v) | BIT(15);
        }
        return c565(((const u16 *)im->px)[y0 * im->w + x0]);
    }
    int inv = recip(n);
    if (im->gray) {
        int sum = 0;
        const u8 *p = (const u8 *)im->px + y0 * im->w;
        for (int y = y0; y < y1; y++, p += im->w)
            for (int x = x0; x < x1; x++) sum += p[x];
        int v = (int)(((u32)sum * (u32)inv) >> 17);       // /n, then /2 for 5-bit gray
        return RGB15(v, v, v) | BIT(15);
    }
    int r = 0, g = 0, b = 0;
    const u16 *p = (const u16 *)im->px + y0 * im->w;
    for (int y = y0; y < y1; y++, p += im->w)
        for (int x = x0; x < x1; x++) { u16 c = p[x]; r += c >> 11; g += (c >> 5) & 63; b += c & 31; }
    r = (int)(((u32)r * (u32)inv) >> 16);
    g = (int)(((u32)g * (u32)inv) >> 17);
    b = (int)(((u32)b * (u32)inv) >> 16);
    return RGB15(r, g, b) | BIT(15);
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
    int vw, vh, xa[512], xb[512], nx[512];
    geom(im, W, H, f, &vw, &vh);
    for (int u = 0; u < W; u++) {
        int vx = ox + u;
        if (vx < 0 || vx >= vw) { xa[u] = -1; continue; }
        int a = vx * im->w / vw;
        int b = (vx + 1) * im->w / vw;
        if (b <= a) b = a + 1;
        xa[u] = a; xb[u] = b; nx[u] = b - a;
    }
    int rot = g_buf ? 0 : g_rot;
    static u16 *dstcol[512];                  // book views: one screen row per viewer column
    if (rot) for (int i = 0; i < W; i++) {
        int t = RU + i;
        if (t >= 192 && t < 192 + cfg.gap) { dstcol[i] = NULL; continue; }
        if (t >= 192 + cfg.gap) t -= cfg.gap;
        int dy = rot == 1 ? t : 383 - t;
        dstcol[i] = scr[dy >= 192] + (dy % 192) * 256;
    }
    for (int v = 0; v < H; v++) {
        int vy = oy + v;
        u16 *row = NULL;
        if (g_buf) row = g_buf + (RV + v) * g_bufw + RU;
        else if (!rot) {
            int t = RV + v;
            if (t >= 192 && t < 192 + cfg.gap) continue;
            if (t >= 192 + cfg.gap) t -= cfg.gap;
            row = scr[t >= 192] + (t % 192) * 256 + RU;
        }
        int y0 = 0, y1 = 0, ny = 0, blank = vy < 0 || vy >= vh;
        if (!blank) {
            y0 = (int)((long long)vy * im->h / vh);
            y1 = (int)((long long)(vy + 1) * im->h / vh);
            if (y1 <= y0) y1 = y0 + 1;
            ny = y1 - y0;
        }
        for (int u = 0; u < W; u++) {
            u16 *dst;
            if (row) dst = row + u;            // portrait: contiguous row write
            else { if (!dstcol[u]) continue; dst = dstcol[u] + (rot == 1 ? 255 - (RV + v) : RV + v); }
            if (blank || xa[u] < 0) { *dst = BIT(15); continue; }
            *dst = box_px(im, xa[u], xb[u], y0, y1, nx[u] * ny);
        }
    }
}

// Green progress line along the bottom edge of the lower screen.
static void progress_line(int page, int n) {
    if (!scr[1] || n <= 0 || page < 0) return;
    int w = (int)((long long)(page + 1) * 256 / n);
    if (w < 1) w = 1;
    if (w > 256) w = 256;
    u16 bg = RGB15(2, 2, 2) | BIT(15), fg = RGB15(3, 28, 5) | BIT(15);
    for (int y = 189; y < 192; y++) {
        u16 *row = scr[1] + y * 256;
        for (int x = 0; x < 256; x++) row[x] = x < w ? fg : bg;
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
    iprintf("\nRAM: %dMB heap, %dKB/page\n", g_ramkb / 1024, (int)(g_slot >> 10));
    if (g_maxslot > 4) iprintf("Cache: %d slots (5 if <5MB)\n", g_nslot);
    else iprintf("Cache: %d slots\n", g_nslot);
    if (g_lastpeak) iprintf("Last read: %d pages, %dKB peak\n", g_lastpages, (int)(g_lastpeak >> 10));
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
    // Small books: current + 2 back + 2 ahead in RAM. Large books: current + 1 each way.
    g_nslot = pdf.size < (5u << 20) ? g_maxslot : (g_maxslot >= 5 ? 3 : g_maxslot);
    g_slot = slot_budget(g_ram, g_nslot);
    g_cachebytes = g_cachepeak = 0;
    g_lastpeak = 0;
    for (int i = 0; i < NSLOT; i++) { cache[i].page = -1; cache[i].im.px = NULL; cache[i].want = 0; cache[i].bytes = 0; }
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
        g_want = 384 * f;                    // widest view; zoom adds detail on demand
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

        int span = g_nslot >= 5 ? 2 : 1;
        if (dirty) {
            dirty = 0;
            trim(page - span, page + span);
            if (!cached(page)) busy();
            load(&pdf, page, bad, 0);
            if (dual && page + 1 < n) load(&pdf, page + 1, bad, 0);
            draw_view(view, page, n, rtl, f, &ox, &oy, bad);
            progress_line(page, n);
        } else if (!hd && !dn) {                               // idle: fill the cache
            int cand[4];
            int nc = 0, hi = page + span + (dual ? 1 : 0);
            cand[nc++] = page + (dual ? 2 : 1);
            cand[nc++] = page + (dual ? 3 : 2);
            cand[nc++] = page - 1;
            cand[nc++] = page - 2;
            for (int i = 0; i < nc; i++) {
                int c = cand[i];
                if (c >= page - span && c <= hi && c >= 0 && c < n &&
                    !bad[c] && !cached(c)) { load(&pdf, c, bad, 1); break; }
            }
        }
    }
    cfg.view = view; cfg.rtl = rtl;
    b->page = page;
    g_lastpeak = g_cachepeak;
    g_lastpages = 0;
    for (int i = 0; i < g_nslot; i++) if (cache[i].im.px) g_lastpages++;
    trim(1 << 30, -1);
    g_cachebytes = g_cachepeak = 0;
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
    g_ram = ram;
    g_ramkb = (int)(ram >> 10);
    g_maxslot = ram > (2304u << 10) ? 5 : 3;
    g_nslot = g_maxslot;
    g_slot = slot_budget(ram, g_maxslot);
    tables_init();
    audio_init();

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
