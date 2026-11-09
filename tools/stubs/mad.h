#pragma once
#include <stddef.h>
typedef int mad_fixed_t;
#define MAD_F_FRACBITS 28
#define MAD_F_ONE (1 << 28)
#define MAD_BUFFER_GUARD 8
enum mad_error { MAD_ERROR_NONE = 0, MAD_ERROR_BUFLEN = 1, MAD_ERROR_LOSTSYNC = 0x101 };
#define MAD_RECOVERABLE(e) ((e) & 0xff00)
struct mad_stream { const unsigned char *buffer, *bufend, *next_frame; int error; };
struct mad_header { unsigned samplerate; };
struct mad_frame { struct mad_header header; };
struct mad_pcm { unsigned samplerate; unsigned short channels, length; mad_fixed_t samples[2][1152]; };
struct mad_synth { struct mad_pcm pcm; };
void mad_stream_init(struct mad_stream*); void mad_stream_finish(struct mad_stream*); void mad_stream_buffer(struct mad_stream*, const unsigned char*, unsigned long);
void mad_frame_init(struct mad_frame*); void mad_frame_finish(struct mad_frame*); int mad_frame_decode(struct mad_frame*, struct mad_stream*);
void mad_synth_init(struct mad_synth*); void mad_synth_finish(struct mad_synth*); void mad_synth_frame(struct mad_synth*, struct mad_frame const*);
