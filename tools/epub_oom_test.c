// Host test: fail each allocation in turn while opening/reading an EPUB.
//
//   gcc -O1 -g -fsanitize=address,undefined -Isource -Wl,--wrap=malloc \
//       -Wl,--wrap=calloc -Wl,--wrap=realloc -o epub_oom_test \
//       tools/epub_oom_test.c source/epub.c && ./epub_oom_test /tmp/epub_fixtures
//
// strdup() inside libc is not wrapped, but every direct malloc/calloc/realloc in
// epub.c is.  LeakSanitizer reports anything a failed open forgot to free.
#include "epub.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);

static long budget = -1;

static int fail_now(void) {
    if (budget < 0) return 0;
    if (budget == 0) return 1;
    budget--;
    return 0;
}
void *__wrap_malloc(size_t n) { return fail_now() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t a, size_t b) { return fail_now() ? NULL : __real_calloc(a, b); }
void *__wrap_realloc(void *p, size_t n) { return fail_now() ? NULL : __real_realloc(p, n); }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/epub_fixtures";
    char path[512];
    snprintf(path, sizeof path, "%s/good.epub", dir);

    for (int fail = 0; fail < 500; fail++) {
        budget = fail;
        Epub *e = epub_open(path);
        if (e) {
            if (epub_chapters(e) > 0) {
                epub_load_chapter(e, 0);
                int id = epub_cover(e);
                if (id >= 0) {
                    uint8_t *d = NULL;
                    size_t n = 0;
                    if (!epub_image_data(e, id, &d, &n)) free(d);
                }
            }
            epub_close(e);
        }
    }
    budget = -1;
    Epub *e = epub_open(path);
    assert(e && epub_chapters(e) == 2);
    epub_close(e);
    puts("epub oom test: OK");
    return 0;
}
