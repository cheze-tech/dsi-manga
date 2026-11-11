// EPUB 2 / basic EPUB 3 reader core.
//
// This file is standalone (stdio/stdlib/string only) so it can be compiled and
// fuzzed on the host with tools/epub_test.c.  It owns:
//   - the ZIP container (central directory, stored + deflate entries)
//   - a small RFC 1951 inflate (no zlib dependency)
//   - container.xml, OPF manifest/spine/metadata, EPUB 2 NCX and EPUB 3 nav
//   - XHTML -> wrapped text lines + full-page image references
//   - JPEG/PNG header sniffing, and a compact PNG decoder (8-bit, non-interlaced)
//
// Page turns, drawing, zoom and input stay in enhanced.c.  PDF/TXT/audio code is
// not referenced here at all.
#include "epub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define MAXENT      4096
#define MAXMAN      2048
#define MAXSPINE    1024
#define MAXIMG      1024
#define MAXTOC      512
#define MAXENTRY    (8u << 20)     // biggest single ZIP entry we will unpack
#define MAXTEXT     (2u << 20)     // chapter text kept in RAM
#define MAXPARA     8192
#define MAXXML      (2u << 20)

typedef struct { char *name; uint32_t lho, csize, usize, crc; uint16_t method; } ZEnt;
typedef struct { char *id, *href, *type, *props; int zip; } Man;
typedef struct { char *title; int chapter; } Toc;

struct Epub {
    FILE *f;
    uint32_t fsize;
    ZEnt *ent; int nent;

    Man *man; int nman;
    int *spine; int nch;              // manifest indexes, reading order
    int *man2chap; int *man2img;
    int *img; int nimg;               // manifest indexes of image/* items
    int cover;
    char *dir;                        // OPF directory, with trailing slash
    char title[128];

    Toc *toc; int ntoc;
    char **chtitle;

    int cur_ch;                       // -1 = nothing loaded
    char *txt; uint32_t txtlen, txtcap;
    EpubLine *lines; int nlines, lcap;
    EpubPage *pages; int npages, pcap;
    int pagelines, pagefirst;
    char *pbuf; int plen, pbsize;
};

// ---- small helpers ---------------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    return q;
}

static uint32_t crc_tab[256];
static int crc_ready;
static void crc_init(void) {
    if (crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        crc_tab[i] = c;
    }
    crc_ready = 1;
}
static uint32_t crc_buf(const uint8_t *p, size_t n) {
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = (c >> 8) ^ crc_tab[(c ^ *p++) & 0xFF];
    return ~c;
}

// Normalise a ZIP/EPUB path: slashes, "." and ".." segments, no leading slash.
static void norm_path(const char *in, char *out, size_t cap) {
    char tmp[1200];
    size_t o = 0, seg[64];
    int depth = 0;
    const char *p = in;
    while (*p == '/') p++;
    while (*p) {
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t l = (size_t)(p - s);
        if (l == 0 || (l == 1 && s[0] == '.')) {
            // skip
        } else if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (depth) o = seg[--depth];
        } else if (depth < 64 && o + l + 1 < sizeof tmp) {
            seg[depth++] = o;
            memcpy(tmp + o, s, l);
            o += l;
            tmp[o++] = '/';
        }
        if (*p) p++;
    }
    if (o && tmp[o - 1] == '/') o--;
    if (o >= cap) o = cap ? cap - 1 : 0;
    memcpy(out, tmp, o);
    out[o] = 0;
}

static void pct_decode(char *s) {
    char *d = s;
    for (const char *p = s; *p; p++) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            int hi = isdigit((unsigned char)p[1]) ? p[1] - '0' : (tolower((unsigned char)p[1]) - 'a' + 10);
            int lo = isdigit((unsigned char)p[2]) ? p[2] - '0' : (tolower((unsigned char)p[2]) - 'a' + 10);
            *d++ = (char)((hi << 4) | lo);
            p += 2;
        } else *d++ = *p;
    }
    *d = 0;
}

static void dirname_of(const char *path, char *out, size_t cap) {
    const char *s = strrchr(path, '/');
    size_t n = s ? (size_t)(s - path) + 1 : 0;
    if (n >= cap) n = cap ? cap - 1 : 0;
    memcpy(out, path, n);
    out[n] = 0;
}

static void resolve_path(const char *base, const char *href, char *out, size_t cap) {
    char h[800];
    size_t n = strcspn(href, "#");
    if (n >= sizeof h) n = sizeof h - 1;
    memcpy(h, href, n);
    h[n] = 0;
    pct_decode(h);
    char joined[1000];
    if (h[0] == '/') snprintf(joined, sizeof joined, "%s", h + 1);
    else snprintf(joined, sizeof joined, "%s%s", base ? base : "", h);
    norm_path(joined, out, cap);
}

// UTF-8 -> one ASCII byte.  Returns bytes consumed, or 0 if not a sequence.
static int utf8_ascii(const unsigned char *s, size_t n, char *out) {
    unsigned c = s[0];
    if (c < 0x80) { *out = (char)c; return 1; }
    if (c == 0xC2 && n >= 2) {
        unsigned d = s[1];
        *out = d == 0xA0 ? ' ' : d == 0xA9 ? 'c' : d == 0xAE ? 'r' : d == 0xB0 ? 'o'
             : d == 0xB7 ? '.' : d == 0xAB ? '<' : d == 0xBB ? '>' : '?';
        return 2;
    }
    if (c == 0xC3 && n >= 2) {
        unsigned d = s[1];
        static const char map[64] = {
            'a','a','a','a','a','a','a','c','e','e','e','e','i','i','i','i',
            'd','n','o','o','o','o','o','x','o','u','u','u','u','y','t','s',
            'a','a','a','a','a','a','a','c','e','e','e','e','i','i','i','i',
            'd','n','o','o','o','o','o','/','o','u','u','u','u','y','t','y'
        };
        *out = (d >= 0x80 && d <= 0xBF) ? map[d - 0x80] : '?';
        return 2;
    }
    if (c == 0xE2 && n >= 3 && s[1] == 0x80) {
        switch (s[2]) {
            case 0x93: case 0x94: *out = '-'; return 3;
            case 0x98: case 0x99: *out = '\''; return 3;
            case 0x9C: case 0x9D: *out = '"'; return 3;
            case 0x9A: case 0x9E: *out = ','; return 3;
            case 0xA6: *out = '.'; return 3;      // ellipsis: one dot is enough here
            case 0x8B: case 0x8C: case 0x8D: case 0x8E: *out = 0; return 3;
            default: *out = ' '; return 3;
        }
    }
    // Eat the whole sequence so a 2/3/4-byte character is not turned into 2-4 '?'.
    int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if ((size_t)len > n) len = (int)n;
    *out = '?';
    return len;
}

static int entity_ascii(const char *s, size_t n, char *out) {
    if (n < 3 || s[0] != '&') return 0;
    size_t e = 1;
    while (e < n && e < 12 && s[e] != ';') e++;
    if (e >= n || s[e] != ';') return 0;
    char name[12];
    size_t l = e - 1;
    memcpy(name, s + 1, l);
    name[l] = 0;
    if (name[0] == '#') {
        long v = 0;
        if (name[1] == 'x' || name[1] == 'X') v = strtol(name + 2, NULL, 16);
        else v = strtol(name + 1, NULL, 10);
        if (v == 160) *out = ' ';
        else if (v == 8211 || v == 8212) *out = '-';
        else if (v == 8216 || v == 8217) *out = '\'';
        else if (v == 8220 || v == 8221) *out = '"';
        else if (v == 8230) *out = '.';
        else *out = (v > 31 && v < 127) ? (char)v : '?';
        return (int)e + 1;
    }
    static const struct { const char *n; char c; } tab[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
        { "nbsp", ' ' }, { "copy", 'c' }, { "reg", 'r' }, { "hellip", '.' }, { "mdash", '-' },
        { "ndash", '-' }, { "lsquo", '\'' }, { "rsquo", '\'' }, { "ldquo", '"' }, { "rdquo", '"' }
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(name, tab[i].n)) { *out = tab[i].c; return (int)e + 1; }
    *out = '?';
    return (int)e + 1;
}

// ---- inflate (RFC 1951) ----------------------------------------------------

typedef struct {
    const uint8_t *in; size_t inlen, inpos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outcap, outpos;
} Inf;

static int inf_bits(Inf *s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) return -1;
        v |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

typedef struct { short count[16]; short sym[288]; } Huff;

static int huff_build(Huff *h, const uint8_t *len, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;
    }
    short offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    return 0;
}

static int huff_decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const short lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short lext[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short dext[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inflate_mem(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen) {
    Inf s;
    memset(&s, 0, sizeof s);
    s.in = src; s.inlen = slen; s.out = dst; s.outcap = dlen;
    int last;
    do {
        last = inf_bits(&s, 1);
        if (last < 0) return -1;
        int type = inf_bits(&s, 2);
        if (type < 0) return -1;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;              // stored block starts on a byte boundary
            if (s.inpos + 4 > s.inlen) return -1;
            unsigned len = (unsigned)rd16(s.in + s.inpos);
            unsigned nlen = (unsigned)rd16(s.in + s.inpos + 2);
            s.inpos += 4;
            if ((len ^ 0xFFFFu) != nlen) return -1;
            if (s.inpos + len > s.inlen || s.outpos + len > s.outcap) return -1;
            memcpy(s.out + s.outpos, s.in + s.inpos, len);
            s.inpos += len; s.outpos += len;
        } else if (type == 1 || type == 2) {
            Huff lit, dist;
            if (type == 1) {
                uint8_t l[288];
                int i;
                for (i = 0; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                if (huff_build(&lit, l, 288)) return -1;
                uint8_t d[30];
                memset(d, 5, sizeof d);
                if (huff_build(&dist, d, 30)) return -1;
            } else {
                int hlit = inf_bits(&s, 5), hdist = inf_bits(&s, 5), hclen = inf_bits(&s, 4);
                if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
                hlit += 257; hdist += 1; hclen += 4;
                if (hlit > 286 || hdist > 30 || hclen > 19) return -1;
                static const uint8_t ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                uint8_t cl[19];
                memset(cl, 0, sizeof cl);
                for (int i = 0; i < hclen; i++) {
                    int v = inf_bits(&s, 3);
                    if (v < 0) return -1;
                    cl[ord[i]] = (uint8_t)v;
                }
                Huff ch;
                if (huff_build(&ch, cl, 19)) return -1;
                uint8_t lens[318];
                int n = 0;
                while (n < hlit + hdist) {
                    int sym = huff_decode(&s, &ch);
                    if (sym < 0) return -1;
                    if (sym < 16) lens[n++] = (uint8_t)sym;
                    else {
                        int rep, val = 0;
                        if (sym == 16) {
                            if (n == 0) return -1;
                            val = lens[n - 1];
                            rep = inf_bits(&s, 2);
                            if (rep < 0) return -1;
                            rep += 3;
                        } else if (sym == 17) {
                            rep = inf_bits(&s, 3);
                            if (rep < 0) return -1;
                            rep += 3;
                        } else {
                            rep = inf_bits(&s, 7);
                            if (rep < 0) return -1;
                            rep += 11;
                        }
                        if (n + rep > hlit + hdist) return -1;
                        while (rep--) lens[n++] = (uint8_t)val;
                    }
                }
                if (lens[256] == 0) return -1;
                if (huff_build(&lit, lens, hlit)) return -1;
                if (huff_build(&dist, lens + hlit, hdist)) return -1;
            }
            for (;;) {
                int sym = huff_decode(&s, &lit);
                if (sym < 0) return -1;
                if (sym < 256) {
                    if (s.outpos >= s.outcap) return -1;
                    s.out[s.outpos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return -1;
                    int eb = inf_bits(&s, lext[sym]);
                    if (eb < 0) return -1;
                    int len = lbase[sym] + eb;
                    int dsym = huff_decode(&s, &dist);
                    if (dsym < 0 || dsym >= 30) return -1;
                    int db = inf_bits(&s, dext[dsym]);
                    if (db < 0) return -1;
                    int d = dbase[dsym] + db;
                    if ((size_t)d > s.outpos || s.outpos + len > s.outcap) return -1;
                    while (len--) {
                        s.out[s.outpos] = s.out[s.outpos - d];
                        s.outpos++;
                    }
                }
            }
        } else return -1;
    } while (!last);
    return s.outpos == dlen ? 0 : -1;
}

// ---- ZIP -------------------------------------------------------------------

static int zip_find(const Epub *e, const char *name) {
    for (int i = 0; i < e->nent; i++)
        if (!strcmp(e->ent[i].name, name)) return i;
    return -1;
}

static int zip_open(Epub *e, const char *path) {
    e->f = fopen(path, "rb");
    if (!e->f) return -1;
    if (fseek(e->f, 0, SEEK_END) != 0) return -1;
    long sz = ftell(e->f);
    if (sz < 22 || sz > 0x7FFFFFFF) return -1;
    e->fsize = (uint32_t)sz;

    size_t tail = e->fsize > 65557 ? 65557 : e->fsize;
    uint8_t *t = malloc(tail);
    if (!t) return -1;
    if (fseek(e->f, (long)(e->fsize - tail), SEEK_SET) || fread(t, 1, tail, e->f) != tail) {
        free(t);
        return -1;
    }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--) {
        if (rd32(t + i) == 0x06054b50u) { eocd = i; break; }
    }
    free(t);
    if (eocd < 0) return -1;

    if (fseek(e->f, (long)(e->fsize - tail + (size_t)eocd), SEEK_SET)) return -1;
    uint8_t h[22];
    if (fread(h, 1, 22, e->f) != 22) return -1;
    uint16_t count = rd16(h + 10);
    uint32_t cdsize = rd32(h + 12), cdoff = rd32(h + 16);
    if (count == 0xFFFF || cdoff == 0xFFFFFFFFu || cdsize > MAXENTRY) return -1;   // ZIP64
    if ((uint64_t)cdoff + cdsize > e->fsize) return -1;

    uint8_t *cd = malloc(cdsize ? cdsize : 1);
    if (!cd) return -1;
    if (fseek(e->f, (long)cdoff, SEEK_SET) || fread(cd, 1, cdsize, e->f) != cdsize) {
        free(cd);
        return -1;
    }
    e->ent = calloc(MAXENT, sizeof *e->ent);
    if (!e->ent) { free(cd); return -1; }

    size_t p = 0;
    while (p + 46 <= cdsize && e->nent < MAXENT && rd32(cd + p) == 0x02014b50u) {
        uint16_t method = rd16(cd + p + 10);
        uint32_t crc = rd32(cd + p + 16), csize = rd32(cd + p + 20), usize = rd32(cd + p + 24);
        uint16_t nl = rd16(cd + p + 28), el = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        uint32_t lho = rd32(cd + p + 42);
        if (p + 46 + nl > cdsize) break;
        const char *raw = (const char *)cd + p + 46;
        if (nl && raw[nl - 1] != '/') {
            char name[800], norm[800];
            size_t n = nl < sizeof name - 1 ? nl : sizeof name - 1;
            memcpy(name, raw, n);
            name[n] = 0;
            norm_path(name, norm, sizeof norm);
            ZEnt *z = &e->ent[e->nent];
            z->name = strdup(norm);
            if (z->name) {
                z->lho = lho; z->csize = csize; z->usize = usize;
                z->crc = crc; z->method = method;
                e->nent++;
            }
        }
        p += 46u + nl + el + cl;
    }
    free(cd);
    return e->nent ? 0 : -1;
}

static int zip_extract(Epub *e, int idx, uint8_t **out, size_t *len) {
    *out = NULL; *len = 0;
    if (!e || idx < 0 || idx >= e->nent) return -1;
    ZEnt *z = &e->ent[idx];
    if (z->usize > MAXENTRY) return -1;
    if (fseek(e->f, (long)z->lho, SEEK_SET)) return -1;
    uint8_t lh[30];
    if (fread(lh, 1, 30, e->f) != 30 || rd32(lh) != 0x04034b50u) return -1;
    uint16_t nl = rd16(lh + 26), el = rd16(lh + 28);
    long data = (long)z->lho + 30 + nl + el;
    uint8_t *dst = malloc((size_t)z->usize + 1);
    if (!dst) return -1;
    if (z->method == 0) {
        if (z->csize != z->usize || z->usize > MAXENTRY) { free(dst); return -1; }
        if (fseek(e->f, data, SEEK_SET) || fread(dst, 1, z->usize, e->f) != z->usize) {
            free(dst); return -1;
        }
    } else if (z->method == 8) {
        if (z->csize > MAXENTRY) { free(dst); return -1; }
        uint8_t *comp = malloc(z->csize ? z->csize : 1);
        if (!comp) { free(dst); return -1; }
        if (fseek(e->f, data, SEEK_SET) || fread(comp, 1, z->csize, e->f) != z->csize) {
            free(comp); free(dst); return -1;
        }
        int rc = inflate_mem(comp, z->csize, dst, z->usize);
        free(comp);
        if (rc) { free(dst); return -1; }
    } else {
        free(dst);
        return -1;
    }
    if (z->usize && crc_buf(dst, z->usize) != z->crc) { free(dst); return -1; }
    dst[z->usize] = 0;                                    // convenient NUL for XML/text
    *out = dst;
    *len = z->usize;
    return 0;
}

// ---- XML -------------------------------------------------------------------

// Find the next opening tag whose local name is `want` (prefixes ignored).
static const char *tag_open(const char *p, const char *want) {
    size_t wl = strlen(want);
    while (p && (p = strchr(p, '<'))) {
        if (!strncmp(p, "<!--", 4)) {
            const char *q = strstr(p + 4, "-->");
            p = q ? q + 3 : p + 4;
            continue;
        }
        if (!strncmp(p, "<![CDATA[", 9)) {
            const char *q = strstr(p + 9, "]]>");
            p = q ? q + 3 : p + 9;
            continue;
        }
        if (p[1] == '!' || p[1] == '?') {
            const char *q = strchr(p, '>');
            p = q ? q + 1 : p + 1;
            continue;
        }
        const char *q = p + 1;
        if (*q == '/') { p = q + 1; continue; }
        const char *nm = q;
        while (*q && (isalnum((unsigned char)*q) || *q == '-' || *q == '_' || *q == ':')) q++;
        const char *loc = nm;
        for (const char *r = nm; r < q; r++) if (*r == ':') loc = r + 1;
        if ((size_t)(q - loc) == wl && !strncasecmp(loc, want, wl)) return p;
        p = q;
    }
    return NULL;
}

static int tag_attr(const char *tag, const char *name, char *out, size_t cap) {
    out[0] = 0;
    const char *end = strchr(tag, '>');
    if (!end) end = tag + strlen(tag);
    const char *p = tag + 1;
    size_t nl = strlen(name);
    while (p < end) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (p < end && *p != q) p++;
            if (p < end) p++;
            continue;
        }
        if (!isalpha((unsigned char)*p)) { p++; continue; }
        const char *k = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ':')) p++;
        const char *loc = k;
        for (const char *r = k; r < p; r++) if (*r == ':') loc = r + 1;
        size_t kl = (size_t)(p - loc);
        const char *save = p;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p < end && *p == '=' && kl == nl && !strncasecmp(loc, name, nl)) {
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            char q = (p < end) ? *p : 0;
            if (q != '"' && q != '\'') { p = save; continue; }
            p++;
            const char *v = p;
            while (p < end && *p != q) p++;
            size_t vl = (size_t)(p - v);
            if (vl >= cap) vl = cap ? cap - 1 : 0;
            for (size_t i = 0; i < vl; i++) out[i] = v[i];
            out[vl] = 0;
            return 1;
        }
        p = save;
    }
    return 0;
}

// Strip tags/entities to simple ASCII.  Used for titles and TOC labels.
static void plain_text(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    int sp = 1;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '<') {
            while (i < n && s[i] != '>') i++;
            continue;
        }
        if (c == '&') {
            char ch = 0;
            int used = entity_ascii(s + i, n - i, &ch);
            if (used) {
                i += (size_t)used - 1;
                if (ch == ' ') { if (!sp && o + 1 < cap) { out[o++] = ' '; sp = 1; } }
                else if (ch) { out[o++] = ch; sp = 0; }
                continue;
            }
        }
        if (c >= 128) {
            char ch = 0;
            int used = utf8_ascii((const unsigned char *)s + i, n - i, &ch);
            if (used) {
                i += (size_t)used - 1;
                if (ch == ' ') { if (!sp && o + 1 < cap) { out[o++] = ' '; sp = 1; } }
                else if (ch) { out[o++] = ch; sp = 0; }
                continue;
            }
        }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!sp && o + 1 < cap) { out[o++] = ' '; sp = 1; }
        } else if (c >= 32) {
            out[o++] = (char)c;
            sp = 0;
        }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static int inner_plain(const char *xml, const char *local, char *out, size_t cap) {
    const char *t = tag_open(xml, local);
    if (!t) { out[0] = 0; return 0; }
    const char *gt = strchr(t, '>');
    if (!gt) { out[0] = 0; return 0; }
    const char *p = gt + 1, *q = p;
    size_t ll = strlen(local);
    while ((q = strstr(q, "</"))) {
        const char *nm = q + 2, *r = nm;
        while (*r && *r != '>' && !isspace((unsigned char)*r)) r++;
        const char *loc = nm;
        for (const char *x = nm; x < r; x++) if (*x == ':') loc = x + 1;
        if ((size_t)(r - loc) == ll && !strncasecmp(loc, local, ll)) break;
        q = r;
    }
    size_t n = q ? (size_t)(q - p) : strlen(p);
    plain_text(p, n, out, cap);
    return out[0] != 0;
}

static int man_by_id(const Epub *e, const char *id) {
    for (int i = 0; i < e->nman; i++)
        if (e->man[i].id && !strcmp(e->man[i].id, id)) return i;
    return -1;
}
static int man_by_path(const Epub *e, const char *path) {
    for (int i = 0; i < e->nman; i++)
        if (e->man[i].href && !strcmp(e->man[i].href, path)) return i;
    return -1;
}

// ---- chapter text -> pages -------------------------------------------------

static void ch_free(Epub *e) {
    free(e->txt); e->txt = NULL;
    e->txtlen = e->txtcap = 0;
    free(e->lines); e->lines = NULL;
    e->nlines = e->lcap = 0;
    free(e->pages); e->pages = NULL;
    e->npages = e->pcap = 0;
    e->pagelines = e->pagefirst = 0;
    e->plen = 0;
    e->cur_ch = -1;
}

static void ep_close_page(Epub *e) {
    if (e->pagelines <= 0) return;
    if (e->npages == e->pcap) {
        int nc = e->pcap ? e->pcap * 2 : 32;
        EpubPage *p = xrealloc(e->pages, (size_t)nc * sizeof *p);
        if (!p) { e->pagelines = 0; return; }
        e->pages = p; e->pcap = nc;
    }
    e->pages[e->npages].first = e->pagefirst;
    e->pages[e->npages].n = e->pagelines;
    e->pages[e->npages].img = -1;
    e->npages++;
    e->pagefirst = e->nlines;
    e->pagelines = 0;
}

static void ep_add_line(Epub *e, const char *s, int n) {
    if (n <= 0) return;
    if (e->pagelines >= EPUB_ROWS) ep_close_page(e);
    uint32_t need = e->txtlen + (uint32_t)n + 1;
    if (need > e->txtcap) {
        uint32_t nc = e->txtcap ? e->txtcap * 2 : 16384;
        while (nc < need) nc *= 2;
        if (nc > MAXTEXT) nc = MAXTEXT;
        if (nc < need) return;                 // chapter text too large: stop adding
        char *t = xrealloc(e->txt, nc);
        if (!t) return;
        e->txt = t; e->txtcap = nc;
    }
    if (e->nlines == e->lcap) {
        int nc = e->lcap ? e->lcap * 2 : 256;
        EpubLine *l = xrealloc(e->lines, (size_t)nc * sizeof *l);
        if (!l) return;
        e->lines = l; e->lcap = nc;
    }
    EpubLine *l = &e->lines[e->nlines++];
    l->off = e->txtlen;
    l->len = (uint16_t)n;
    l->kind = EPUB_LINE_TEXT;
    memcpy(e->txt + e->txtlen, s, n);
    e->txtlen += (uint32_t)n;
    e->txt[e->txtlen++] = 0;
    e->pagelines++;
}

static void ep_add_blank(Epub *e) {
    if (e->nlines && e->lines[e->nlines - 1].kind == EPUB_LINE_BLANK) return;
    if (e->nlines == e->lcap) {
        int nc = e->lcap ? e->lcap * 2 : 256;
        EpubLine *l = xrealloc(e->lines, (size_t)nc * sizeof *l);
        if (!l) return;
        e->lines = l; e->lcap = nc;
    }
    if (e->pagelines >= EPUB_ROWS) ep_close_page(e);
    EpubLine *l = &e->lines[e->nlines++];
    l->off = 0; l->len = 0; l->kind = EPUB_LINE_BLANK;
    e->pagelines++;
}

static void ep_add_image_page(Epub *e, int img) {
    ep_close_page(e);
    if (e->npages == e->pcap) {
        int nc = e->pcap ? e->pcap * 2 : 32;
        EpubPage *p = xrealloc(e->pages, (size_t)nc * sizeof *p);
        if (!p) return;
        e->pages = p; e->pcap = nc;
    }
    e->pages[e->npages].first = 0;
    e->pages[e->npages].n = 0;
    e->pages[e->npages].img = img;
    e->npages++;
    e->pagefirst = e->nlines;
}

static void ep_wrap(Epub *e, const char *s, int n) {
    int i = 0;
    while (i < n) {
        while (i < n && s[i] == ' ') i++;
        if (i >= n) break;
        int start = i, last = -1, j = i, len = 0;
        while (j < n && len < EPUB_COLS) {
            if (s[j] == ' ') last = j;
            j++; len++;
        }
        int end;
        if (j >= n) end = n;
        else if (s[j] == ' ') end = j;
        else if (last > start) end = last;
        else end = j;
        if (end <= start) end = start + 1;
        ep_add_line(e, s + start, end - start);
        i = end;
        while (i < n && s[i] == ' ') i++;
    }
}

static void ep_flush_para(Epub *e, int blank) {
    while (e->plen > 0 && e->pbuf[e->plen - 1] == ' ') e->plen--;
    if (e->plen > 0) ep_wrap(e, e->pbuf, e->plen);
    e->plen = 0;
    if (blank) ep_add_blank(e);
}

static void ep_put(Epub *e, char c) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ') {
        if (e->plen == 0 || e->pbuf[e->plen - 1] == ' ') return;
    }
    if (!e->pbuf) return;
    if (e->plen >= e->pbsize - 1) ep_flush_para(e, 1);
    if (e->plen < e->pbsize - 1) e->pbuf[e->plen++] = c;
}

static int name_is(const char *loc, size_t n, const char *want) {
    size_t w = strlen(want);
    return n == w && !strncasecmp(loc, want, w);
}

static int block_start(const char *loc, size_t n) {
    static const char *t[] = {
        "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul", "ol",
        "tr", "table", "blockquote", "pre", "section", "article", "header", "footer",
        "figure", "figcaption", "hr", "aside", "nav", "main"
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (name_is(loc, n, t[i])) return 1;
    return 0;
}

static const char *skip_element(const char *p, const char *end, const char *local) {
    size_t ll = strlen(local);
    while (p && p < end) {
        const char *q = strstr(p, "</");
        if (!q || q >= end) return end;
        const char *nm = q + 2, *r = nm;
        while (r < end && *r != '>' && !isspace((unsigned char)*r)) r++;
        const char *loc = nm;
        for (const char *x = nm; x < r; x++) if (*x == ':') loc = x + 1;
        if ((size_t)(r - loc) == ll && !strncasecmp(loc, local, ll)) {
            const char *gt = memchr(r, '>', (size_t)(end - r));
            return gt ? gt + 1 : end;
        }
        p = r;
    }
    return end;
}

// <img src>, or the <image xlink:href> commonly used inside SVG wrappers in
// fixed-layout comics.  Resolve against the chapter and map to an image id.
static void ep_image_ref(Epub *e, const char *dir, const char *src) {
    if (!e || !src || !src[0]) return;
    char path[800];
    resolve_path(dir, src, path, sizeof path);
    int mi = man_by_path(e, path);
    int id = mi >= 0 ? e->man2img[mi] : -1;
    if (id >= 0) ep_add_image_page(e, id);
}

static void parse_xhtml(Epub *e, const char *doc, int n, const char *dir) {
    const char *end = doc + n;
    const char *body = tag_open(doc, "body");
    const char *p = doc;
    if (body) {
        const char *gt = strchr(body, '>');
        if (gt) p = gt + 1;
    }
    if (!e->pbuf) {
        e->pbsize = MAXPARA;
        e->pbuf = malloc(e->pbsize);
        if (!e->pbuf) return;
    }
    e->plen = 0;

    while (p < end) {
        if (*p != '<') {
            if (*p == '&' || (unsigned char)*p >= 128) {
                char o = 0;
                int u = 0;
                if (*p == '&') u = entity_ascii(p, (size_t)(end - p), &o);
                else u = utf8_ascii((const unsigned char *)p, (size_t)(end - p), &o);
                if (u) { if (o) ep_put(e, o); p += u; continue; }
            }
            ep_put(e, *p++);
            continue;
        }
        if (end - p >= 4 && !strncmp(p, "<!--", 4)) {
            const char *q = strstr(p + 4, "-->");
            p = q ? q + 3 : end;
            continue;
        }
        if (end - p >= 9 && !strncmp(p, "<![CDATA[", 9)) {
            const char *q = strstr(p + 9, "]]>");
            const char *t = p + 9;
            const char *te = q ? q : end;
            while (t < te) ep_put(e, *t++);
            p = q ? q + 3 : end;
            continue;
        }
        if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
            const char *q = memchr(p, '>', (size_t)(end - p));
            p = q ? q + 1 : end;
            continue;
        }
        int closing = (p + 1 < end && p[1] == '/');
        const char *q = p + 1 + (closing ? 1 : 0);
        const char *nm = q;
        while (q < end && (isalnum((unsigned char)*q) || *q == '-' || *q == '_' || *q == ':')) q++;
        const char *loc = nm;
        for (const char *x = nm; x < q; x++) if (*x == ':') loc = x + 1;
        size_t nl = (size_t)(q - loc);
        const char *gt = memchr(q, '>', (size_t)(end - q));
        if (!gt) break;

        if (!closing) {
            if (name_is(loc, nl, "script") || name_is(loc, nl, "style")) {
                p = skip_element(gt + 1, end, name_is(loc, nl, "script") ? "script" : "style");
                continue;
            }
            if (name_is(loc, nl, "img") || name_is(loc, nl, "image")) {
                char src[600];
                src[0] = 0;
                tag_attr(p, "href", src, sizeof src);       // xlink:href in SVG
                if (!src[0]) tag_attr(p, "src", src, sizeof src);
                ep_flush_para(e, 1);
                ep_image_ref(e, dir, src);
                p = gt + 1;
                continue;
            }
            if (name_is(loc, nl, "br")) {
                ep_flush_para(e, 0);
                p = gt + 1;
                continue;
            }
            if (name_is(loc, nl, "li")) {
                ep_flush_para(e, 1);
                ep_put(e, '-'); ep_put(e, ' ');
            } else if (block_start(loc, nl)) {
                ep_flush_para(e, 1);
            }
        } else {
            if (block_start(loc, nl)) ep_flush_para(e, 1);
        }
        p = gt + 1;
    }
    ep_flush_para(e, 0);
    ep_close_page(e);
}

// ---- OPF / TOC -------------------------------------------------------------

static void add_toc(Epub *e, const char *title, int chapter) {
    if (!e->toc || !title || !title[0] || e->ntoc >= MAXTOC) return;
    for (int i = 0; i < e->ntoc; i++)
        if (e->toc[i].chapter == chapter && !strcmp(e->toc[i].title, title)) return;
    e->toc[e->ntoc].title = strdup(title);
    e->toc[e->ntoc].chapter = chapter;
    if (e->toc[e->ntoc].title) e->ntoc++;
}

static int is_html(const Man *m) {
    if (m->type && strstr(m->type, "html")) return 1;
    const char *h = m->href ? m->href : "";
    size_t n = strlen(h);
    return (n > 6 && !strcasecmp(h + n - 6, ".xhtml")) ||
           (n > 5 && !strcasecmp(h + n - 5, ".html")) ||
           (n > 4 && !strcasecmp(h + n - 4, ".htm"));
}

static int load_container(Epub *e, char *out, size_t cap) {
    int zi = zip_find(e, "META-INF/container.xml");
    uint8_t *raw = NULL; size_t n = 0;
    if (zi < 0 || zip_extract(e, zi, &raw, &n)) return -1;
    const char *p = tag_open((const char *)raw, "rootfile");
    char path[600];
    if (!p || !tag_attr(p, "full-path", path, sizeof path)) { free(raw); return -1; }
    resolve_path("", path, out, cap);
    free(raw);
    return out[0] ? 0 : -1;
}

static int load_opf(Epub *e, const char *path) {
    int zi = zip_find(e, path);
    uint8_t *raw = NULL; size_t n = 0;
    if (zi < 0 || zip_extract(e, zi, &raw, &n) || n > MAXXML) return -1;
    const char *xml = (const char *)raw;
    char dir[600];
    dirname_of(path, dir, sizeof dir);
    e->dir = strdup(dir);
    e->man = calloc(MAXMAN, sizeof *e->man);
    e->spine = calloc(MAXSPINE, sizeof *e->spine);
    e->img = calloc(MAXIMG, sizeof *e->img);
    e->toc = calloc(MAXTOC, sizeof *e->toc);
    if (!e->dir || !e->man || !e->spine || !e->img || !e->toc) { free(raw); return -1; }

    char tmp[160];
    if (inner_plain(xml, "title", tmp, sizeof tmp)) snprintf(e->title, sizeof e->title, "%.127s", tmp);

    const char *p = xml;
    while ((p = tag_open(p, "item")) && e->nman < MAXMAN) {
        char id[96], href[600], type[96], props[128];
        id[0] = href[0] = type[0] = props[0] = 0;
        tag_attr(p, "id", id, sizeof id);
        tag_attr(p, "href", href, sizeof href);
        tag_attr(p, "media-type", type, sizeof type);
        tag_attr(p, "properties", props, sizeof props);
        if (id[0] && href[0]) {
            Man *m = &e->man[e->nman];
            char full[700];
            resolve_path(e->dir, href, full, sizeof full);
            m->id = strdup(id); m->type = strdup(type); m->props = strdup(props); m->href = strdup(full);
            if (!m->id || !m->type || !m->props || !m->href) break;
            m->zip = zip_find(e, full);
            if (!strncmp(type, "image/", 6) && e->nimg < MAXIMG) e->img[e->nimg++] = e->nman;
            e->nman++;
        }
        p = strchr(p, '>');
        if (!p) break;
        p++;
    }

    const char *sp = tag_open(xml, "spine");
    char toc_id[96] = "";
    if (sp) tag_attr(sp, "toc", toc_id, sizeof toc_id);
    p = sp ? sp : xml;
    while ((p = tag_open(p, "itemref")) && e->nch < MAXSPINE) {
        char idref[96];
        if (tag_attr(p, "idref", idref, sizeof idref)) {
            int mi = man_by_id(e, idref);
            if (mi >= 0 && is_html(&e->man[mi])) e->spine[e->nch++] = mi;
        }
        p = strchr(p, '>');
        if (!p) break;
        p++;
    }
    if (!e->nch) { free(raw); return -1; }

    e->man2chap = malloc((size_t)e->nman * sizeof *e->man2chap);
    e->man2img = malloc((size_t)e->nman * sizeof *e->man2img);
    if (!e->man2chap || !e->man2img) { free(raw); return -1; }
    for (int i = 0; i < e->nman; i++) { e->man2chap[i] = -1; e->man2img[i] = -1; }
    for (int i = 0; i < e->nch; i++) e->man2chap[e->spine[i]] = i;
    for (int i = 0; i < e->nimg; i++) e->man2img[e->img[i]] = i;

    e->cover = -1;
    for (int i = 0; i < e->nman; i++)
        if (e->man2img[i] >= 0 && e->man[i].props && strstr(e->man[i].props, "cover-image"))
            e->cover = e->man2img[i];

    // EPUB 3 nav first, then EPUB 2 NCX.
    int navi = -1, ncx = toc_id[0] ? man_by_id(e, toc_id) : -1;
    for (int i = 0; i < e->nman; i++)
        if (e->man[i].props && strstr(e->man[i].props, "nav")) navi = i;
    int ti = navi >= 0 ? navi : ncx;
    if (ti >= 0 && e->man[ti].zip >= 0) {
        uint8_t *t = NULL; size_t tn = 0;
        if (!zip_extract(e, e->man[ti].zip, &t, &tn) && tn <= MAXXML) {
            char tdir[600];
            dirname_of(e->man[ti].href, tdir, sizeof tdir);
            if (navi >= 0) {
                const char *q = (const char *)t;
                while ((q = tag_open(q, "a"))) {
                    char href[600];
                    if (tag_attr(q, "href", href, sizeof href)) {
                        const char *gt = strchr(q, '>');
                        const char *close = gt ? strstr(gt + 1, "</a>") : NULL;
                        char label[128] = "";
                        if (gt && close) plain_text(gt + 1, (size_t)(close - gt - 1), label, sizeof label);
                        char full[800];
                        resolve_path(tdir, href, full, sizeof full);
                        int mi = man_by_path(e, full);
                        int ch = mi >= 0 ? e->man2chap[mi] : -1;
                        if (ch >= 0) add_toc(e, label, ch);
                    }
                    q = strchr(q, '>');
                    if (!q) break;
                    q++;
                }
            } else {
                const char *q = (const char *)t;
                char label[128] = "";
                while (*q) {
                    const char *a = tag_open(q, "text");
                    const char *b = tag_open(q, "content");
                    if (!a && !b) break;
                    if (a && (!b || a < b)) {
                        const char *gt = strchr(a, '>');
                        const char *close = gt ? strstr(gt + 1, "</text>") : NULL;
                        if (gt && close) plain_text(gt + 1, (size_t)(close - gt - 1), label, sizeof label);
                        q = close ? close + 7 : (a + 1);
                    } else {
                        char src[600] = "";
                        tag_attr(b, "src", src, sizeof src);
                        char full[800];
                        resolve_path(tdir, src, full, sizeof full);
                        int mi = man_by_path(e, full);
                        int ch = mi >= 0 ? e->man2chap[mi] : -1;
                        if (ch >= 0 && label[0]) add_toc(e, label, ch);
                        q = strchr(b, '>');
                        if (!q) break;
                        q++;
                    }
                }
            }
            free(t);
        }
    }

    e->chtitle = calloc(MAXSPINE, sizeof *e->chtitle);
    for (int i = 0; i < e->nch; i++) {
        for (int k = 0; k < e->ntoc; k++) {
            if (e->toc[k].chapter == i) { e->chtitle[i] = strdup(e->toc[k].title); break; }
        }
        if (!e->chtitle[i]) {
            char b[48];
            snprintf(b, sizeof b, "Chapter %d", i + 1);
            e->chtitle[i] = strdup(b);
        }
    }
    free(raw);
    return 0;
}

// ---- images ----------------------------------------------------------------

static int jpeg_info(const uint8_t *d, size_t n, int *w, int *h, int *comp) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 0;
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) { i++; continue; }
        while (i < n && d[i] == 0xFF) i++;
        if (i >= n) return 0;
        unsigned m = d[i++];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9 || m == 0xDA) return 0;
        if (i + 2 > n) return 0;
        unsigned len = ((unsigned)d[i] << 8) | d[i + 1];
        if (len < 2 || i + len > n) return 0;
        if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) ||
            (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF)) {
            if (len < 8) return 0;
            *h = (d[i + 3] << 8) | d[i + 4];
            *w = (d[i + 5] << 8) | d[i + 6];
            *comp = d[i + 7];
            return (*w > 0 && *h > 0) ? 1 : 0;
        }
        i += len;
    }
    return 0;
}

static int png_info(const uint8_t *d, size_t n, int *w, int *h, int *gray) {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n < 33 || memcmp(d, sig, 8)) return 0;
    if (memcmp(d + 12, "IHDR", 4)) return 0;
    *w = (int)be32(d + 16);
    *h = (int)be32(d + 20);
    int ct = d[25];
    *gray = (ct == 0 || ct == 4);
    return (*w > 0 && *h > 0) ? 1 : 0;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int epub_png_decode(const uint8_t *d, size_t n, int maxw, uint16_t **out, int *ow, int *oh) {
    *out = NULL; *ow = *oh = 0;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n < 33 || memcmp(d, sig, 8)) return -1;

    uint32_t w = 0, h = 0;
    int bd = 0, ct = 0, inter = -1;
    const uint8_t *plte = NULL, *trns = NULL;
    size_t plen = 0, tlen = 0;
    uint8_t *idat = NULL;
    size_t idlen = 0, idcap = 0;

    size_t p = 8;
    while (p + 12 <= n) {
        uint32_t len = be32(d + p);
        const uint8_t *type = d + p + 4;
        const uint8_t *data = d + p + 8;
        if ((size_t)len > n - p - 12) break;
        if (!memcmp(type, "IHDR", 4)) {
            if (len < 13) break;
            w = be32(data); h = be32(data + 4);
            bd = data[8]; ct = data[9]; inter = data[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            plte = data; plen = len;
        } else if (!memcmp(type, "tRNS", 4)) {
            trns = data; tlen = len;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idlen + len > (8u << 20)) { free(idat); return -1; }
            if (idlen + len > idcap) {
                size_t nc = idcap ? idcap * 2 : 65536;
                while (nc < idlen + len) nc *= 2;
                uint8_t *t = xrealloc(idat, nc);
                if (!t) { free(idat); return -1; }
                idat = t; idcap = nc;
            }
            memcpy(idat + idlen, data, len);
            idlen += len;
        } else if (!memcmp(type, "IEND", 4)) break;
        p += 12u + len;
    }
    if (!w || !h || w > 8192 || h > 16384 || bd != 8 || inter != 0) { free(idat); return -1; }
    int ch;
    switch (ct) {
        case 0: ch = 1; break;                 // gray
        case 2: ch = 3; break;                 // RGB
        case 3: ch = 1; break;                 // palette
        case 4: ch = 2; break;                 // gray + alpha
        case 6: ch = 4; break;                 // RGBA
        default: free(idat); return -1;
    }
    if (ct == 3 && (!plte || plen < 3)) { free(idat); return -1; }
    size_t stride = (size_t)w * (size_t)ch;
    size_t rawsz = (stride + 1) * h;
    if (rawsz > (8u << 20) || idlen < 6) { free(idat); return -1; }
    uint8_t *raw = malloc(rawsz);
    uint8_t *prev = calloc(stride, 1);
    if (!raw || !prev) { free(raw); free(prev); free(idat); return -1; }
    if (inflate_mem(idat + 2, idlen - 6, raw, rawsz)) {
        free(raw); free(prev); free(idat); return -1;
    }
    free(idat);

    int oww = (int)w > maxw ? maxw : (int)w;
    if (oww < 1) oww = 1;
    int ohh = (int)((long long)h * oww / w);
    if (ohh < 1) ohh = 1;
    if ((size_t)oww * (size_t)ohh * 2 > (8u << 20)) { free(raw); free(prev); return -1; }
    uint16_t *px = malloc((size_t)oww * ohh * 2);
    if (!px) { free(raw); free(prev); return -1; }

    int next = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1);
        int ft = row[0];
        row++;
        if (ft > 4) { free(raw); free(prev); free(px); return -1; }
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)ch ? row[i - ch] : 0;
            int b = prev[i];
            int c = i >= (size_t)ch ? prev[i - ch] : 0;
            int v = row[i];
            switch (ft) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) >> 1; break;
                case 4: v += paeth(a, b, c); break;
                default: break;
            }
            row[i] = (uint8_t)v;
        }
        while (next < ohh && (uint32_t)((long long)next * h / ohh) <= y) {
            uint16_t *o = px + (size_t)next * oww;
            for (int x = 0; x < oww; x++) {
                size_t sx = (size_t)x * w / (size_t)oww;
                int r, g, b;
                if (ct == 0) r = g = b = row[sx];
                else if (ct == 4) r = g = b = row[sx * 2];
                else if (ct == 2) { r = row[sx * 3]; g = row[sx * 3 + 1]; b = row[sx * 3 + 2]; }
                else if (ct == 6) { r = row[sx * 4]; g = row[sx * 4 + 1]; b = row[sx * 4 + 2]; }
                else {
                    size_t idx = row[sx];
                    if (idx * 3 + 2 < plen) { r = plte[idx * 3]; g = plte[idx * 3 + 1]; b = plte[idx * 3 + 2]; }
                    else r = g = b = 255;
                    if (trns && idx < tlen && trns[idx] < 128) r = g = b = 255;
                }
                o[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            }
            next++;
        }
        memcpy(prev, row, stride);
    }
    // Very small source images can leave rows unfilled when upscaling.
    while (next < ohh) {
        memcpy(px + (size_t)next * oww, px + (size_t)(next - 1) * oww, (size_t)oww * 2);
        next++;
    }
    free(raw);
    free(prev);
    *out = px; *ow = oww; *oh = ohh;
    return 0;
}

int epub_image_data(Epub *e, int id, uint8_t **buf, size_t *len) {
    if (!e || id < 0 || id >= e->nimg) return -1;
    int mi = e->img[id];
    if (mi < 0 || mi >= e->nman || e->man[mi].zip < 0) return -1;
    return zip_extract(e, e->man[mi].zip, buf, len);
}

int epub_image_probe(const uint8_t *data, size_t len, int *w, int *h, int *gray, int *type) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (gray) *gray = 0;
    if (type) *type = 0;
    if (!data) return 1;
    int comp = 0;
    if (jpeg_info(data, len, w, h, &comp)) {
        if (gray) *gray = comp == 1;
        if (type) *type = 1;
        return 0;
    }
    if (png_info(data, len, w, h, gray)) {
        if (type) *type = 2;
        return 0;
    }
    return 1;
}

int epub_image_info(Epub *e, int id, int *w, int *h, int *gray, int *type) {
    uint8_t *d = NULL;
    size_t n = 0;
    if (epub_image_data(e, id, &d, &n)) return -1;
    int r = epub_image_probe(d, n, w, h, gray, type);
    free(d);
    return r;               // 1 = entry exists but format is unsupported
}

// ---- public API ------------------------------------------------------------

Epub *epub_open(const char *path) {
    Epub *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->cur_ch = -1; e->cover = -1;
    if (zip_open(e, path)) { epub_close(e); return NULL; }
    char opf[700];
    if (load_container(e, opf, sizeof opf)) { epub_close(e); return NULL; }
    if (load_opf(e, opf)) { epub_close(e); return NULL; }
    return e;
}

void epub_close(Epub *e) {
    if (!e) return;
    ch_free(e);
    free(e->pbuf);
    if (e->ent) {
        for (int i = 0; i < e->nent; i++) free(e->ent[i].name);
        free(e->ent);
    }
    if (e->man) {
        for (int i = 0; i < e->nman; i++) {
            free(e->man[i].id); free(e->man[i].href);
            free(e->man[i].type); free(e->man[i].props);
        }
        free(e->man);
    }
    free(e->spine); free(e->man2chap); free(e->man2img); free(e->img);
    free(e->dir);
    if (e->toc) {
        for (int i = 0; i < e->ntoc; i++) free(e->toc[i].title);
        free(e->toc);
    }
    if (e->chtitle) {
        for (int i = 0; i < e->nch; i++) free(e->chtitle[i]);
        free(e->chtitle);
    }
    if (e->f) fclose(e->f);
    free(e);
}

const char *epub_title(const Epub *e) { return e ? e->title : ""; }
int epub_chapters(const Epub *e) { return e ? e->nch : 0; }
const char *epub_chapter_title(const Epub *e, int ch) {
    if (!e || ch < 0 || ch >= e->nch || !e->chtitle || !e->chtitle[ch]) return "";
    return e->chtitle[ch];
}

int epub_load_chapter(Epub *e, int ch) {
    if (!e || ch < 0 || ch >= e->nch) return -1;
    if (e->cur_ch == ch) return e->npages;
    ch_free(e);
    int mi = e->spine[ch];
    char dir[600];
    dirname_of(e->man[mi].href, dir, sizeof dir);
    int zi = e->man[mi].zip;
    int parsed = 0;
    if (zi >= 0) {
        uint8_t *raw = NULL; size_t n = 0;
        if (!zip_extract(e, zi, &raw, &n) && n <= MAXXML) {
            parse_xhtml(e, (const char *)raw, (int)n, dir);
            parsed = 1;
            free(raw);
        }
    }
    if (!e->npages) {
        if (!e->pbuf) { e->pbsize = MAXPARA; e->pbuf = malloc(e->pbsize); }
        if (e->pbuf) {
            e->plen = 0;
            const char *msg = e->man[mi].zip < 0 ? "This chapter is missing from the EPUB."
                            : parsed ? "This chapter is empty."
                                     : "This chapter could not be read.";
            for (const char *m = msg; *m; m++) ep_put(e, *m);
            ep_flush_para(e, 0);
        }
        if (!e->npages) {
            e->pagefirst = e->nlines;
            e->pagelines = 1;
            if (e->nlines == e->lcap) {
                EpubLine *l = xrealloc(e->lines, (size_t)(e->lcap + 16) * sizeof *l);
                if (l) { e->lines = l; e->lcap += 16; }
            }
            if (e->nlines < e->lcap) {
                e->lines[e->nlines].off = 0;
                e->lines[e->nlines].len = 0;
                e->lines[e->nlines].kind = EPUB_LINE_BLANK;
                e->nlines++;
            }
            e->pages = malloc(32 * sizeof *e->pages);
            if (e->pages) {
                e->pcap = 32;
                e->pages[0].first = e->pagefirst;
                e->pages[0].n = 1;
                e->pages[0].img = -1;
                e->npages = 1;
            }
        }
    }
    e->cur_ch = ch;
    return e->npages;
}

int epub_pages(const Epub *e) { return e ? e->npages : 0; }
const EpubPage *epub_page(const Epub *e, int pg) {
    if (!e || pg < 0 || pg >= e->npages) return NULL;
    return &e->pages[pg];
}
int epub_line_count(const Epub *e) { return e ? e->nlines : 0; }
const EpubLine *epub_line(const Epub *e, int i) {
    if (!e || i < 0 || i >= e->nlines) return NULL;
    return &e->lines[i];
}
const char *epub_text(const Epub *e) { return e && e->txt ? e->txt : ""; }

int epub_toc_count(const Epub *e) { return e ? e->ntoc : 0; }
const char *epub_toc_title(const Epub *e, int i) {
    if (!e || i < 0 || i >= e->ntoc) return "";
    return e->toc[i].title;
}
int epub_toc_chapter(const Epub *e, int i) {
    if (!e || i < 0 || i >= e->ntoc) return -1;
    return e->toc[i].chapter;
}

int epub_image_count(const Epub *e) { return e ? e->nimg : 0; }

int epub_cover(const Epub *e) {
    if (!e) return -1;
    if (e->cover >= 0) return e->cover;
    return e->nimg ? 0 : -1;
}

int epub_first_image(Epub *e, int ch) {
    if (!e || ch < 0 || ch >= e->nch) return -1;
    if (epub_load_chapter(e, ch) < 0) return -1;
    for (int i = 0; i < e->npages; i++)
        if (e->pages[i].img >= 0) return e->pages[i].img;
    return -1;
}
