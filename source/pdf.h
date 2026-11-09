// Minimal PDF page-image locator for the DSi manga reader.
// Finds, for each page, the byte range of its embedded DCT (JPEG) image.
#pragma once
#include <stdio.h>
#include <stdint.h>

typedef struct { uint32_t off, len; } PdfPage;   // JPEG bytes inside the file

typedef struct {
    FILE     *f;
    uint32_t  size;
    uint32_t *obj;  int nobj;      // object number -> file offset (0 = unknown)
    PdfPage  *pages; int npages, cap;
    int n_obj, n_dct, n_flate, n_jpx, n_other;   // filled only when no page was found
} Pdf;

int  pdf_open(Pdf *p, const char *path);   // 0 = ok
void pdf_close(Pdf *p);
void pdf_diag(const Pdf *p, char *out, size_t n);   // short text: what the images in this PDF are
