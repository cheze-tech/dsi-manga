// Host test: word wrap, pagination and UTF-8 clean-up of the text reader.
#define main enhanced_main
#include "../source/enhanced.c"
#undef main
#include <assert.h>
int main(void) {
    const char *src = "Chapter 1\r\n\r\n\xEF\xBB\xBF\xE2\x80\x9CHello,\xE2\x80\x9D she said \xE2\x80\x94 it\xE2\x80\x99s a long\tday\xE2\x80\xA6 "
        "Supercalifragilisticexpialidocious_and_then_some_more_letters_to_force_a_hard_cut ok fine.\n"
        "A line with exactly thirty-one chr\nAnother paragraph that keeps going and going so that it has to wrap over several screen lines properly.\n   indented line\n";
    char *t = strdup(src); size_t n = sanitize(t, strlen(src));
    printf("sanitized: %.*s|\n", (int)(n > 60 ? 60 : n), t);
    assert(!strstr(t, "\xE2") && !strchr(t, '\r') && !strchr(t, '\t'));
    // every wrapped line fits and no word is lost or reordered
    char words_in[2048] = "", words_out[2048] = "", line[COLS + 1];
    for (size_t i = 0, w = 0; i < n; i++) { if (t[i] != '\n' && t[i] != ' ') { size_t l = strlen(words_in); words_in[l] = t[i]; words_in[l+1] = 0; } else if (strlen(words_in) && words_in[strlen(words_in)-1] != '|') strcat(words_in, "|"); (void)w; }
    size_t pos = 0; int lines = 0, maxlen = 0;
    while (pos < n) {
        size_t nx = wrap_line(t, n, pos, line);
        assert(nx > pos);                                   // always progresses
        int l = (int)strlen(line); if (l > maxlen) maxlen = l; assert(l <= COLS);
        for (int i = 0; i < l; i++) { if (line[i] != ' ') { size_t k = strlen(words_out); words_out[k] = line[i]; words_out[k+1] = 0; } else if (strlen(words_out) && words_out[strlen(words_out)-1] != '|') strcat(words_out, "|"); }
        // a wrapped line boundary counts as a word boundary unless a huge word was cut
        lines++; pos = nx;
        if (pos < n) { size_t k = strlen(words_out); if (k && words_out[k-1] != '|') { /* hard cut keeps letters joined */ } }
        if (pos < n && strlen(words_out) && words_out[strlen(words_out)-1] != '|') strcat(words_out, "~");
    }
    // compare ignoring the '|' / '~' separators
    char a[2048] = "", b[2048] = "";
    for (char *p = words_in; *p; p++) if (*p != '|') strncat(a, p, 1);
    for (char *p = words_out; *p; p++) if (*p != '|' && *p != '~') strncat(b, p, 1);
    assert(!strcmp(a, b));
    printf("lines=%d longest=%d  text preserved: yes\n", lines, maxlen);
    pos = 0; for (int i = 0; i < 12 && pos < n; i++) { pos = wrap_line(t, n, pos, line); printf("  [%-31s]\n", line); }
    return 0;
}
