// DSi manga/PDF reader. Both screens are used as one tall 256x384 page.
// Controls  (page)  A / R = next   B / L = prev   D-pad L/R = turn (reading dir)
//           Y = flip reading direction (starts right-to-left)   X = zoom
//           (zoom)  D-pad or touch-drag = pan      START = back to file list
#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pdf.h"
#include "tjpgd.h"          // TJpgDec R0.03: JD_USE_SCALE 1, JD_FORMAT 0 or 1
#if !JD_USE_SCALE
#error "tjpgdcnf.h: set JD_USE_SCALE 1"
#endif
#if JD_FORMAT > 1
#error "tjpgdcnf.h: set JD_FORMAT to 1 (or 0), not 2"
#endif

typedef struct { u16 *px; int w, h; } Img;
typedef struct { FILE *f; uint32_t left; Img *img; } Dec;

static u16 *scr[2];         // [0] top, [1] bottom, 256 px stride
static int gap = 40;        // pixels hidden by the hinge (D-pad up/down when not zoomed)

static void gfx_init(void) {
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetPrimaryBanks(VRAM_A_MAIN_BG, VRAM_B_LCD, VRAM_C_SUB_BG, VRAM_D_LCD);
    scr[0] = bgGetGfxPtr(bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0));
    scr[1] = bgGetGfxPtr(bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0));
}

static void fill(u16 c) {
    for (int s = 0; s < 2; s++) for (int i = 0; i < 256 * 192; i++) scr[s][i] = c;
}

// ---- JPEG decode (TJpgDec reads straight from the PDF file) ----------------

static size_t jin(JDEC *jd, uint8_t *buf, size_t n) {
    Dec *d = jd->device;
    if (n > d->left) n = d->left;
    if (buf) n = fread(buf, 1, n, d->f); else fseek(d->f, n, SEEK_CUR);
    d->left -= n;
    return n;
}

static int jout(JDEC *jd, void *bm, JRECT *r) {
    Dec *d = jd->device;
    int w = r->right - r->left + 1;
    int n = d->img->w - r->left; if (n > w) n = w;
    if (n <= 0) return 1;
    for (int y = r->top; y <= r->bottom; y++) {
        if (y >= d->img->h) continue;
        u16 *o = d->img->px + y * d->img->w + r->left;
        int row = y - r->top;
#if JD_FORMAT == 0                           // RGB888 from the decoder
        const uint8_t *s = (const uint8_t *)bm + row * w * 3;
        for (int x = 0; x < n; x++, s += 3) o[x] = ((s[0] & 0xF8) << 8) | ((s[1] & 0xFC) << 3) | (s[2] >> 3);
#else
        memcpy(o, (const u16 *)bm + row * w, n * 2);
#endif
    }
    return 1;
}

// decode page to RGB565, shrunk by 1/2/4/8 so it is still >= want_w wide
static int decode(Pdf *pdf, int page, int want_w, Img *im) {
    free(im->px); im->px = NULL;
    Dec d = { pdf->f, pdf->pages[page].len, im };
    fseek(pdf->f, pdf->pages[page].off, SEEK_SET);
    JDEC jd;
    void *pool = malloc(8192);
    int rc = -1;
    if (pool && jd_prepare(&jd, jin, pool, 8192, &d) == JDR_OK) {
        int s = 0;
        while (s < 3 && (jd.width >> (s + 1)) >= want_w) s++;
        for (;; s++) {                                  // low on RAM? shrink more
            im->w = (jd.width + (1 << s) - 1) >> s;
            im->h = (jd.height + (1 << s) - 1) >> s;
            im->px = malloc((size_t)im->w * im->h * 2);
            if (im->px || s >= 3) break;
        }
        if (im->px && jd_decomp(&jd, jout, s) == JDR_OK) rc = 0;
        else { free(im->px); im->px = NULL; }
    }
    free(pool);
    return rc;
}

// ---- drawing ---------------------------------------------------------------
// The page is virtually vw pixels wide; the screens show a 256x384 window
// at (ox,oy) of it. Each screen pixel is a box average of source pixels.

static void render(const Img *im, int vw, int ox, int oy) {
    int WH = 384 + gap;                          // window height; `gap` rows fall under the hinge
    int vh = (int)((long long)im->h * vw / im->w);
    int xa[256], xb[256];
    for (int sx = 0; sx < 256; sx++) {
        int vx = ox + sx;
        if (vx < 0 || vx >= vw) { xa[sx] = -1; continue; }
        xa[sx] = vx * im->w / vw;
        xb[sx] = (vx + 1) * im->w / vw;
        if (xb[sx] <= xa[sx]) xb[sx] = xa[sx] + 1;
    }
    for (int sy = 0; sy < WH; sy++) {
        if (sy >= 192 && sy < 192 + gap) continue;
        int dy = sy < 192 ? sy : sy - gap;
        u16 *row = scr[dy >= 192] + (dy % 192) * 256;
        int vy = oy + sy;
        if (vy < 0 || vy >= vh) { for (int i = 0; i < 256; i++) row[i] = BIT(15); continue; }
        int y0 = vy * im->h / vh, y1 = (vy + 1) * im->h / vh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int sx = 0; sx < 256; sx++) {
            if (xa[sx] < 0) { row[sx] = BIT(15); continue; }
            int r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++) {
                const u16 *p = im->px + y * im->w;
                for (int x = xa[sx]; x < xb[sx]; x++, n++) {
                    u16 c = p[x];
                    r += c >> 11; g += (c >> 5) & 63; b += c & 31;
                }
            }
            row[sx] = RGB15(r / n, (g / n) >> 1, b / n) | BIT(15);
        }
    }
}

static void fit_view(const Img *im, int *vw, int *ox, int *oy) {
    int WH = 384 + gap;
    *vw = (im->w * WH >= 256 * im->h) ? 256 : im->w * WH / im->h;
    int vh = (int)((long long)im->h * *vw / im->w);
    *ox = -(256 - *vw) / 2;
    *oy = -(WH - vh) / 2;
}

// ---- bookmark --------------------------------------------------------------

static int bm_load(const char *path) {
    char p[300]; int n = 0;
    snprintf(p, sizeof p, "%s.bm", path);
    FILE *f = fopen(p, "r");
    if (f) { if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f); }
    return n;
}
static void bm_save(const char *path, int n) {
    char p[300];
    snprintf(p, sizeof p, "%s.bm", path);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%d\n", n); fclose(f); }
}

// ---- reader ----------------------------------------------------------------

static void wait_key(void) { do { swiWaitForVBlank(); scanKeys(); } while (!keysDown()); }

static void read_book(const char *path) {
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
    gfx_init();
    Img im = { 0 };
    int page = bm_load(path), zoom = 0, rtl = 1;
    int vw = 256, ox = 0, oy = 0, reload = 1, dirty = 0, tx = -1, ty = -1, refit = 0;
    if (page < 0 || page >= pdf.npages) page = 0;
    touchPosition tp;

    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        u32 dn = keysDown(), hd = keysHeld();
        if (dn & KEY_START) break;

        int step = 0;
        if (dn & (KEY_A | KEY_R)) step = 1;
        if (dn & (KEY_B | KEY_L)) step = -1;
        if (!zoom) {
            if (dn & KEY_LEFT)  step = rtl ? 1 : -1;
            if (dn & KEY_RIGHT) step = rtl ? -1 : 1;
        }
        if (dn & KEY_Y) rtl ^= 1;
        if (!zoom && (dn & (KEY_UP | KEY_DOWN))) {         // hinge gap: up = more, down = less
            gap += (dn & KEY_UP) ? 4 : -4;
            if (gap < 0) gap = 0;
            if (gap > 96) gap = 96;
            refit = 1;
        }
        if (dn & KEY_X) { zoom ^= 1; reload = 1; }
        if (step && page + step >= 0 && page + step < pdf.npages) {
            page += step; reload = 1; bm_save(path, page);
        }

        if (reload) {
            reload = 0;
            if (decode(&pdf, page, zoom ? 600 : 384, &im) < 0) { fill(RGB15(20, 0, 0) | BIT(15)); continue; }
            if (zoom) { vw = 512; ox = rtl ? vw - 256 : 0; oy = 0; }
            else fit_view(&im, &vw, &ox, &oy);
            dirty = 1;
        }
        if (!im.px) continue;
        if (refit && !zoom) { refit = 0; fit_view(&im, &vw, &ox, &oy); dirty = 1; }

        if (zoom) {
            int px = ox, py = oy;
            if (hd & KEY_LEFT) ox -= 6;
            if (hd & KEY_RIGHT) ox += 6;
            if (hd & KEY_UP) oy -= 6;
            if (hd & KEY_DOWN) oy += 6;
            if (hd & KEY_TOUCH) {
                touchRead(&tp);
                if (tx >= 0) { ox -= tp.px - tx; oy -= tp.py - ty; }
                tx = tp.px; ty = tp.py;
            } else tx = -1;
            int vh = (int)((long long)im.h * vw / im.w);
            int mx = vw - 256, my = vh - (384 + gap);
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (ox < 0) ox = 0;
            if (ox > mx) ox = mx;
            if (oy < 0) oy = 0;
            if (oy > my) oy = my;
            if (ox != px || oy != py) dirty = 1;
        }
        if (dirty) { render(&im, vw, ox, oy); dirty = 0; }
    }
    bm_save(path, page);
    free(im.px);
    pdf_close(&pdf);
}

// ---- file list -------------------------------------------------------------

static int cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static int list_pdfs(const char *dir, char **names, int max) {
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".pdf")) names[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    qsort(names, n, sizeof *names, cmp);
    return n;
}

int main(void) {
    consoleDemoInit();
    if (!fatInitDefault()) {
        iprintf("SD card / FAT init failed.\n");
        for (;;) swiWaitForVBlank();
    }
    for (;;) {
        char *names[256];
        const char *dir = "/manga/";
        int n = list_pdfs(dir, names, 256);
        if (!n) n = list_pdfs(dir = "/", names, 256);
        int sel = 0;
        for (;;) {
            consoleClear();
            iprintf("PDF reader  (%s)\n\n", dir);
            if (!n) iprintf("No PDFs in /manga or /\n");
            int top = sel > 18 ? sel - 18 : 0;
            for (int i = top; i < n && i < top + 20; i++) iprintf("%c %.29s\n", i == sel ? '>' : ' ', names[i]);
            swiWaitForVBlank(); scanKeys();
            while (!keysDown()) { swiWaitForVBlank(); scanKeys(); }
            u32 k = keysDown();
            if (k & KEY_UP && sel > 0) sel--;
            if (k & KEY_DOWN && sel < n - 1) sel++;
            if (k & KEY_START) return 0;
            if (k & KEY_A && n) break;
        }
        char path[300];
        snprintf(path, sizeof path, "%s%s", dir, names[sel]);
        for (int i = 0; i < n; i++) free(names[i]);
        read_book(path);
        videoSetMode(MODE_0_2D);       // blank the top screen again
        consoleDemoInit();
    }
}
