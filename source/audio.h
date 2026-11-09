// Background music: plays every .mp3 in /mp3 in shuffled order (a track never repeats
// until the whole folder has played). Needs libmad + maxmod; build with `make dsi AUDIO=1`.
// Without AUDIO=1 every function here does nothing.
#pragma once
void audio_init(void);             // find /mp3 files and start playing (silently does nothing if none)
void audio_pump(int max_frames);   // call OFTEN: decodes a little MP3 and feeds the sound hardware
void audio_next(void);             // skip to the next shuffled track
void audio_toggle(void);           // pause / resume
int  audio_paused(void);
const char *audio_title(void);     // file name of the current track, or NULL when there is no music

// shuffle bag, exposed so it can be tested on a PC
void audio_shuffle_reset(int n);
int  audio_shuffle_next(void);
