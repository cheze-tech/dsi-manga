// Host test for the bounded ID3 APIC/PIC reader.
//   gcc -O1 -o id3art_test tools/id3art_test.c source/id3art.c
#include "id3art.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, unsigned v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void syncsafe(unsigned char *p, unsigned v) {
    p[0] = (v >> 21) & 0x7f; p[1] = (v >> 14) & 0x7f;
    p[2] = (v >> 7) & 0x7f; p[3] = v & 0x7f;
}
static void write_file(const char *path, const unsigned char *tag, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(tag, 1, n, f) == n);
    fclose(f);
}
static void make23(const char *path, const unsigned char *jpg, size_t jn, int broken) {
    size_t dlen = 1 + 11 + 1 + 1 + 1 + jn;      // enc, mime\0, type, desc\0, image
    unsigned char d[2048]; size_t p = 0;
    d[p++] = 0;
    memcpy(d + p, "image/jpeg", 11); p += 11;
    d[p++] = 0;                                // MIME terminator
    d[p++] = 3;                                // front cover
    d[p++] = 0;                                // empty description
    memcpy(d + p, jpg, jn); p += jn;
    unsigned char tag[2300]; size_t t = 0;
    memcpy(tag, "ID3", 3); tag[3] = 3; tag[4] = 0; tag[5] = 0; t = 10;
    syncsafe(tag + 6, (unsigned)(10 + dlen));
    memcpy(tag + t, "APIC", 4); put32(tag + t + 4, (unsigned)(broken ? 0x1000 : dlen));
    tag[t + 8] = tag[t + 9] = 0; t += 10;
    memcpy(tag + t, d, dlen); t += dlen;
    write_file(path, tag, t);
}
static void make24(const char *path, const unsigned char *jpg, size_t jn) {
    size_t dlen = 1 + 11 + 1 + 1 + 1 + jn;
    unsigned char d[2048]; size_t p = 0;
    d[p++] = 0; memcpy(d + p, "image/jpeg", 11); p += 11; d[p++] = 0; d[p++] = 3; d[p++] = 0;
    memcpy(d + p, jpg, jn); p += jn;
    unsigned char tag[2300]; size_t t = 0;
    memcpy(tag, "ID3", 3); tag[3] = 4; tag[4] = 0; tag[5] = 0; t = 10;
    syncsafe(tag + 6, (unsigned)(10 + dlen));
    memcpy(tag + t, "APIC", 4); syncsafe(tag + t + 4, (unsigned)dlen);
    tag[t + 8] = tag[t + 9] = 0; t += 10;
    memcpy(tag + t, d, dlen); t += dlen;
    write_file(path, tag, t);
}
static void make22(const char *path, const unsigned char *jpg, size_t jn) {
    size_t dlen = 1 + 3 + 1 + 1 + jn;
    unsigned char d[2048]; size_t p = 0;
    d[p++] = 0; memcpy(d + p, "JPG", 3); p += 3; d[p++] = 3; d[p++] = 0;
    memcpy(d + p, jpg, jn); p += jn;
    unsigned char tag[2300]; size_t t = 10;
    memcpy(tag, "ID3", 3); tag[3] = 2; tag[4] = 0; tag[5] = 0;
    syncsafe(tag + 6, (unsigned)(6 + dlen));
    memcpy(tag + t, "PIC", 3); tag[t + 3] = dlen >> 16; tag[t + 4] = dlen >> 8; tag[t + 5] = dlen; t += 6;
    memcpy(tag + t, d, dlen); t += dlen;
    write_file(path, tag, t);
}
int main(void) {
    const unsigned char jpg[] = { 0xff, 0xd8, 0xff, 0xd9 };
    Id3Art a;
    make23("/tmp/id3art23.mp3", jpg, sizeof jpg, 0);
    assert(id3art_load("/tmp/id3art23.mp3", &a) == 1);
    assert(a.len == sizeof jpg && !memcmp(a.data, jpg, sizeof jpg));
    id3art_free(&a); assert(!a.data);
    make24("/tmp/id3art24.mp3", jpg, sizeof jpg);
    assert(id3art_load("/tmp/id3art24.mp3", &a) == 1 && a.len == sizeof jpg);
    id3art_free(&a);
    make22("/tmp/id3art22.mp3", jpg, sizeof jpg);
    assert(id3art_load("/tmp/id3art22.mp3", &a) == 1 && a.len == sizeof jpg);
    id3art_free(&a);
    make23("/tmp/id3artbad.mp3", jpg, sizeof jpg, 1);
    assert(id3art_load("/tmp/id3artbad.mp3", &a) == 0 && a.data == NULL);
    const unsigned char noart[] = { 'I','D','3',3,0,0,0,0,0,0 };
    write_file("/tmp/id3artnone.mp3", noart, sizeof noart);
    assert(id3art_load("/tmp/id3artnone.mp3", &a) == 0 && a.data == NULL);
    assert(id3art_load("/tmp/id3artmissing.mp3", &a) == 0 && a.data == NULL);
    remove("/tmp/id3art23.mp3"); remove("/tmp/id3art24.mp3"); remove("/tmp/id3art22.mp3");
    remove("/tmp/id3artbad.mp3"); remove("/tmp/id3artnone.mp3");
    puts("id3 artwork: OK");
    return 0;
}
