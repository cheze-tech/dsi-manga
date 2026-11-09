// Host-side test: prints the page count and each page's JPEG byte range.
#include "../source/pdf.h"
int main(int argc, char **argv) {
    Pdf p;
    if (argc < 2 || pdf_open(&p, argv[1])) { puts("open failed"); return 1; }
    printf("%d\n", p.npages);
    for (int i = 0; i < p.npages; i++) printf("%u %u\n", p.pages[i].off, p.pages[i].len);
    pdf_close(&p);
    return 0;
}
