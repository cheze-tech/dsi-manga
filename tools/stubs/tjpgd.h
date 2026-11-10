#pragma once
#include <stdint.h>
#include <stddef.h>
#ifndef JD_FORMAT
#define JD_FORMAT 1
#endif
#define JD_USE_SCALE 1
typedef enum { JDR_OK, JDR_INTR, JDR_INP, JDR_FMT3, JDR_MORE } JRESULT;
typedef struct { uint16_t left, right, top, bottom; } JRECT;
typedef struct JDEC { uint16_t width, height; uint16_t x, y, rst, rsc; uint8_t ncomp, started; void *device; } JDEC;
JRESULT jd_prepare(JDEC*, size_t(*)(JDEC*,uint8_t*,size_t), void*, size_t, void*);
JRESULT jd_decomp(JDEC*, int(*)(JDEC*,void*,JRECT*), uint8_t);
JRESULT jd_decomp_begin(JDEC*, int(*)(JDEC*,void*,JRECT*), uint8_t);
JRESULT jd_decomp_step(JDEC*, int(*)(JDEC*,void*,JRECT*), unsigned int);
