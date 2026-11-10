// Background music: plays every .mp3 in /mp3 in shuffled order (a track never repeats
// until the whole folder has played). Built into the DSi edition by default (libmad + maxmod);
// `make dsi AUDIO=0` builds without music, and every function here then does nothing.
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
