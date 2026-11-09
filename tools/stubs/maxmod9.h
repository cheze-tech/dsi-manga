#pragma once
#include <stdint.h>
typedef unsigned int mm_word; typedef void *mm_addr; typedef int mm_stream_formats;
typedef mm_word (*mm_stream_func)(mm_word, mm_addr, mm_stream_formats);
typedef struct { mm_word sampling_rate, buffer_length; mm_stream_func callback; mm_word format, timer; int manual; } mm_stream;
typedef struct { mm_word mod_count, samp_count; mm_addr mem_bank; mm_word fifo_channel; } mm_ds_system;
enum { MM_STREAM_16BIT_STEREO = 3, MM_TIMER0 = 0, FIFO_MAXMOD = 4 };
void mmInit(mm_ds_system*); void mmStreamOpen(mm_stream*); void mmStreamUpdate(void); void mmStreamClose(void);
