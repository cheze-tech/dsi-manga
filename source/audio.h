// Background music: plays every .mp3 in /mp3 in shuffled order (a track never repeats
// until the whole folder has played). Built into the DSi edition by default (libmad + maxmod);
// `make dsi AUDIO=0` builds without music, and every function here then does nothing.
#pragma once
void audio_init(void);             // find /mp3 files and start playing (silently does nothing if none)
void audio_pump(int max_frames);   // call OFTEN: decodes a little MP3 and feeds the sound hardware
void audio_next(void);             // skip to the next shuffled track
int  audio_select(int i);          // play track i immediately; 0 if there is no such track
void audio_toggle(void);           // pause / resume
int  audio_paused(void);
const char *audio_title(void);     // file name of the current track, or NULL when there is no music
int  audio_count(void);            // number of detected tracks
const char *audio_name(int i);     // file name of track i, or NULL
int  audio_current_index(void);    // selected/playing index, or -1
int  audio_next_index(void);       // planned shuffled next index, or -1
const char *audio_next_title(void);// file name of the planned next track, or NULL
unsigned audio_track_serial(void); // increments whenever a new track actually opens
const char *audio_music_dir(void); // directory containing the detected tracks

// shuffle bag, exposed so it can be tested on a PC
void audio_shuffle_reset(int n);
int  audio_shuffle_next(void);
