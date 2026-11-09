#pragma once
#include <stdint.h>
#include <stddef.h>
#ifndef JD_FORMAT
#define JD_FORMAT 1
#endif
#define JD_USE_SCALE 1
typedef enum { JDR_OK, JDR_INTR, JDR_INP, JDR_FMT3 } JRESULT;
typedef struct { uint16_t left, right, top, bottom; } JRECT;
typedef struct JDEC { uint16_t width, height; uint8_t ncomp; void *device; } JDEC;
JRESULT jd_prepare(JDEC*, size_t(*)(JDEC*,uint8_t*,size_t), void*, size_t, void*);
JRESULT jd_decomp(JDEC*, int(*)(JDEC*,void*,JRECT*), uint8_t);
