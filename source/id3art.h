// Small ID3v2 APIC/PIC reader used by the DSi music menu.
// Only embedded JPEG artwork is accepted; everything else is reported as
// "no artwork" so the caller can draw its placeholder.
#pragma once
#include <stddef.h>

typedef struct {
    unsigned char *data;
    size_t len;
} Id3Art;

// Returns 1 when a JPEG picture was found, 0 when the file has no usable
// picture or malformed metadata. The caller owns art->data on success.
int id3art_load(const char *path, Id3Art *art);
void id3art_free(Id3Art *art);
