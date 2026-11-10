#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>

#ifndef MP3_DIR
#define MP3_DIR "/mp3/"
#endif

// ---- shuffle bag: every track once per round, never the same track twice in a row ----
static int *bag, bag_n, bag_pos, last_track = -1;

void audio_shuffle_reset(int n) {
    free(bag);
    bag = n > 0 ? malloc(n * sizeof *bag) : NULL;
    bag_n = bag ? n : 0;
    bag_pos = bag_n;            // forces a fresh shuffle on the first call
    last_track = -1;
}

int audio_shuffle_next(void) {
    if (bag_n <= 0) return -1;
    if (bag_pos >= bag_n) {
        for (int i = 0; i < bag_n; i++) bag[i] = i;
        for (int i = bag_n - 1; i > 0; i--) { int j = rand() % (i + 1), t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
        if (bag_n > 1 && bag[0] == last_track) { int t = bag[0]; bag[0] = bag[1]; bag[1] = t; }
        bag_pos = 0;
    }
    return last_track = bag[bag_pos++];
}

// Pick the next shuffled track, but never pick `avoid` again immediately.
// This matters after a manual selection: the current track may still be sitting
// at the front of the shuffle bag.
static int shuffle_next_avoid(int avoid) {
    int i = audio_shuffle_next();
    if (i != avoid || bag_n <= 1 || bag_pos <= 0) return i;
    for (int k = 0; k < bag_n; k++) {
        if (bag[k] != avoid) {
            int t = bag[bag_pos - 1];
            bag[bag_pos - 1] = bag[k];
            bag[k] = t;
            i = bag[bag_pos - 1];
            break;
        }
    }
    last_track = i;
    return i;
}

#ifdef USE_AUDIO
#include <nds.h>
#include <maxmod9.h>
#include <mad.h>

// Older libnds exposed FIFO_MAXMOD; current libnds documents the maxmod FIFO channel as 7.
#ifndef FIFO_MAXMOD
#define FIFO_MAXMOD 7
#endif

#define RING 65536              // stereo frames of decoded audio waiting to be played (~1.5 s)

static s16 *ring;
static unsigned rd, wr, cnt;
static char **names;
static unsigned char *badmap;       // files that opened but never produced a frame
static int nnames, ready, paused, cur_rate, stream_open;
static int cur_track = -1, next_track = -1;
static int frames_for_track, serial_pending;
static unsigned track_serial;
static const char *title;
static FILE *mp;
static struct mad_stream ms;
static struct mad_frame mf;
static struct mad_synth msy;
static unsigned char inbuf[8192 + MAD_BUFFER_GUARD];
static mm_stream strm;

// maxmod asks for samples here (we call mmStreamUpdate ourselves, so it runs in the main thread)
static mm_word stream_cb(mm_word length, mm_addr dest, mm_stream_formats format) {
    (void)format;
    s16 *d = dest;
    for (mm_word i = 0; i < length; i++) {
        if (!paused && cnt) { *d++ = ring[rd * 2]; *d++ = ring[rd * 2 + 1]; rd = (rd + 1) & (RING - 1); cnt--; }
        else { *d++ = 0; *d++ = 0; }
    }
    return length;
}

static void open_stream(int rate) {
    if (stream_open) mmStreamClose();
    rd = wr = cnt = 0;
    strm.sampling_rate = rate;
    strm.buffer_length = 4096;
    strm.callback = stream_cb;
    strm.format = MM_STREAM_16BIT_STEREO;
    strm.timer = MM_TIMER0;
    strm.manual = 1;
    mmStreamOpen(&strm);
    stream_open = 1;
    cur_rate = rate;
}

static void close_file(void) {
    if (!mp) return;
    mad_synth_finish(&msy); mad_frame_finish(&mf); mad_stream_finish(&ms);
    fclose(mp);
    mp = NULL;
}

static int open_track(int i) {
    if (i < 0 || i >= nnames) return 0;
    char p[300];
    snprintf(p, sizeof p, MP3_DIR "%s", names[i]);
    if (!(mp = fopen(p, "rb"))) return 0;
    mad_stream_init(&ms); mad_frame_init(&mf); mad_synth_init(&msy);
    title = names[i];
    cur_track = i;
    frames_for_track = 0;
    serial_pending = 1;             // count the track only once a frame decodes
    return 1;
}

static void plan_next(void) {
    next_track = shuffle_next_avoid(cur_track);
}

static int open_next(void) {
    for (int tries = 0; tries < nnames; tries++) {
        int i = next_track;
        next_track = -1;
        if (i < 0) i = audio_shuffle_next();
        if (i < 0) return 0;
        if (badmap && badmap[i]) continue;   // already known to produce no audio
        if (open_track(i)) {
            plan_next();
            return 1;
        }
    }
    return 0;
}

static inline s16 to16(mad_fixed_t s) {
    s += 1L << (MAD_F_FRACBITS - 16);
    if (s >= MAD_F_ONE) s = MAD_F_ONE - 1; else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
    return (s16)(s >> (MAD_F_FRACBITS + 1 - 16));
}

// decode one MP3 frame into the ring. 1 = ok, 0 = end of file / unusable
static int decode_frame(void) {
    for (;;) {
        if (!ms.buffer || ms.error == MAD_ERROR_BUFLEN) {              // need more file data
            size_t keep = 0;
            if (ms.next_frame) { keep = ms.bufend - ms.next_frame; memmove(inbuf, ms.next_frame, keep); }
            size_t got = fread(inbuf + keep, 1, sizeof inbuf - MAD_BUFFER_GUARD - keep, mp);
            if (!got) return 0;
            memset(inbuf + keep + got, 0, MAD_BUFFER_GUARD);
            mad_stream_buffer(&ms, inbuf, keep + got);
            ms.error = MAD_ERROR_NONE;
        }
        if (mad_frame_decode(&mf, &ms)) {
            if (MAD_RECOVERABLE(ms.error) || ms.error == MAD_ERROR_BUFLEN) continue;
            return 0;
        }
        mad_synth_frame(&msy, &mf);
        if (serial_pending) { track_serial++; serial_pending = 0; }
        frames_for_track++;
        int rate = (int)msy.pcm.samplerate, ch = msy.pcm.channels;
        if (rate != cur_rate) open_stream(rate);
        for (unsigned i = 0; i < msy.pcm.length; i++) {
            s16 l = to16(msy.pcm.samples[0][i]), r = ch > 1 ? to16(msy.pcm.samples[1][i]) : l;
            ring[wr * 2] = l; ring[wr * 2 + 1] = r;
            wr = (wr + 1) & (RING - 1);
        }
        cnt += msy.pcm.length;
        return 1;
    }
}

void audio_pump(int max_frames) {
    if (!ready) return;
    if (stream_open) mmStreamUpdate();
    if (paused) return;
    while (max_frames-- > 0 && RING - cnt >= 1152) {                    // room for a whole frame
        if (!mp && !open_next()) { ready = 0; return; }                 // nothing playable: give up quietly
        if (!decode_frame()) {
            if (cur_track >= 0 && frames_for_track == 0 && badmap) badmap[cur_track] = 1;
            close_file();                                              // next call starts the next track
        }
    }
}

void audio_next(void) {
    if (!ready) return;
    close_file();
    rd = wr = cnt = 0;
    paused = 0;
    audio_pump(2);
}

int audio_select(int i) {
    if (!ready || i < 0 || i >= nnames) return 0;
    close_file();
    rd = wr = cnt = 0;
    paused = 0;
    cur_track = -1;
    next_track = -1;
    title = NULL;
    if (!open_track(i)) { audio_pump(2); return 0; }
    last_track = i;
    plan_next();
    audio_pump(2);
    return 1;
}

void audio_toggle(void) { if (ready) paused ^= 1; }
int audio_paused(void) { return paused; }
const char *audio_title(void) { return ready ? title : NULL; }
int audio_count(void) { return nnames; }
const char *audio_name(int i) { return i >= 0 && i < nnames ? names[i] : NULL; }
int audio_current_index(void) { return ready ? cur_track : -1; }
int audio_next_index(void) { return ready && next_track >= 0 ? next_track : -1; }
const char *audio_next_title(void) { int i = audio_next_index(); return i >= 0 ? names[i] : NULL; }
unsigned audio_track_serial(void) { return ready ? track_serial : 0; }

void audio_init(void) {
    DIR *d = opendir(MP3_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) && nnames < 512) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".mp3")) {
            char **nn = realloc(names, (nnames + 1) * sizeof *names);
            if (!nn) break;
            names = nn;
            if (!(names[nnames] = strdup(e->d_name))) break;
            nnames++;
        }
    }
    if (d) closedir(d);
    if (!nnames || !(ring = malloc(RING * 4))) return;
    badmap = calloc(nnames, 1);
    mm_ds_system sys;
    memset(&sys, 0, sizeof sys);
    sys.fifo_channel = FIFO_MAXMOD;
    mmInit(&sys);
    srand((unsigned)time(NULL));
    audio_shuffle_reset(nnames);
    cur_track = next_track = -1;
    title = NULL;
    ready = 1;
    audio_pump(2);
}

#else   // built without AUDIO=1: no music, everything is a no-op
#warning "audio.c built without USE_AUDIO; MP3 playback is disabled"
void audio_init(void) {}
void audio_pump(int max_frames) { (void)max_frames; }
void audio_next(void) {}
int audio_select(int i) { (void)i; return 0; }
void audio_toggle(void) {}
int audio_paused(void) { return 0; }
const char *audio_title(void) { return NULL; }
int audio_count(void) { return 0; }
const char *audio_name(int i) { (void)i; return NULL; }
int audio_current_index(void) { return -1; }
int audio_next_index(void) { return -1; }
const char *audio_next_title(void) { return NULL; }
unsigned audio_track_serial(void) { return 0; }
#endif
