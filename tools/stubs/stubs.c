#include "nds.h"
#include "tjpgd.h"
#include "fat.h"
JRESULT jd_prepare(JDEC*a, size_t(*b)(JDEC*,uint8_t*,size_t), void*c, size_t d, void*e){return JDR_FMT3;}
JRESULT jd_decomp(JDEC*a, int(*b)(JDEC*,void*,JRECT*), uint8_t c){return JDR_FMT3;}
JRESULT jd_decomp_begin(JDEC*a, int(*b)(JDEC*,void*,JRECT*), uint8_t c){return JDR_FMT3;}
JRESULT jd_decomp_step(JDEC*a, int(*b)(JDEC*,void*,JRECT*), unsigned int d){return JDR_FMT3;}
void scanKeys(void){} u32 keysDown(void){return 0;} u32 keysHeld(void){return 0;} void touchRead(touchPosition*t){}
void consoleDemoInit(void){} void consoleClear(void){} int fatInitDefault(void){return 1;}
void swiWaitForVBlank(void){} void videoSetMode(int a){} void videoSetModeSub(int a){}
void vramSetPrimaryBanks(int a,int b,int c,int d){} void vramSetBankA(int a){}
int bgInit(int a,int b,int c,int d,int e){return 0;} int bgInitSub(int a,int b,int c,int d,int e){return 1;} u16 *bgGetGfxPtr(int i){return 0;}
int isDSiMode(void){return 1;}
void consoleInit(PrintConsole*a,int b,int c,int d,int e,int f,int g,int h){} void consoleSelect(PrintConsole*a){} void vramSetBankC(int a){}
void audio_init(void){} void audio_pump(int n){(void)n;} void audio_next(void){} void audio_toggle(void){}
int audio_paused(void){return 0;} const char *audio_title(void){return 0;}
