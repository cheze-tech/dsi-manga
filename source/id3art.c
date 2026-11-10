// Minimal, bounded ID3v2.2/2.3/2.4 APIC/PIC extractor. It never trusts a
// declared frame/tag length beyond the actual file or the artwork cap.
#include "id3art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID3ART_MAX_TAG  (8u << 20)
#define ID3ART_MAX_ART  (256u << 10)

static unsigned be32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
static unsigned syncsafe(const unsigned char *p) {
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80) return 0xffffffffu;
    return ((unsigned)p[0] << 21) | ((unsigned)p[1] << 14) | ((unsigned)p[2] << 7) | p[3];
}

// ID3 unsynchronisation inserts 0x00 after 0xff. Undo it in place.
static size_t deunsync(unsigned char *p, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        p[o++] = p[i];
        if (p[i] == 0xff && i + 1 < n && p[i + 1] == 0x00) i++;
    }
    return o;
}

static int jpeg(const unsigned char *p, size_t n) {
    return n >= 4 && p[0] == 0xff && p[1] == 0xd8;
}

static int picture(const unsigned char *d, size_t n, int ver, Id3Art *art) {
    if (n < 4) return 0;
    size_t p = 0;
    int enc = d[p++];

    if (ver == 2) {
        if (p + 3 >= n) return 0;
        if (memcmp(d + p, "JPG", 3) && memcmp(d + p, "jpg", 3)) return 0;
        p += 3;
    } else {
        // MIME string, NUL terminated.
        while (p < n && d[p]) p++;
        if (p >= n) return 0;
        p++;
    }
    if (p >= n) return 0;
    p++;                                  // picture type

    // Description is Latin-1/UTF-8 for encodings 0/3 and UTF-16 for 1/2.
    if (enc == 1 || enc == 2) {
        while (p + 1 < n && !(d[p] == 0 && d[p + 1] == 0)) p += 2;
        if (p + 1 >= n) return 0;
        p += 2;
    } else {
        while (p < n && d[p]) p++;
        if (p >= n) return 0;
        p++;
    }
    if (p >= n) return 0;

    size_t len = n - p;
    if (len > ID3ART_MAX_ART || !jpeg(d + p, len)) return 0;
    unsigned char *copy = malloc(len);
    if (!copy) return 0;
    memcpy(copy, d + p, len);
    art->data = copy;
    art->len = len;
    return 1;
}

int id3art_load(const char *path, Id3Art *art) {
    if (art) memset(art, 0, sizeof *art);
    if (!path || !art) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long fs = ftell(f);
    if (fs < 10 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }

    unsigned char h[10];
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "ID3", 3)) {
        fclose(f);
        return 0;
    }
    int ver = h[3], flags = h[5];
    if (ver < 2 || ver > 4) { fclose(f); return 0; }
    unsigned tag = syncsafe(h + 6);
    if (!tag || tag > ID3ART_MAX_TAG) { fclose(f); return 0; }

    long end = 10 + (long)tag;
    if (end > fs) end = fs;
    long pos = 10;
    int found = 0;

    if (flags & 0x40) {                    // extended header
        unsigned char eh[6];
        if (ver == 2) {
            if (fread(eh, 1, 6, f) == 6) pos += 6;
        } else if (fread(eh, 1, 4, f) == 4) {
            unsigned sz = (ver == 4) ? syncsafe(eh) : be32(eh);
            if (ver == 4) pos += (long)sz;
            else pos += 4 + (long)sz;
        }
    }
    if (pos >= end) { fclose(f); return 0; }

    int hdr = ver == 2 ? 6 : 10;
    while (pos + hdr <= end) {
        unsigned char fh[10];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(fh, 1, hdr, f) != (size_t)hdr) break;
        char id[5] = { 0 };
        unsigned fsz;
        if (ver == 2) {
            memcpy(id, fh, 3);
            fsz = ((unsigned)fh[3] << 16) | ((unsigned)fh[4] << 8) | fh[5];
        } else {
            memcpy(id, fh, 4);
            fsz = (ver == 4) ? syncsafe(fh + 4) : be32(fh + 4);
        }
        // Reject zero, wrapped and absurd frame sizes before casting to long,
        // so a corrupt tag cannot make pos jump backwards or repeat.
        if (!fsz || fsz > ID3ART_MAX_TAG || pos + hdr + (long)fsz > end) break;

        int apic = (ver == 2) ? !memcmp(id, "PIC", 3) : !memcmp(id, "APIC", 4);
        if (apic) {
            if (fsz <= ID3ART_MAX_ART + 1024) {
                unsigned char *d = malloc(fsz);
                if (d) {
                    size_t got = fread(d, 1, fsz, f);
                    if (got == fsz) {
                        if (flags & 0x80) got = deunsync(d, got);
                        if (picture(d, got, ver, art)) found = 1;
                    }
                    free(d);
                }
            }
            if (found) break;
        }
        pos += hdr + (long)fsz;
    }
    fclose(f);
    return found;
}

void id3art_free(Id3Art *art) {
    if (!art) return;
    free(art->data);
    art->data = NULL;
    art->len = 0;
}
