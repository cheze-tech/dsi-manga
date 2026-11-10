// Host test of the music logic (ring buffer, track chaining, shuffle, skip, pause) using
// FAKE libmad/maxmod. It checks my code, not the real libraries.
//   gcc -DUSE_AUDIO -DMP3_DIR='"/tmp/simmp3/"' -Itools/stubs -Isource tools/audio_sim.c source/audio.c -o audio_sim
#include "audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "mad.h"
#include "maxmod9.h"
#include <stdint.h>

#define FRAME_BYTES 417
// ---- fake libmad: a file is N 417-byte "frames"; byte 1 selects the sample rate ----
void mad_stream_init(struct mad_stream *s) { memset(s, 0, sizeof *s); }
void mad_stream_finish(struct mad_stream *s) { (void)s; }
void mad_stream_buffer(struct mad_stream *s, const unsigned char *b, unsigned long n) { s->buffer = b; s->bufend = b + n; s->next_frame = b; }
void mad_frame_init(struct mad_frame *f) { memset(f, 0, sizeof *f); }
void mad_frame_finish(struct mad_frame *f) { (void)f; }
int mad_frame_decode(struct mad_frame *f, struct mad_stream *s) {
    if (s->bufend - s->next_frame < FRAME_BYTES) { s->error = MAD_ERROR_BUFLEN; return -1; }
    f->header.samplerate = s->next_frame[1] ? 32000 : 44100;
    s->next_frame += FRAME_BYTES;
    return 0;
}
void mad_synth_init(struct mad_synth *y) { memset(y, 0, sizeof *y); }
void mad_synth_finish(struct mad_synth *y) { (void)y; }
void mad_synth_frame(struct mad_synth *y, const struct mad_frame *f) {
    y->pcm.samplerate = f->header.samplerate; y->pcm.channels = 2; y->pcm.length = 1152;
    for (int i = 0; i < 1152; i++) { y->pcm.samples[0][i] = 1 << 22; y->pcm.samples[1][i] = 1 << 22; }
}
// ---- fake maxmod: a "sound card" that asks for samples when the harness says time passed ----
static mm_stream *S; static long sim_need; static long played, silent; static int opens;
void mmInit(mm_ds_system *s) { (void)s; }
void mmStreamOpen(mm_stream *s) { S = s; opens++; }
void mmStreamClose(void) { S = NULL; }
void mmStreamUpdate(void) {
    static int16_t buf[4000 * 2];
    if (!S || sim_need <= 0) return;
    long n = sim_need; if (n > 4000) n = 4000;
    S->callback((mm_word)n, buf, 0);
    for (long i = 0; i < n; i++) { if (buf[2 * i] == 0) silent++; else played++; }
    sim_need = 0;
}
static void tick(void) { sim_need = S ? (long)S->sampling_rate / 60 : 0; audio_pump(4); }
static void mkfile(const char *name, int frames, int rate_code) {
    char p[200]; snprintf(p, sizeof p, "/tmp/simmp3/%s", name);
    FILE *f = fopen(p, "wb");
    for (int i = 0; i < frames; i++) { unsigned char fr[FRAME_BYTES] = { 0xFF, (unsigned char)rate_code }; fwrite(fr, 1, FRAME_BYTES, f); }
    fclose(f);
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "empty")) {
        mkdir(MP3_DIR, 0777);
        audio_init();
        assert(audio_count() == 0);
        assert(audio_title() == NULL && audio_next_title() == NULL);
        assert(audio_name(0) == NULL && audio_current_index() == -1);
        assert(!audio_select(0) && audio_next_index() == -1);
        audio_toggle(); audio_next(); audio_pump(4);
        puts("empty folder: OK");
        return 0;
    }
    mkdir("/tmp/simmp3", 0777);
    mkfile("a.mp3", 30, 0); mkfile("b.MP3", 30, 0); mkfile("c.mp3", 30, 1);
    FILE *f = fopen("/tmp/simmp3/notes.txt", "w"); fputs("ignore me", f); fclose(f);
    audio_init();
    assert(audio_title() != NULL);

    // 1) shuffle: log the track order over a minute of simulated time
    const char *order[500]; int no = 0; const char *last = NULL; long silent_after_warm = 0;
    for (int t = 0; t < 3600; t++) {
        long s0 = silent; tick();
        if (t > 30) silent_after_warm += silent - s0;
        if (audio_title() != last) { last = audio_title(); order[no++] = last; if (no >= 500) break; }
    }
    printf("track changes in 60 s: %d, first nine: ", no);
    for (int i = 0; i < 9 && i < no; i++) printf("%s ", order[i]);
    printf("\n");
    for (int i = 0; i + 2 < no; i += 3) {                // each round of 3 = a permutation of the 3 files
        assert(strcmp(order[i], order[i + 1]) && strcmp(order[i], order[i + 2]) && strcmp(order[i + 1], order[i + 2]));
        if (i + 3 < no) assert(strcmp(order[i + 2], order[i + 3]));          // no immediate repeat across rounds
    }
    printf("shuffle: every file once per round, no back-to-back repeats: OK\n");
    printf("played %ld samples, silent %ld (%.2f%%), silent after warm-up %ld, stream (re)opens %d\n",
           played, silent, 100.0 * silent / (played + silent), silent_after_warm, opens);
    assert(played > 2000000 && silent_after_warm == 0);

    // 2) skip changes track immediately, pause stops consuming and resumes
    const char *before = audio_title(); audio_next();
    printf("skip: %s -> %s\n", before, audio_title()); assert(audio_title() != before);
    for (int t = 0; t < 10; t++) tick();
    long p0 = played; audio_toggle(); assert(audio_paused());
    for (int t = 0; t < 60; t++) tick();
    long during = played - p0;
    audio_toggle(); for (int t = 0; t < 30; t++) tick();
    printf("paused: %ld real samples during 1 s of pause (expect 0); after resume: %ld\n", during, played - p0 - during);
    assert(during == 0 && played - p0 > 0);

    // 3) playlist state, manual selection, and the planned next track
    int count = audio_count();
    assert(count == 3);
    assert(audio_name(0) && audio_name(1) && audio_name(2));
    assert(audio_name(-1) == NULL && audio_name(3) == NULL);
    int cur = audio_current_index();
    assert(cur >= 0 && cur < count);
    int nxt = audio_next_index();
    assert(nxt >= 0 && nxt < count && nxt != cur);
    assert(audio_next_title() && strcmp(audio_next_title(), audio_title()) != 0);

    int pick = (cur + 1) % count;
    assert(audio_select(pick));
    assert(audio_current_index() == pick);
    assert(strcmp(audio_title(), audio_name(pick)) == 0);
    assert(audio_next_index() >= 0 && audio_next_index() != pick);
    assert(audio_next_title() && strcmp(audio_next_title(), audio_title()) != 0);
    audio_toggle(); assert(audio_paused());
    assert(audio_select(pick)); assert(!audio_paused());

    const char *planned = audio_next_title();
    int planned_i = audio_next_index();
    audio_next();
    assert(audio_current_index() == planned_i);
    assert(strcmp(audio_title(), planned) == 0);
    assert(audio_next_index() >= 0 && audio_next_index() != planned_i);
    printf("manual select/next: %s -> %s -> %s\n", audio_name(pick), planned, audio_next_title());
    for (int i = 0; i < count; i++) {              // every manual choice must not repeat immediately
        assert(audio_select(i));
        assert(audio_current_index() == i);
        assert(audio_next_index() >= 0 && audio_next_index() != i);
    }
    assert(!audio_select(-1) && !audio_select(count));
    puts("audio logic OK");
    return 0;
}
