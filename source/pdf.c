// Minimal PDF parser: classic xref -> page tree -> first JPEG image per page.
// Fallbacks: if the xref/tree can't be used (xref streams, object streams,
// damaged files) it scans the file for objects and lists JPEG images in
// object-number order, which is page order for nearly every scan PDF.
#include "pdf.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAXOBJ   100000
#define MAXPAGES 20000

// ---- tiny token helpers ---------------------------------------------------

// value following a /Key (not a prefix of a longer name), or NULL
static const char *key(const char *d, const char *k) {
    size_t n = strlen(k);
    for (const char *s = d; (s = strstr(s, k)); s += n)
        if (!isalnum((unsigned char)s[n])) {
            s += n;
            while (isspace((unsigned char)*s)) s++;
            return s;
        }
    return NULL;
}

// "N G R" -> N, else -1
static int ref(const char *s) {
    char *e;
    long n = strtol(s, &e, 10);
    if (e == s) return -1;
    strtol(e, &e, 10);
    while (isspace((unsigned char)*e)) e++;
    return *e == 'R' ? (int)n : -1;
}

// skip past "N G R"
static const char *skipref(const char *s) {
    char *e;
    strtol(s, &e, 10); strtol(e, &e, 10);
    while (isspace((unsigned char)*e)) e++;
    return *e == 'R' ? e + 1 : e;
}

static char *rfind(char *b, size_t n, const char *k) {
    size_t kl = strlen(k);
    for (size_t i = n; i >= kl; i--)
        if (!memcmp(b + i - kl, k, kl)) return b + i - kl;
    return NULL;
}

// ---- object access --------------------------------------------------------

static int grow(Pdf *p, int n) {
    if (n < p->nobj) return 0;
    if (n >= MAXOBJ) return -1;
    int nn = n + 1024; if (nn > MAXOBJ) nn = MAXOBJ;
    uint32_t *t = realloc(p->obj, nn * sizeof *t);
    if (!t) return -1;
    memset(t + p->nobj, 0, (nn - p->nobj) * sizeof *t);
    p->obj = t; p->nobj = nn;
    return 0;
}

// first `cap` bytes of object n, NUL-terminated (caller frees)
static char *getobj(Pdf *p, int n, int cap) {
    if (n < 0 || n >= p->nobj || !p->obj[n]) return NULL;
    char *b = malloc(cap + 1);
    if (!b) return NULL;
    fseek(p->f, p->obj[n], SEEK_SET);
    size_t r = fread(b, 1, cap, p->f);
    b[r] = 0;
    char *e = strstr(b, "endobj"), *st = strstr(b, "stream");
    if (e && (!st || e < st)) *e = 0;          // don't leak into the next object
    return b;
}

static const char *body(const char *b) {      // text after "N G obj"
    const char *o = strstr(b, "obj");
    return o ? o + 3 : NULL;
}

static long numval(Pdf *p, const char *s) {   // 123 or "N G R"
    int r = ref(s);
    if (r < 0) return strtol(s, NULL, 10);
    char *b = getobj(p, r, 64);
    if (!b) return -1;
    const char *d = body(b);
    long v = d ? strtol(d, NULL, 10) : -1;
    free(b);
    return v;
}

static void add_page(Pdf *p, PdfPage pg) {
    if (p->npages == p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        PdfPage *t = realloc(p->pages, nc * sizeof *t);
        if (!t) return;
        p->pages = t; p->cap = nc;
    }
    p->pages[p->npages++] = pg;
}

// ---- images ---------------------------------------------------------------

// is object n a plain-JPEG image stream? fill in where its bytes are
static int image_at(Pdf *p, int n, PdfPage *out) {
    char *b = getobj(p, n, 2048);
    if (!b) return 0;
    int ok = 0;
    const char *d = body(b), *st = strstr(b, "stream");
    const char *t = d ? key(d, "/Subtype") : NULL;
    const char *f = d ? strstr(d, "/DCTDecode") : NULL;
    const char *z = d ? strstr(d, "/FlateDecode") : NULL;
    const char *l = d ? key(d, "/Length") : NULL;
    if (st && t && t < st && !strncmp(t, "/Image", 6) && f && f < st && !(z && z < st) && l && l < st) {
        long len = numval(p, l);
        const char *s = st + 6;
        if (*s == '\r') s++;
        if (*s == '\n') s++;
        if (len > 0) {
            out->off = p->obj[n] + (uint32_t)(s - b);
            out->len = (uint32_t)len;
            ok = 1;
        }
    }
    free(b);
    return ok;
}

// first usable image in a page's /Resources /XObject
static int page_image(Pdf *p, const char *pd, PdfPage *out) {
    char *rb = NULL, *xb = NULL;
    const char *r, *x;
    int ok = 0, rn, xn;

    r = key(pd, "/Resources");
    if (!r) return 0;
    if ((rn = ref(r)) >= 0) {
        if (!(rb = getobj(p, rn, 2048)) || !(r = body(rb))) goto done;
    }
    if (!(x = key(r, "/XObject"))) goto done;
    if ((xn = ref(x)) >= 0) {
        if (!(xb = getobj(p, xn, 2048)) || !(x = body(xb))) goto done;
        while (isspace((unsigned char)*x)) x++;
    }
    if (x[0] == '<' && x[1] == '<') x += 2;
    for (int i = 0; i < 8 && !ok; i++) {
        while (isspace((unsigned char)*x)) x++;
        if (*x != '/') break;
        for (x++; *x && !isspace((unsigned char)*x) && !strchr("/<>[]()", *x); x++) ;
        while (isspace((unsigned char)*x)) x++;
        int n = ref(x);
        if (n < 0) break;
        ok = image_at(p, n, out);
        x = skipref(x);
    }
done:
    free(rb); free(xb);
    return ok;
}

// ---- page tree ------------------------------------------------------------

static void walk(Pdf *p, int n, int depth) {
    if (depth > 12 || p->npages >= MAXPAGES) return;
    char *b = getobj(p, n, 2048);
    if (!b) return;
    if (strstr(b, "/Kids") && strlen(b) >= 2048) {        // big flat /Kids array
        free(b);
        if (!(b = getobj(p, n, 65536))) return;
    }
    const char *d = body(b), *k = d ? key(d, "/Kids") : NULL;
    if (k && *k == '[') {
        int cap = 64, cnt = 0, *kids = malloc(cap * sizeof *kids);
        const char *s = k + 1;
        while (kids) {
            while (isspace((unsigned char)*s)) s++;
            int r = isdigit((unsigned char)*s) ? ref(s) : -1;
            if (r < 0) break;
            if (cnt == cap) {
                int *t = realloc(kids, (cap *= 2) * sizeof *kids);
                if (!t) break;
                kids = t;
            }
            kids[cnt++] = r;
            s = skipref(s);
        }
        free(b);
        for (int i = 0; i < cnt; i++) walk(p, kids[i], depth + 1);
        free(kids);
    } else {
        PdfPage pg;
        if (d && page_image(p, d, &pg)) add_page(p, pg);
        free(b);
    }
}

// ---- xref table / recovery scan ------------------------------------------

static int load_xref(Pdf *p, uint32_t off, int *root) {
    for (int hop = 0; off && hop < 64; hop++) {
        char t[16]; int first, count;
        fseek(p->f, off, SEEK_SET);
        if (fscanf(p->f, "%15s", t) != 1 || strcmp(t, "xref")) return -1;   // xref stream etc.
        while (fscanf(p->f, "%d %d", &first, &count) == 2) {
            int c;
            while ((c = fgetc(p->f)) == ' ' || c == '\r' || c == '\n') ;
            ungetc(c, p->f);
            if (first < 0 || count < 0 || grow(p, first + count) < 0) return -1;
            for (int i = 0; i < count; i++) {
                char e[20];
                if (fread(e, 1, 20, p->f) != 20) return -1;
                if (e[17] == 'n' && !p->obj[first + i]) p->obj[first + i] = strtoul(e, NULL, 10);
            }
        }
        if (fscanf(p->f, "%15s", t) != 1 || strcmp(t, "trailer")) return -1;
        char *b = malloc(1025);
        if (!b) return -1;
        b[fread(b, 1, 1024, p->f)] = 0;
        char *e = strstr(b, "startxref");
        if (e) *e = 0;
        const char *r = key(b, "/Root"), *v = key(b, "/Prev");
        if (*root < 0 && r) *root = ref(r);
        off = v ? strtoul(v, NULL, 10) : 0;
        free(b);
    }
    return 0;
}

// find every "N G obj" in the file; later copies win (incremental updates)
static void scan_objects(Pdf *p) {
    enum { C = 32768, OV = 24 };
    unsigned char *b = malloc(C);
    if (!b) return;
    for (uint32_t pos = 0; pos < p->size; pos += C - OV) {
        fseek(p->f, pos, SEEK_SET);
        int n = fread(b, 1, C, p->f);
        if (n <= 0) break;
        for (int i = 1; i + 4 < n; i++) {
            if (b[i] != ' ' || memcmp(b + i + 1, "obj", 3) || isalpha(b[i + 4])) continue;
            int g = i; while (g > 0 && isdigit(b[g - 1])) g--;
            if (g == i || g < 2 || b[g - 1] != ' ') continue;
            int e = g - 1, s = e; while (s > 0 && isdigit(b[s - 1])) s--;
            if (s == e || (s == 0 && pos)) continue;
            long num = strtol((char *)b + s, NULL, 10);
            if (num > 0 && grow(p, (int)num) == 0) p->obj[num] = pos + s;
        }
        if (n < C) break;
    }
    free(b);
}

// count the image objects by encoding (only used to explain a failure)
static void tally(Pdf *p) {
    for (int i = 1; i < p->nobj; i++) {
        char *b = p->obj[i] ? getobj(p, i, 2048) : NULL;
        if (!b) continue;
        p->n_obj++;
        const char *d = body(b), *st = strstr(b, "stream"), *t = d ? key(d, "/Subtype") : NULL, *x;
        if (d && st && t && t < st && !strncmp(t, "/Image", 6)) {
            if ((x = strstr(d, "/FlateDecode")) && x < st) p->n_flate++;
            else if ((x = strstr(d, "/DCTDecode")) && x < st) p->n_dct++;
            else if ((x = strstr(d, "/JPXDecode")) && x < st) p->n_jpx++;
            else p->n_other++;
        }
        free(b);
    }
}

// ---- public ---------------------------------------------------------------

int pdf_open(Pdf *p, const char *path) {
    memset(p, 0, sizeof *p);
    if (!(p->f = fopen(path, "rb"))) return -1;
    setvbuf(p->f, NULL, _IOFBF, 8192);
    fseek(p->f, 0, SEEK_END);
    p->size = ftell(p->f);

    char tail[2049];
    uint32_t t = p->size > 2048 ? p->size - 2048 : 0;
    fseek(p->f, t, SEEK_SET);
    size_t n = fread(tail, 1, 2048, p->f);
    tail[n] = 0;
    char *sx = rfind(tail, n, "startxref");
    char *rt = rfind(tail, n, "/Root");
    int root = rt ? ref(rt + 5) : -1;

    if (sx && load_xref(p, strtoul(sx + 9, NULL, 10), &root) == 0 && root >= 0) {
        char *rb = getobj(p, root, 2048);
        const char *d = rb ? body(rb) : NULL, *pg = d ? key(d, "/Pages") : NULL;
        int pn = pg ? ref(pg) : -1;
        free(rb);
        if (pn >= 0) walk(p, pn, 0);
    }
    if (!p->npages) {                       // fallback: JPEG images in object order
        scan_objects(p);
        for (int i = 1; i < p->nobj; i++) {
            PdfPage pg;
            if (p->obj[i] && image_at(p, i, &pg)) add_page(p, pg);
        }
    }
    if (!p->npages) tally(p);
    return 0;
}

void pdf_diag(const Pdf *p, char *out, size_t n) {
    snprintf(out, n, "%u KB, %d objects\nimages: JPEG %d, Flate %d,\nJPEG2000 %d, other %d\n",
             (unsigned)(p->size >> 10), p->n_obj, p->n_dct, p->n_flate, p->n_jpx, p->n_other);
}

void pdf_close(Pdf *p) {
    if (p->f) fclose(p->f);
    free(p->obj); free(p->pages);
    memset(p, 0, sizeof *p);
}
